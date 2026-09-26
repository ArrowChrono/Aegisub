local seed = 7
local function make_worker(offset)
	local bias = seed + offset
	return function(value)
		local adjusted = value + bias
		local doubled = adjusted * 2
		return doubled + 3
	end
end
local worker = make_worker(5)
function source_a(value)
	local from_worker = worker(value)
	return from_worker + 1
end
