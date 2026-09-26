using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Aegisub.GuiAutomation.Driver;

public sealed class AutomationDebugClient : IDisposable
{
    private const int MaxHeaderBytes = 8 * 1024;
    private const int MaxPayloadBytes = 16 * 1024 * 1024;

    private readonly TcpClient client;
    private readonly NetworkStream stream;
    private readonly StreamWriter transcript;
    private readonly List<JsonElement> events = [];
    private int nextSequence = 1;
    private string? token;
    private bool disposed;

    public AutomationDebugClient(int port, string transcriptPath, TimeSpan connectTimeout)
    {
        if (port is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(port));
        if (string.IsNullOrWhiteSpace(transcriptPath))
            throw new ArgumentException("An artifact NDJSON path is required", nameof(transcriptPath));

        client = new TcpClient(AddressFamily.InterNetwork) { NoDelay = true };
        try
        {
            var path = Path.GetFullPath(transcriptPath);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            transcript = new StreamWriter(
                new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read),
                new UTF8Encoding(false));
            using var deadline = NewDeadline(connectTimeout);
            try
            {
                client.ConnectAsync(IPAddress.Loopback, port, deadline.Token)
                    .AsTask().GetAwaiter().GetResult();
            }
            catch (OperationCanceledException error) when (deadline.IsCancellationRequested)
            {
                throw new TimeoutException("DAP loopback connect timed out", error);
            }
            stream = client.GetStream();
        }
        catch
        {
            client.Dispose();
            transcript?.Dispose();
            throw;
        }
    }

    public JsonElement Request(string command, object? arguments, TimeSpan timeout)
    {
        if (string.IsNullOrWhiteSpace(command))
            throw new ArgumentException("A DAP command is required", nameof(command));
        ThrowIfDisposed();
        using var deadline = NewDeadline(timeout);
        try
        {
            return RequestCoreAsync(command, arguments, deadline.Token).GetAwaiter().GetResult();
        }
        catch (OperationCanceledException error) when (deadline.IsCancellationRequested)
        {
            Dispose();
            throw new TimeoutException($"DAP request {command} timed out", error);
        }
    }

    public IReadOnlyList<JsonElement> TakeEvents()
    {
        ThrowIfDisposed();
        var taken = events.ToArray();
        events.Clear();
        return taken;
    }

    public JsonElement WaitEvent(string eventName, TimeSpan timeout)
    {
        if (string.IsNullOrWhiteSpace(eventName))
            throw new ArgumentException("A DAP event name is required", nameof(eventName));
        ThrowIfDisposed();
        using var deadline = NewDeadline(timeout);
        try
        {
            return WaitEventCoreAsync(eventName, deadline.Token).GetAwaiter().GetResult();
        }
        catch (OperationCanceledException error) when (deadline.IsCancellationRequested)
        {
            Dispose();
            throw new TimeoutException($"DAP event {eventName} timed out", error);
        }
    }

    private async Task<JsonElement> RequestCoreAsync(
        string command, object? arguments, CancellationToken cancellationToken)
    {
        var sequence = nextSequence++;
        var request = new Dictionary<string, object?>
        {
            ["seq"] = sequence,
            ["type"] = "request",
            ["command"] = command
        };
        if (arguments is not null)
            request["arguments"] = arguments;
        var payload = JsonSerializer.SerializeToUtf8Bytes(request);
        if (payload.Length > MaxPayloadBytes)
            throw new InvalidDataException("DAP request exceeds the 16 MB payload limit");
        using var document = JsonDocument.Parse(payload);
        token ??= FindToken(document.RootElement);

        var header = Encoding.ASCII.GetBytes($"Content-Length: {payload.Length}\r\n\r\n");
        await stream.WriteAsync(header, cancellationToken);
        await stream.WriteAsync(payload, cancellationToken);
        Record("send", document.RootElement);

        while (true)
        {
            var message = await ReadMessageAsync(cancellationToken);
            var type = message.GetProperty("type").GetString();
            if (type == "event")
            {
                events.Add(message);
                continue;
            }
            if (type != "response")
                throw new InvalidDataException("Unexpected DAP message type while awaiting a response");
            if (message.GetProperty("request_seq").GetInt32() != sequence)
                throw new InvalidDataException("DAP response request_seq does not match the request");
            return message;
        }
    }

    private async Task<JsonElement> WaitEventCoreAsync(string eventName, CancellationToken cancellationToken)
    {
        for (var index = 0; index < events.Count; ++index)
        {
            if (events[index].GetProperty("event").GetString() != eventName)
                continue;
            var cached = events[index];
            events.RemoveAt(index);
            return cached;
        }

        while (true)
        {
            var message = await ReadMessageAsync(cancellationToken);
            if (message.GetProperty("type").GetString() != "event")
                throw new InvalidDataException("Unexpected DAP response while awaiting an event");
            if (message.GetProperty("event").GetString() == eventName)
                return message;
            events.Add(message);
        }
    }

    private async Task<JsonElement> ReadMessageAsync(CancellationToken cancellationToken)
    {
        var header = new byte[MaxHeaderBytes];
        var count = 0;
        while (true)
        {
            if (count == header.Length)
                throw new InvalidDataException("DAP header exceeds the 8 KB limit");
            if (await stream.ReadAsync(header.AsMemory(count, 1), cancellationToken) == 0)
                throw new EndOfStreamException("DAP socket closed while reading the header");
            ++count;
            if (count >= 4 && header[count - 4] == '\r' && header[count - 3] == '\n'
                && header[count - 2] == '\r' && header[count - 1] == '\n')
                break;
        }

        int? length = null;
        foreach (var line in Encoding.ASCII.GetString(header, 0, count - 4).Split("\r\n"))
        {
            var separator = line.IndexOf(':');
            if (separator < 0 || !line[..separator].Trim().Equals("Content-Length", StringComparison.OrdinalIgnoreCase))
                continue;
            if (length is not null || !int.TryParse(line[(separator + 1)..].Trim(),
                    NumberStyles.None, CultureInfo.InvariantCulture, out var parsed))
                throw new InvalidDataException("DAP Content-Length is missing or duplicated");
            length = parsed;
        }
        if (length is null or <= 0 or > MaxPayloadBytes)
            throw new InvalidDataException("DAP Content-Length is outside the 16 MB payload limit");

        var payload = new byte[length.Value];
        await stream.ReadExactlyAsync(payload, cancellationToken);
        using var document = JsonDocument.Parse(payload);
        Record("receive", document.RootElement);
        return document.RootElement.Clone();
    }

    private void Record(string direction, JsonElement message)
    {
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteString("direction", direction);
            writer.WritePropertyName("message");
            WriteRedacted(writer, message);
            writer.WriteEndObject();
        }
        transcript.WriteLine(Encoding.UTF8.GetString(buffer.ToArray()));
        transcript.Flush();
    }

    private void WriteRedacted(Utf8JsonWriter writer, JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Object)
        {
            writer.WriteStartObject();
            foreach (var property in value.EnumerateObject())
            {
                writer.WritePropertyName(property.Name);
                if (property.Name.Equals("token", StringComparison.OrdinalIgnoreCase))
                    writer.WriteStringValue("[redacted]");
                else
                    WriteRedacted(writer, property.Value);
            }
            writer.WriteEndObject();
        }
        else if (value.ValueKind == JsonValueKind.Array)
        {
            writer.WriteStartArray();
            foreach (var item in value.EnumerateArray())
                WriteRedacted(writer, item);
            writer.WriteEndArray();
        }
        else if (value.ValueKind == JsonValueKind.String && token is { Length: > 0 })
        {
            writer.WriteStringValue(value.GetString()!.Replace(token, "[redacted]", StringComparison.Ordinal));
        }
        else
            value.WriteTo(writer);
    }

    private static string? FindToken(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in value.EnumerateObject())
            {
                if (property.Name.Equals("token", StringComparison.OrdinalIgnoreCase)
                    && property.Value.ValueKind == JsonValueKind.String)
                    return property.Value.GetString();
                if (FindToken(property.Value) is { } nested)
                    return nested;
            }
        }
        else if (value.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in value.EnumerateArray())
                if (FindToken(item) is { } nested)
                    return nested;
        }
        return null;
    }

    private static CancellationTokenSource NewDeadline(TimeSpan timeout)
    {
        if (timeout <= TimeSpan.Zero || timeout == Timeout.InfiniteTimeSpan)
            throw new ArgumentOutOfRangeException(nameof(timeout), "A finite positive timeout is required");
        return new CancellationTokenSource(timeout);
    }

    private void ThrowIfDisposed()
    {
        if (disposed)
            throw new ObjectDisposedException(nameof(AutomationDebugClient));
    }

    public void Dispose()
    {
        if (disposed)
            return;
        disposed = true;
        client.Dispose();
        transcript.Dispose();
    }
}
