script_name = "Lua Workspace runtime outcomes"
script_description = "Exercises real Automation completion, cancellation and rollback"
script_author = "Aegisub"
script_version = "1"

local function provisional_edit(subs)
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" and not line.comment then
            line.text = "This provisional runtime edit must not be committed"
            subs[i] = line
            return
        end
    end
    error("The runtime fixture has no ordinary dialogue")
end

aegisub.register_macro("Workspace Runtime Select Code", script_description, function(subs)
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" and line.comment and line.effect == "code once" then
            return {i}, i
        end
    end
    error("The runtime fixture has no code-once target")
end)

aegisub.register_macro("Workspace Runtime Verify Rollback", script_description, function(subs, selected, active)
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" then
            assert(line.text ~= "This provisional runtime edit must not be committed", "Failed or cancelled macro committed its provisional edit")
        end
    end
    return selected, active
end)

aegisub.register_macro("Workspace Runtime No Output", script_description,
    function(subs, selected, active) return selected, active end,
    function() return true end)

aegisub.register_macro("Workspace Runtime Cancel", script_description, function(subs)
    provisional_edit(subs)
    aegisub.cancel()
end)

aegisub.register_macro("Workspace Runtime Error", script_description, function(subs)
    provisional_edit(subs)
    error("workspace-runtime-error")
end)

aegisub.register_macro("Workspace Runtime Nil Error", script_description, function(subs)
    provisional_edit(subs)
    error(nil)
end)

aegisub.register_macro("Workspace Runtime Caught Cancel", script_description, function(subs, selected, active)
    local ok, value = pcall(aegisub.cancel)
    assert(not ok and value == nil, "The Lua-visible cancellation contract changed")
    return selected, active
end)

aegisub.register_macro("Workspace Runtime Caught Cancel Then Error", script_description, function(subs)
    local ok, value = pcall(aegisub.cancel)
    assert(not ok and value == nil, "The Lua-visible cancellation contract changed")
    provisional_edit(subs)
    error(nil)
end)

aegisub.register_macro("Workspace Runtime Dialog Cancel", script_description, function(subs, selected, active)
    local button = aegisub.dialog.display({
        {class = "label", label = "Cancel only this dialog; the invocation must complete", x = 0, y = 0}
    }, {"Cancel"})
    assert(button == false or button == "Cancel", "Unexpected fixture dialog decision")
    return selected, active
end)

aegisub.register_macro("Workspace Runtime Hidden Acknowledgement", script_description, function(subs, selected, active)
    local button = aegisub.dialog.display({
        {class = "label", label = "The hidden Workspace invocation has started", x = 0, y = 0}
    }, {"OK"})
    assert(button == "OK", "Hidden invocation acknowledgement was not accepted")
    return selected, active
end)

aegisub.register_macro("Workspace Runtime Progress Cancel", script_description, function(subs)
    provisional_edit(subs)
    local deadline = os.time() + 30
    aegisub.progress.task("Waiting for the bounded runtime cancellation scenario")
    while os.time() < deadline do
        if aegisub.progress.is_cancelled() then aegisub.cancel() end
    end
    error("The runtime cancellation scenario was not cancelled within thirty seconds")
end)

local prepared_events
local event_fields = {"comment", "layer", "start_time", "end_time", "style", "actor", "margin_l", "margin_r", "margin_t", "margin_b", "effect", "text"}
local template_failures = {
    {name = "Code Parse", effect = "code once", text = "decorate = function(value) return value end; local broken ="},
    {name = "Code Runtime", effect = "code once", text = "_G.error(\"workspace-code-runtime\")"},
    {name = "Expression Parse", effect = "template line notext loop 2", text = "L!j!:!(1 +)!;"},
    {name = "Expression Runtime", effect = "template line notext loop 2", text = "L!j!:!_G.error(\"workspace-expression-runtime\")!;"}
}

for _, failure in ipairs(template_failures) do
    aegisub.register_macro("Workspace Runtime Prepare " .. failure.name, script_description, function(subs, selected, active)
        local changed = false
        for i = 1, #subs do
            local line = subs[i]
            if line.class == "dialogue" and line.comment and line.effect == failure.effect then
                assert(not changed, "Ambiguous failure preparation target")
                line.text = failure.text
                subs[i] = line
                changed = true
            end
        end
        assert(changed, "Failure preparation target was not found")
        prepared_events = {}
        for i = 1, #subs do
            local line = subs[i]
            if line.class == "dialogue" then
                prepared_events[#prepared_events + 1] = line
            end
        end
        return selected, active
    end)
end

aegisub.register_macro("Workspace Runtime Verify Prepared Rollback", script_description, function(subs, selected, active)
    assert(prepared_events, "No prepared template failure baseline")
    local count = 0
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" then
            count = count + 1
            local expected = assert(prepared_events[count], "Failed template added an event")
            for _, field in ipairs(event_fields) do
                assert(line[field] == expected[field], "Failed template changed event " .. count .. " field " .. field)
            end
        end
    end
    assert(count == #prepared_events, "Failed template removed an event")
    return selected, active
end)
