local values = { A = 1, B = 2, C = 3 }
local out = {}
for key, value in pairs(values) do
    local count = 0
    repeat
        count = count + 1
    until count == value
    out[#out + 1] = key .. "=" .. tostring((count + 2) ^ 2)
end
table.sort(out)
return table.concat(out, ";")