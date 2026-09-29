local function nested(value)
    local squared = value * value
    return squared + 1
end
local co = coroutine.create(function(seed)
    local value = nested(seed)
    coroutine.yield(value)
    value = nested(value + 1)
    return value
end)
local ok, first = coroutine.resume(co, 3)
assert(ok and first == 10)
local ok_again, second = coroutine.resume(co)
assert(ok_again and second == 122)
return first + second
