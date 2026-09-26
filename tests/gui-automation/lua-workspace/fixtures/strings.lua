local text = "雪" .. [==[中
! $ \N ]==] .. "A\
B\0Z"
local bytes = {}
for i = 1, #text do
    bytes[i] = string.format("%02X", string.byte(text, i))
end
return table.concat(bytes, "")