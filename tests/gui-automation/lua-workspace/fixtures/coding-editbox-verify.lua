script_name = "Coding edit box saved verification"
script_description = "Independently checks persisted event fields and executes the saved Lua"
script_author = "Aegisub"
script_version = "1"

aegisub.register_macro(script_name, script_description, function(subs)
    local events = {}
    for i = 1, #subs do
        if subs[i].class == "dialogue" then events[#events + 1] = subs[i] end
    end
    assert(#events == 5, "Expected exactly five events")
    local a, ordinary, b, template, false_code = unpack(events)
    local function assert_fields(line, comment, layer, start_time, end_time, actor, ml, mr, mt, effect)
        assert(line.comment == comment and line.layer == layer
            and line.start_time == start_time and line.end_time == end_time
            and line.style == "Default" and line.actor == actor
            and line.margin_l == ml and line.margin_r == mr and line.margin_t == mt
            and line.effect == effect, "Saved business fields changed for " .. actor)
    end
    assert_fields(a, true, 1, 1000, 3000, "CodeA", 1, 2, 3, "code once")
    assert(not a.text:find("[\r\n]"), "First Lua source occupies more than one physical ASS line")
    local run = assert(loadstring(a.text, "@coding-editbox-saved"))
    assert(run() == "漢字\nsecond:10", "Saved complex Lua returned the wrong business result")
    assert_fields(ordinary, false, 2, 3000, 5000, "Ordinary", 4, 5, 6, "")
    assert(ordinary.text == "First\\Nsecond {\\i1}subtitle", "Ordinary paste did not keep a literal ASS newline and override")
    assert_fields(b, true, 3, 5000, 7000, "CodeB", 7, 8, 9, "code line")
    assert(b.text == 'return "seed-b"', "Second code source changed")
    assert_fields(template, true, 4, 7000, 9000, "Template", 10, 11, 12, "template line")
    assert(template.text == "template remains", "Template text changed")
    assert_fields(false_code, false, 5, 9000, 11000, "FalseCode", 13, 14, 15, "code once")
    assert(false_code.text == "not a code comment", "False-code dialogue text changed")
    aegisub.debug.out("coding-editbox-result=漢字\\nsecond:10\n")
end)
