local ascii_cjk = string.rep("A", 114) .. "中" .. "tail"
local emoji = string.rep("B", 113) .. "🙂" .. "tail"
local invalid = string.rep("C", 109) .. string.char(0xFF, 0x80) .. "tail" .. string.rep("D", 6)
local ascii118 = string.rep("A", 118)
local ascii119 = string.rep("A", 119)
local ascii120 = string.rep("A", 120)
local ascii121 = string.rep("A", 121)
local utf8_exact = string.rep("U", 117) .. "中"
local utf8_over = string.rep("U", 118) .. "中"
local escape_exact = string.rep("V", 116) .. string.char(0xFF)
local escape_over = string.rep("V", 117) .. string.char(0xFF)
local marker = 42
return marker
