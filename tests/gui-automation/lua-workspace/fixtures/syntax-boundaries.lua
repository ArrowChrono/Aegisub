local a = 1
---@type string
local b = 'x'
local function unpack_pair(...)
    local first, second = ...
    return first, second
end
local first, second = unpack_pair('雪', 3)
local total
total = 0
for index = 1, 3 do
    total = total + index
end
local record = {
    value = 2;
    calculate = function()
        local count = 1; count = count + 1; return count
    end;
}
function record:run(amount)
    self.value = self.value + amount
    return self.value
end
local done = false
repeat
    done = not done
until done
while not done do total = 99 end
for _, value in ipairs({a, #b}) do
    assert(value == 1)
end
goto finish
total = 100
::finish::
assert(record.calculate() == 2 and -2^2 == -4)
return table.concat({first, second, total, record:run(2)}, ':')
