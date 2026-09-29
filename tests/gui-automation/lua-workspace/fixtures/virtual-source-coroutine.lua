local bias = 11
local function transform(value)
	local scaled = value * 2
	local result = scaled + bias
	coroutine.yield(result)
	return result + 5
end
local thread = coroutine.create(function()
	local total = 0
	for value = 1, 3 do
		total = total + transform(value)
	end
	return total
end)
local outputs = {}
for index = 1, 4 do
	local ok, result = coroutine.resume(thread)
	assert(ok, result)
	outputs[index] = result
end
assert(coroutine.status(thread) == "dead")
return table.concat(outputs, ","), "complete"
