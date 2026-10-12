script_name = "Float configuration fixture"

local fields = {"precise", "negative", "tiny", "large", "subnormal", "negative_zero", "maximum", "minimum_normal"}
local expected = {1.2345678901234567, -12.345678901234567, 1e-100, 1e100, 5e-324, -0.0, 1.7976931348623157e308, 2.2250738585072014e-308}

aegisub.register_filter("Float configuration", "Round-trip numerical parameters", 0,
    function(subs, values)
        local expected_values = expected
        if values.legacy then
            expected_values = {1.234568, -12.345679, 1e-100, 1e100, 5e-324, -0.0, 1.7976931348623157e308, 2.2250738585072014e-308}
        end
        for i, name in ipairs(fields) do
            assert(values[name] == expected_values[i], "Restored float differs: " .. name)
        end
        assert(1 / values.negative_zero == -math.huge, "Negative zero sign was lost")
        for i = 1, #subs do
            local line = subs[i]
            if line.class == "dialogue" and not line.comment then
                line.text = values.legacy and "legacy-float-config-executed" or
                    "float-config-executed:" .. string.format("%.17g", values.precise)
                subs[i] = line
                break
            end
        end
    end,
    function()
        local controls = {}
        for i, name in ipairs(fields) do
            controls[i] = {class = "floatedit", name = name, value = 0, x = 0, y = i - 1}
        end
        controls[#fields + 1] = {class = "checkbox", name = "legacy", value = false, x = 0, y = #fields}
        return controls
    end)
