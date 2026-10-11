using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Aegisub.GuiAutomation.Driver;

public static class AutomationTestProfile
{
    public static void WriteMenusWithoutClipboardValidation(string profile, string artifacts)
    {
        var json = new JsonSerializerOptions { WriteIndented = true };
        var menus = JsonNode.Parse(File.ReadAllText("src/libresrc/default_menu.json"))!.AsObject();
        var removed = new List<string>();
        foreach (var entry in menus)
        {
            var items = entry.Value!.AsArray();
            for (var index = items.Count - 1; index >= 0; --index)
            {
                var command = items[index]?["command"]?.GetValue<string>();
                if (command is not ("edit/line/paste" or "edit/line/paste/over")) continue;
                removed.Add(entry.Key + "/" + command);
                items.RemoveAt(index);
            }
        }
        if (removed.Count == 0) throw new InvalidDataException("Clipboard-validation menu entries were not found in the default configuration");
        var menuPath = Path.Combine(profile, "user", "menu.json");
        File.WriteAllText(menuPath, menus.ToJsonString(json));
        File.WriteAllText(Path.Combine(artifacts, "private-profile-settings.json"), JsonSerializer.Serialize(new {
            removed_menu_entries = removed,
            reason = "The normal menu refresh validates every command; paste validators probe the shared window-station clipboard.",
            menu_configuration = Path.GetRelativePath(artifacts, menuPath).Replace('\\', '/'),
            global_input = false, clipboard_operations = false
        }, json));
    }
}
