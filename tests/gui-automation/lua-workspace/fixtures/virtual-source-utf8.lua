local ascii_cjk = string.rep("A", 114) .. "中" .. "tail"
local emoji = string.rep("B", 113) .. "🙂" .. "tail"
local invalid = string.rep("C", 109) .. string.char(0xFF, 0x80) .. "tail" .. string.rep("D", 6)
local marker = 42
return marker
