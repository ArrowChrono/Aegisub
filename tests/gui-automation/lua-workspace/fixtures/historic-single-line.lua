local output = {} local total = 0 local values = { 2, 3, 5 } for index, value in ipairs(values) do total = total + value output[#output + 1] = tostring(total) end return table.concat(output, ",")
