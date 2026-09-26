local function build(seed)
    local rows = {}
    for i = 1, 4 do
        local acc = 0
        for j = 1, i do
            acc = acc + (seed + i) * j
        end
        rows[i] = { value = acc, name = "r" .. i }
    end
    return function(offset)
        local out = {}
        for _, row in ipairs(rows) do
            out[#out + 1] = row.name .. ":" .. (row.value + offset)
        end
        return table.concat(out, ",")
    end
end
return build(2)(3)