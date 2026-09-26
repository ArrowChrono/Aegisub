script_name = "Verify Lua Workspace persisted source"
script_description = "Checks the saved Workspace scenario through the real Automation host"
script_author = "Aegisub"
script_version = "1"

aegisub.register_macro(script_name, script_description, function(subs)
    local events = {}
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" then
            events[#events + 1] = line
        end
    end
    assert(#events == 3, "Unexpected event count after Workspace Apply")
    assert(events[1].comment and events[1].effect == "code once", "Changed code-line identity")
    assert(not events[1].text:find("[\r\n]"), "Saved coding Text is not one physical line")
    local run = assert(loadstring(events[1].text, "@saved-workspace-code"))
    assert(run() == "38", "Saved source did not produce the independent expected result")
    assert(events[2].comment and events[2].effect == "code syl", "Changed the other code line")
    assert(events[2].text == 'return "other code must remain"', "Overwrote the other code source")
    assert(not events[3].comment and events[3].effect == "", "Changed the ordinary subtitle kind")
    assert(events[3].text == "Ordinary subtitle must remain untouched.", "Overwrote ordinary subtitle text")
    aegisub.debug.out("Workspace persisted source result: 38\n")
end)
