local multiplier = 3
function source_b(value)
	local scaled = value * multiplier
	local before_call = scaled + 1
	local result = source_a(before_call)
	return result + value
end
