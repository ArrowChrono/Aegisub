local values = {"\255", "\128\129", "汉字\255", "\000\255", "\240\159\153\130", "\192\128\237\160\128\244\144\128\128\226\130"}
local encoded = {}
for index, value in ipairs(values) do
    local bytes = {}
    for offset = 1, #value do
        bytes[#bytes + 1] = string.format("%02X", string.byte(value, offset))
    end
    encoded[index] = table.concat(bytes)
end
return table.concat(encoded, "|")
