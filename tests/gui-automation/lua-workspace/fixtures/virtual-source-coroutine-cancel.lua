local thread = coroutine.create(function()
	local sum = 0
	for value = 1, 100000 do
		sum = sum + value
		if value == 3 then request_stop() end
	end
	return sum
end)
return thread
