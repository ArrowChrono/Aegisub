--[=[ \N ! $ ]] ]=]
local hex = 0x1.8p+2
local joined = (3) .. "x"
local cmp = hex >= 6 and joined ~= "3y"
local mix = (2 ^ 3) + (10 % 5)
-- ]
local fraction = .5 + 0x1.fp+3
-- ]=
local minus = 7 - -2
local chained = (function() return function() return 7 end end)()
()
return table.concat({tostring(hex), joined, tostring(cmp), tostring(mix), tostring(fraction), tostring(minus), tostring(chained)}, "|")