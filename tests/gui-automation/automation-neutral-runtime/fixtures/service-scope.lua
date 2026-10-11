local clipboard = require("aegisub.clipboard")
local host_module = require("neutral_host")

script_name = aegisub.gettext("scope")
script_version = "1"

aegisub.register_macro("Services/Read own scope", "Per-window runtime services", function(subs, selected)
    local previous = clipboard.get()
    local translated = aegisub.gettext("run")
    assert(clipboard.set(host_module.identity .. ":" .. translated))
    local line = subs[selected[1]]
    line.text = table.concat({script_name, host_module.identity, previous, translated, clipboard.get()}, "|")
    subs[selected[1]] = line
    return selected, selected[1]
end)
