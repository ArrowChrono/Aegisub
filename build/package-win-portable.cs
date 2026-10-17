#:package System.CommandLine@2.0.5
#:package Spectre.Console@0.54.0
#:package SharpCompress@0.48.0
#:property PublishAot=true

using System.CommandLine;
using System.Diagnostics;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Text.Json.Serialization;
using SharpCompress.Archives;
using SharpCompress.Common;
using SharpCompress.Readers;
using Spectre.Console;

var repoRoot = Directory.GetCurrentDirectory();

var modeOption = new Option<string>("--mode")
{
    Description = "prepare-dependency or package",
    Required = true,
    CustomParser = result =>
    {
        var value = result.Tokens.Single().Value;
        if (value is not ("prepare-dependency" or "package"))
            result.AddError("--mode must be prepare-dependency or package.");
        return value;
    }
};

var buildDirOption = new Option<string?>("--build-dir")
{
    Description = "Build output directory used by package mode."
};

var outputDirOption = new Option<string>("--output-dir")
{
    Description = "Output directory.",
    DefaultValueFactory = _ => Path.Combine("artifacts", "win-portable")
};

var cacheDirOption = new Option<string>("--cache-dir")
{
    Description = "Download and extraction cache.",
    DefaultValueFactory = _ => Path.Combine(".cache", "package-win-portable")
};

var specOption = new Option<string>("--spec")
{
    Description = "Packaging spec json.",
    DefaultValueFactory = _ => Path.Combine("build", "package-win-portable.thirdparty.json")
};

var packageNameOption = new Option<string>("--package-name")
{
    Description = "Package directory and zip name.",
    DefaultValueFactory = _ => "aegisub-portable"
};

var refreshOption = new Option<bool>("--refresh")
{
    Description = "Redownload, reextract, or reclone cached sources."
};

var skipZipOption = new Option<bool>("--skip-zip")
{
    Description = "Skip zip creation in package mode."
};

var rootCommand = new RootCommand("Prepare dependencies and build a Windows portable package.");
rootCommand.Options.Add(modeOption);
rootCommand.Options.Add(buildDirOption);
rootCommand.Options.Add(outputDirOption);
rootCommand.Options.Add(cacheDirOption);
rootCommand.Options.Add(specOption);
rootCommand.Options.Add(packageNameOption);
rootCommand.Options.Add(refreshOption);
rootCommand.Options.Add(skipZipOption);

rootCommand.SetAction(async parseResult =>
{
    var mode = parseResult.GetValue(modeOption)!;
    var buildDir = parseResult.GetValue(buildDirOption);
    var outputDir = parseResult.GetValue(outputDirOption)!;
    var cacheDir = parseResult.GetValue(cacheDirOption)!;
    var specPath = parseResult.GetValue(specOption)!;
    var packageName = parseResult.GetValue(packageNameOption)!;
    var refresh = parseResult.GetValue(refreshOption);
    var skipZip = parseResult.GetValue(skipZipOption);

    var app = await PackageTool.CreateAsync(
        repoRoot,
        Resolve(specPath),
        Resolve(cacheDir),
        Resolve(outputDir),
        buildDir is null ? null : Resolve(buildDir),
        packageName,
        refresh,
        skipZip);

    await app.RunAsync(mode);
});

return await rootCommand.Parse(args).InvokeAsync();

string Resolve(string path) =>
    Path.GetFullPath(Path.IsPathRooted(path) ? path : Path.Combine(repoRoot, path));

sealed class PackageTool(
    string repoRoot,
    Spec spec,
    string cacheRoot,
    string outputRoot,
    string? buildRoot,
    string packageName,
    bool refresh,
    bool skipZip)
{
    static readonly HttpClient Http = new();

    readonly string downloadsRoot = Path.Combine(cacheRoot, "downloads");
    readonly string sourcesRoot = Path.Combine(cacheRoot, "sources");
    readonly List<string> warnings = [];

    public static async Task<PackageTool> CreateAsync(string repoRoot, string specPath, string cacheRoot, string outputRoot, string? buildRoot, string packageName, bool refresh, bool skipZip)
    {
        await using var stream = File.OpenRead(specPath);
        var spec = await JsonSerializer.DeserializeAsync(stream, PackageJsonContext.Default.Spec)
            ?? throw new InvalidOperationException($"Failed to read spec: {specPath}");

        return new PackageTool(repoRoot, spec, cacheRoot, outputRoot, buildRoot, packageName, refresh, skipZip);
    }

    public async Task RunAsync(string mode)
    {
        Directory.CreateDirectory(downloadsRoot);
        Directory.CreateDirectory(sourcesRoot);

        if (mode == "prepare-dependency")
        {
            Log("mode", "prepare-dependency", "green");
            await PrepareExternalSourcesAsync(spec.Dependency);
            CopyItems(spec.Dependency, repoRoot);
            ShowWarnings();
            return;
        }

        if (buildRoot is null)
            throw new InvalidOperationException("--build-dir is required in package mode.");

        Log("mode", "package", "green");
        await PrepareExternalSourcesAsync(spec.Dependency.Concat(spec.Package));

        Directory.CreateDirectory(outputRoot);
        var stageDir = Path.Combine(outputRoot, packageName);
        if (Directory.Exists(stageDir))
            Directory.Delete(stageDir, true);
        Directory.CreateDirectory(stageDir);

        CopyItems(spec.Package, stageDir);

        if (!skipZip)
        {
            var zipPath = Path.Combine(outputRoot, $"{packageName}.zip");
            if (File.Exists(zipPath))
                File.Delete(zipPath);

            Log("zip", zipPath, "yellow");
            ZipFile.CreateFromDirectory(stageDir, zipPath, CompressionLevel.Optimal, false);
        }

        ShowWarnings();
    }

    async Task PrepareExternalSourcesAsync(IEnumerable<SourceItem> items)
    {
        foreach (var item in items.Where(NeedsPrepare).DistinctBy(item => $"{item.SourceType}:{item.Name}"))
            await PrepareExternalSourceAsync(item);
    }

    async Task PrepareExternalSourceAsync(SourceItem item)
    {
        var name = Need(item.Name, $"Missing name for {item.SourceType} item.");
        var sourceDir = Path.Combine(sourcesRoot, name);

        if (refresh && Directory.Exists(sourceDir))
            Directory.Delete(sourceDir, true);

        if (Directory.Exists(sourceDir))
        {
            Log("cache", name, "grey");
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(sourceDir)!);

        switch (item.SourceType)
        {
            case "archive":
                var archivePath = await DownloadAsync(name, Need(item.Url, $"Missing url for archive '{name}'."));
                await ExtractArchiveAsync(name, archivePath, sourceDir);
                break;

            case "git":
                CloneGit(Need(item.Url, $"Missing url for git '{name}'."), sourceDir);
                break;
        }
    }

    async Task<string> DownloadAsync(string name, string url)
    {
        var fileName = Path.GetFileName(new Uri(url).AbsolutePath);
        if (string.IsNullOrWhiteSpace(fileName))
            fileName = name;

        var archivePath = Path.Combine(downloadsRoot, fileName);

        if (refresh && File.Exists(archivePath))
            File.Delete(archivePath);

        if (File.Exists(archivePath))
        {
            Log("download", $"{name} -> {archivePath}", "grey");
            return archivePath;
        }

        Log("download", $"{name} -> {url}", "yellow");
        var tempPath = archivePath + ".part";
        if (File.Exists(tempPath))
            File.Delete(tempPath);

        using var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
        response.EnsureSuccessStatusCode();

        var totalBytes = response.Content.Headers.ContentLength;
        await using var input = await response.Content.ReadAsStreamAsync();
        try
        {
            await using var output = File.Create(tempPath);
            var progress = new CheckpointProgress(percent => Log("progress", $"download {name} {percent}%", "grey"));

            if (totalBytes is > 0)
            {
                await using var progressInput = new ProgressReadStream(input, totalBytes.Value, progress);
                await progressInput.CopyToAsync(output);
                progress.Complete();
            }
            else
            {
                await input.CopyToAsync(output);
                Log("progress", $"download {name} done", "grey");
            }

            await output.FlushAsync();
        }
        catch
        {
            if (File.Exists(tempPath))
                File.Delete(tempPath);
            throw;
        }

        File.Move(tempPath, archivePath, true);

        return archivePath;
    }

    void CopyItems(IEnumerable<SourceItem> items, string targetRoot)
    {
        foreach (var item in items)
        {
            var sourceRoot = ResolveSourceRoot(item);
            Log("copy", $"{item.SourceType}:{item.Name ?? "-"}", "blue");

            foreach (var rule in item.Paths)
                CopyRule(item, sourceRoot, targetRoot, rule);
        }
    }

    string ResolveSourceRoot(SourceItem item) => item.SourceType switch
    {
        "repo" => repoRoot,
        "build" => buildRoot ?? throw new InvalidOperationException("--build-dir is required in package mode."),
        "archive" or "git" => Path.Combine(sourcesRoot, Need(item.Name, $"Missing name for {item.SourceType} item.")),
        _ => throw new InvalidOperationException($"Unsupported sourceType: {item.SourceType}")
    };

    void CopyRule(SourceItem item, string sourceRoot, string targetRoot, PathRule rule)
    {
        try
        {
            if (rule.Source.EndsWith("/*", StringComparison.Ordinal) && rule.Target.EndsWith("/*", StringComparison.Ordinal))
            {
                var sourceDir = Join(sourceRoot, rule.Source[..^2]);
                var targetDir = Join(targetRoot, rule.Target[..^2]);
                if (!Directory.Exists(sourceDir))
                {
                    Missing($"Missing directory: {sourceDir}", rule.Required);
                    return;
                }

                Directory.CreateDirectory(targetDir);

                foreach (var file in Directory.EnumerateFiles(sourceDir, "*", SearchOption.AllDirectories))
                {
                    var relative = Path.GetRelativePath(sourceDir, file);
                    CopyFile(file, Path.Combine(targetDir, relative));
                }
                return;
            }

            if (rule.Source.Contains('*') || rule.Source.Contains('?'))
            {
                var sourceDir = Join(sourceRoot, Path.GetDirectoryName(Normalize(rule.Source)) ?? "");
                if (!Directory.Exists(sourceDir))
                {
                    Missing($"Missing directory: {sourceDir}", rule.Required);
                    return;
                }

                var files = Directory.EnumerateFiles(sourceDir, Path.GetFileName(rule.Source), SearchOption.TopDirectoryOnly).ToArray();
                if (files.Length == 0)
                {
                    Missing($"No files matched: {Path.Combine(sourceDir, rule.Source)}", rule.Required);
                    return;
                }

                var targetDir = Join(targetRoot, Path.GetDirectoryName(Normalize(rule.Target)) ?? "");
                Directory.CreateDirectory(targetDir);

                foreach (var file in files)
                    CopyFile(file, Path.Combine(targetDir, Path.GetFileName(file)));
                return;
            }

            var sourceFile = Join(sourceRoot, rule.Source);
            if (!File.Exists(sourceFile))
            {
                Missing($"Missing file: {sourceFile}", rule.Required);
                return;
            }

            CopyFile(sourceFile, Join(targetRoot, rule.Target));
        }
        catch (Exception ex)
        {
            var name = item.Name ?? "-";
            throw new InvalidOperationException($"Copy failed for {item.SourceType}:{name} {rule.Source} -> {rule.Target}{Environment.NewLine}{ex.Message}", ex);
        }
    }

    static void CopyFile(string sourceFile, string targetFile)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(targetFile)!);
        File.Copy(sourceFile, targetFile, true);
    }

    async Task ExtractArchiveAsync(string name, string archivePath, string targetDir)
    {
        Log("extract", archivePath, "yellow");

        var tempDir = targetDir + ".tmp";
        if (Directory.Exists(tempDir))
            Directory.Delete(tempDir, true);
        Directory.CreateDirectory(tempDir);

        try
        {
            if (Path.GetExtension(archivePath).Equals(".7z", StringComparison.OrdinalIgnoreCase))
                await ExtractWithArchiveAsync(name, archivePath, tempDir);
            else
                await ExtractWithReaderAsync(name, archivePath, tempDir);
        }
        catch
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, true);
            throw;
        }

        if (Directory.Exists(targetDir))
            Directory.Delete(targetDir, true);
        Directory.Move(tempDir, targetDir);
    }

    async Task ExtractWithReaderAsync(string name, string archivePath, string targetDir)
    {
        var totalBytes = new FileInfo(archivePath).Length;
        var progress = new CheckpointProgress(percent => Log("progress", $"extract {name} {percent}%", "grey"));
        await using var stream = new FileStream(archivePath, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 256, FileOptions.Asynchronous | FileOptions.SequentialScan);
        await using var progressStream = new ProgressReadStream(stream, totalBytes, progress);
        await using var reader = await ReaderFactory.OpenAsyncReader(progressStream, new ReaderOptions(), CancellationToken.None);
        await reader.WriteAllToDirectoryAsync(targetDir, new ExtractionOptions { ExtractFullPath = true, Overwrite = true }, CancellationToken.None);
        progress.Complete();
    }

    async Task ExtractWithArchiveAsync(string name, string archivePath, string targetDir)
    {
        var progress = new CheckpointProgress(percent => Log("progress", $"extract {name} {percent}%", "grey"));
        var archiveProgress = new Progress<ProgressReport>(report =>
        {
            if (report.PercentComplete is { } percent)
                progress.ReportPercent(percent);
            else if (report.TotalBytes is > 0)
                progress.Report(report.BytesTransferred, report.TotalBytes.Value);
        });

        await using var archive = await ArchiveFactory.OpenAsyncArchive(archivePath, new ReaderOptions(), CancellationToken.None);
        await archive.WriteToDirectoryAsync(targetDir, new ExtractionOptions { ExtractFullPath = true, Overwrite = true }, archiveProgress, CancellationToken.None);
        progress.Complete();
    }

    static void CloneGit(string url, string targetDir)
    {
        Log("clone", url, "yellow");
        Run("git", ["clone", "--depth", "1", url, targetDir]);
    }

    static void Run(string fileName, IEnumerable<string> arguments)
    {
        var startInfo = new ProcessStartInfo(fileName)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };

        foreach (var argument in arguments)
            startInfo.ArgumentList.Add(argument);

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Failed to start: {fileName}");

        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit();

        if (process.ExitCode != 0)
            throw new InvalidOperationException($"{fileName} failed with exit code {process.ExitCode}.{Environment.NewLine}{stdout}{stderr}");
    }

    static bool NeedsPrepare(SourceItem item) =>
        (item.SourceType == "archive" || item.SourceType == "git") &&
        !string.IsNullOrWhiteSpace(item.Url);

    static string Need(string? value, string message) =>
        string.IsNullOrWhiteSpace(value) ? throw new InvalidOperationException(message) : value;

    static string Join(string root, string relative) =>
        Path.Combine(root, Normalize(relative));

    static string Normalize(string path) =>
        path.Replace('/', Path.DirectorySeparatorChar);

    static void Log(string label, string value, string color) =>
        AnsiConsole.MarkupLine($"[{color}]{Markup.Escape(label)}[/] {Markup.Escape(value)}");

    void Missing(string message, bool required)
    {
        if (required)
            throw new FileNotFoundException(message);

        warnings.Add(message);
        Log("warn", message, "yellow");
    }

    void ShowWarnings()
    {
        if (warnings.Count > 0)
            Log("warn", $"{warnings.Count} optional path(s) were missing.", "yellow");
    }
}

sealed class Spec
{
    [JsonPropertyName("dependency")]
    public SourceItem[] Dependency { get; init; } = [];

    [JsonPropertyName("package")]
    public SourceItem[] Package { get; init; } = [];
}

sealed class SourceItem
{
    [JsonPropertyName("sourceType")]
    public string SourceType { get; init; } = "";

    [JsonPropertyName("name")]
    public string? Name { get; init; }

    [JsonPropertyName("url")]
    public string? Url { get; init; }

    [JsonPropertyName("path")]
    public PathRule[] Paths { get; init; } = [];
}

sealed class PathRule
{
    [JsonPropertyName("source")]
    public string Source { get; init; } = "";

    [JsonPropertyName("target")]
    public string Target { get; init; } = "";

    [JsonPropertyName("required")]
    public bool Required { get; init; }
}

[JsonSourceGenerationOptions(PropertyNameCaseInsensitive = true)]
[JsonSerializable(typeof(Spec))]
partial class PackageJsonContext : JsonSerializerContext;

sealed class ProgressReadStream(Stream inner, long totalBytes, CheckpointProgress progress) : Stream
{
    long transferred;

    public override bool CanRead => inner.CanRead;
    public override bool CanSeek => inner.CanSeek;
    public override bool CanWrite => inner.CanWrite;
    public override long Length => inner.Length;
    public override long Position { get => inner.Position; set => inner.Position = value; }

    public override void Flush() => inner.Flush();
    public override Task FlushAsync(CancellationToken cancellationToken) => inner.FlushAsync(cancellationToken);
    public override long Seek(long offset, SeekOrigin origin) => inner.Seek(offset, origin);
    public override void SetLength(long value) => inner.SetLength(value);
    public override int Read(byte[] buffer, int offset, int count)
    {
        var read = inner.Read(buffer, offset, count);
        Report(read);
        return read;
    }

    public override int Read(Span<byte> buffer)
    {
        var read = inner.Read(buffer);
        Report(read);
        return read;
    }

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        var read = await inner.ReadAsync(buffer, cancellationToken);
        Report(read);
        return read;
    }

    public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) =>
        ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

    public override void Write(byte[] buffer, int offset, int count) => inner.Write(buffer, offset, count);
    public override void Write(ReadOnlySpan<byte> buffer) => inner.Write(buffer);
    public override Task WriteAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) =>
        inner.WriteAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();
    public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default) =>
        inner.WriteAsync(buffer, cancellationToken);

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            inner.Dispose();
        base.Dispose(disposing);
    }

    public override async ValueTask DisposeAsync()
    {
        await inner.DisposeAsync();
        await base.DisposeAsync();
    }

    void Report(int read)
    {
        if (read <= 0 || totalBytes <= 0)
            return;

        transferred += read;
        progress.Report(Math.Min(transferred, totalBytes), totalBytes);
    }
}

sealed class CheckpointProgress(Action<int> onReached)
{
    int next = 25;

    public void Report(long current, long total)
    {
        if (total > 0)
            ReportPercent(current * 100.0 / total);
    }

    public void ReportPercent(double percent)
    {
        while (next <= 100 && percent >= next)
        {
            onReached(next);
            next += 25;
        }
    }

    public void Complete() => ReportPercent(100);
}
