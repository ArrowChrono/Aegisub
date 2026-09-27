local values = {2 ; 3 ; label = "雪 ; 🌟" ;}
local total = 0 ; local log = {}
for _, value in ipairs(values) do
    total = total + value ; log[#log + 1] = tostring(value) ;
end
local function choose(value)
    return function() return value end
end
local callback = choose(7) ; (function() total = total + callback() end)() ;
-- punctuation ; stays inside this comment
--[=[long comment ; punctuation]=]
local literal = [=[文 ; 本]=] ;
return table.concat({tostring(total), table.concat(log, ","), values.label, literal}, "|") ;
