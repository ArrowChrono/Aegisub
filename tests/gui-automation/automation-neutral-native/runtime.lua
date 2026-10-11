script_name = "Native neutral runtime"
script_description = "Real macro, dialog, progress, selection, history and export fixture"
script_author = "Aegisub"
script_version = "1"

local function events(subs)
    local result = {}
    for index = 1, #subs do
        if subs[index].class == "dialogue" then result[#result + 1] = index end
    end
    assert(#result == 4, "Fixture must contain exactly four events")
    return result
end

local function accepted(subs)
    return subs[events(subs)[1]].effect == "neutral-accepted"
end

local function acknowledge(text)
    local button = aegisub.dialog.display({
        {class = "label", label = text, x = 0, y = 0}
    }, {"Acknowledge"}, {ok = "Acknowledge"})
    assert(button == "Acknowledge", "Acknowledgement was not accepted")
end

local function provisional(subs)
    local index = events(subs)[1]
    local line = subs[index]
    line.text = "PROVISIONAL: cancellation and failure must discard this text"
    line.actor = "provisional-actor"
    line.start_time = 850
    subs[index] = line
    aegisub.set_undo_point("Native provisional history must be discarded")
    local tail = subs[events(subs)[4]]
    tail.layer = 99
    tail.effect = "provisional-tail"
    subs[events(subs)[4]] = tail
end

aegisub.register_macro("Native Neutral Accept", script_description, function(subs)
    local button, values = aegisub.dialog.display({
        {class = "label", label = "Native mixed-control contract", x = 0, y = 0, width = 3},
        {class = "edit", name = "prefix", text = "prefix-seed", x = 0, y = 1, width = 3},
        {class = "textbox", name = "detail", text = "detail-seed", x = 0, y = 2, width = 3, height = 2},
        {class = "intedit", name = "offset", value = 137, min = -500, max = 1000, x = 0, y = 4},
        {class = "floatedit", name = "factor", value = 1.25, min = 0, max = 5, x = 1, y = 4},
        {class = "dropdown", name = "mode", items = {"alpha", "beta", "gamma"}, value = "beta", x = 2, y = 4},
        {class = "checkbox", name = "bold", label = "Use bold layer", value = false, x = 0, y = 5},
        {class = "coloralpha", name = "tint", value = "&H80332211&", x = 1, y = 5},
        {class = "alpha", name = "opacity", value = "#3F", x = 2, y = 5}
    }, {"Apply changes", "Cancel"}, {ok = "Apply changes", cancel = "Cancel"})
    assert(button == "Apply changes", "Acceptance case did not choose Apply changes")
    assert(values.prefix == "Accepted café, 世界", "UTF-8 edit value was not returned")
    local detail = values.detail:gsub("\r\n", "\n")
    assert(detail == "line one\nline two", "Multiline edit value was not returned")
    assert(type(values.offset) == "number" and values.offset == 250, "Integer edit value was not returned")
    assert(type(values.factor) == "number" and values.factor == 1.75, "Float edit value was not returned")
    assert(values.mode == "beta", "Dropdown value was not returned")
    assert(values.bold == true, "Checkbox value was not returned as true")
    assert(values.tint == "#11223380", "RGBA color value was not returned")
    assert(values.opacity == "#7F", "Alpha edit value was not returned")
    local indices = events(subs)
    local first = subs[indices[1]]
    first.text = "{\\b1}" .. values.prefix .. "\\N" .. detail:gsub("\n", "\\N") .. "|" .. values.mode .. "|" .. values.tint .. "|" .. values.opacity
    first.actor = "native-actor"
    first.layer = values.bold and 4 or 0
    first.start_time = first.start_time + values.offset
    first.end_time = first.end_time + values.offset
    first.effect = "neutral-accepted"
    subs[indices[1]] = first
    local active = subs[indices[3]]
    active.layer = math.floor(values.factor * 4)
    active.effect = "neutral-active"
    subs[indices[3]] = active
    aegisub.set_undo_point("Native neutral accept")
    return {indices[1], indices[3]}, indices[3]
end)

aegisub.register_macro("Native Neutral Validate", "Original validation help", function(subs, selected, active)
    local indices = events(subs)
    assert(#selected == 2 and selected[1] == indices[1] and selected[2] == indices[3], "Accepted macro did not publish the exact selected rows")
    assert(active == indices[3] and subs[active].effect == "neutral-active", "Accepted macro did not publish the active row")
    acknowledge("Native validation and selection verified: rows 1,3; active 3")
    return selected, active
end, function(subs)
    if accepted(subs) then return true, "Native accepted validation help" end
    return false, "Native baseline validation help"
end)

aegisub.register_macro("Native Neutral Toggle", script_description,
    function(subs, selected, active) return selected, active end,
    function() return true end,
    function(subs) return accepted(subs) end)

aegisub.register_macro("Native Neutral Dialog Cancel", script_description, function(subs)
    provisional(subs)
    local button = aegisub.dialog.display({
        {class = "label", label = "Cancel staged subtitle and undo changes", x = 0, y = 0},
        {class = "edit", name = "discard", text = "cancel-seed", x = 0, y = 1}
    }, {"Commit unexpected changes", "Cancel"}, {cancel = "Cancel"})
    assert(button == false, "Cancel button did not return false")
    aegisub.cancel()
end)

aegisub.register_macro("Native Neutral Progress Cancel", script_description, function(subs)
    provisional(subs)
    aegisub.progress.title("Native progress cancellation")
    aegisub.progress.task("Staged changes are waiting for explicit cancellation")
    aegisub.progress.set(35)
    local deadline = os.time() + 25
    while os.time() < deadline do
        if aegisub.progress.is_cancelled() then aegisub.cancel() end
    end
    error("Native progress was not cancelled within twenty-five seconds")
end)

aegisub.register_macro("Native Neutral Runtime Failure", script_description, function(subs)
    provisional(subs)
    error("native-neutral-runtime-failure", 0)
end)

aegisub.register_macro("Native Neutral Verify Baseline", script_description, function(subs, selected, active)
    local indices = events(subs)
    assert(subs[indices[1]].text == "{\\i1}café, 世界" and subs[indices[1]].actor == "first", "Baseline first event changed")
    assert(subs[indices[4]].layer == 6 and subs[indices[4]].effect == "keep", "Baseline tail event changed")
    acknowledge("Native baseline Lua inspection completed")
    return selected, active
end)

aegisub.register_filter("Native Neutral Export", "Native export configuration and copy isolation", 2000,
    function(subs, config)
        assert(config.prefix == "EXPORT:|,# café ", "Export did not receive the accepted UTF-8 and delimiter-rich prefix")
        assert(config.increment == 3 and config.enabled == true, "Export did not receive accepted typed settings")
        for _, index in ipairs(events(subs)) do
            local line = subs[index]
            if not line.comment then
                line.text = config.prefix .. line.text
                line.layer = line.layer + config.increment
                subs[index] = line
            end
        end
    end,
    function()
        return {
            {class = "label", label = "Native export settings", x = 0, y = 0, width = 2},
            {class = "edit", name = "prefix", text = "export-seed", x = 0, y = 1, width = 2},
            {class = "intedit", name = "increment", value = 11, min = 0, max = 20, x = 0, y = 2},
            {class = "checkbox", name = "enabled", label = "Apply native export", value = false, x = 1, y = 2}
        }
    end)
