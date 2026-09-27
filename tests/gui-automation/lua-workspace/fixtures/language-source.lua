local object = { ["dot-key"] = 7, count = 3 }
function object:scale(value)
    return self.count * value
end
local emoji = "漢😀"; local candidate = object.count
local called = object:scale(4)
local result = candidate + called
return emoji .. ":" .. result
