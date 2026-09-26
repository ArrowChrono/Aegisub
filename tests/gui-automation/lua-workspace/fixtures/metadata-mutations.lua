script_name = "Lua Workspace metadata E2E"
script_description = "Controlled external edits through the normal Automation host"
script_author = "Aegisub"
script_version = "1"

local function active_dialogue(subs, active)
    assert(type(active) == "number" and active >= 1 and active <= #subs, "No active fixture line")
    local line = subs[active]
    assert(line.class == "dialogue", "The active fixture entry is not dialogue")
    assert(line.effect:match("^code"), "The selected fixture line is not a code target")
    return line
end

local function replace(description, transform)
    return function(subs, selected, active)
        local line = active_dialogue(subs, active)
        transform(line, subs)
        subs[active] = line
        aegisub.set_undo_point(description)
        return selected, active
    end
end

aegisub.register_macro("Workspace E2E Change Effect", script_description,
    replace("Change fixture Effect", function(line)
        line.effect = line.effect == "code once" and "code line" or "code once"
    end))

aegisub.register_macro("Workspace E2E Change Style", script_description,
    replace("Change fixture Style", function(line, subs)
        local found = false
        for i = 1, #subs do
            local entry = subs[i]
            if entry.class == "style" and entry.name == "Alt" then found = true end
        end
        assert(found, "The fixture must define the Alt style")
        line.style = line.style == "Default" and "Alt" or "Default"
    end))

aegisub.register_macro("Workspace E2E Toggle Comment", script_description,
    replace("Toggle fixture Comment", function(line)
        line.comment = not line.comment
    end))

aegisub.register_macro("Workspace E2E Delete Target", script_description, function(subs, selected, active)
    active_dialogue(subs, active)
    assert(active < #subs and subs[active + 1].class == "dialogue", "The fixture needs a following dialogue")
    subs.delete(active)
    aegisub.set_undo_point("Delete fixture target")
    return {active}, active
end)

local function select_code(ordinal)
    return function(subs)
        local count = 0
        for i = 1, #subs do
            local line = subs[i]
            if line.class == "dialogue" and line.effect:match("^code") then
                count = count + 1
                if count == ordinal then return {i}, i end
            end
        end
        error("The requested fixture code line does not exist")
    end
end

aegisub.register_macro("Workspace E2E Select First Code", script_description, select_code(1))
aegisub.register_macro("Workspace E2E Select Second Code", script_description, select_code(2))
