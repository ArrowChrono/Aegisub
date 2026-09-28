local output = {}
local function append(value)
    output[#output + 1] = value
end
local config = {
    prefix = "雪",
    values = {
        2,
        3,
        5
    }
}
local long_text = [=[left
right]=]
--[=[ a long comment
whose contents must not become statements ]=]
for index, value in ipairs(config.values) do
    local adjusted =
        config.values[index] *
        (index + 1)
    if adjusted > 8 then
        append("big:" .. config.prefix .. adjusted)
    else
        append(config.prefix .. adjusted)
    end
end
append((long_text:gsub("\n", "/")))
return table.concat(output, "|")
