local function increment(value)
    return value + 1
end
local index = 0
while index < 8 do index = increment(index) end
return index
