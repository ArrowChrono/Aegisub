local total = 0
local results = {}
for i = 1, 4 do
	local result = source_b(i)
	results[i] = result
	total = total + result
end
return total, table.concat(results, ",")
