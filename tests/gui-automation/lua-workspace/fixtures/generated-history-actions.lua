script_name = "Lua Workspace generated history actions"
script_description = "Select source and verify cancelled output rollback"
script_author = "Aegisub"
script_version = "1"

aegisub.register_macro("Generated History Select Code", script_description, function(subs)
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" and line.comment and line.effect == "code once" then
            return {i}, i
        end
    end
    error("Code source not found")
end)

aegisub.register_macro("Generated History Verify Rollback", script_description, function(subs, selected, active)
    local count = 0
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" then
            count = count + 1
            assert(line.effect ~= "fx", "Cancelled generated line remained in live ASS")
            if not line.comment then
                assert(line.text == "{\\k100}x" and line.start_time == 1000 and line.end_time == 2000,
                    "Cancelled macro changed the original live ASS dialogue")
            end
        end
    end
    assert(count == 3, "Cancelled macro changed the live ASS event count")
    return selected, active
end)
