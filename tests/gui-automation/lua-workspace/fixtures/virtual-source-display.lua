local values = {["k\0a"] = 1, ["k\0b"] = 2, ["\255"] = 3, ["k\\x00a"] = 4}
local line = {class = "dialogue", style = "样式\255\0尾", text = "OK"}
local style = {class = "style", name = "名\128", fontname = "字\255"}
local info = {class = "info", key = "k\0ey", value = "值\255"}
local custom = {class = "tag\255\0end"}
local long_keys = {[string.rep("k", 160) .. "a"] = 5, [string.rep("k", 160) .. "b"] = 6}
local total = values["k\0a"] + values["k\0b"] + values["\255"] + values["k\\x00a"]
return total
