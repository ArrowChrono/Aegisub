local values = {["k\0a"] = 1, ["k\0b"] = 2, ["\255"] = 3, ["k\\x00a"] = 4}
local line = {class = "dialogue", style = "样式\255\0尾", text = "OK"}
local style = {class = "style", name = "名\128", fontname = "字\255"}
local info = {class = "info", key = "k\0ey", value = "值\255"}
local custom = {class = "tag\255\0end"}
local long_keys = {[string.rep("k", 160) .. "a"] = 5, [string.rep("k", 160) .. "b"] = 6}
local numeric_keys = {
    [1.25] = 11, [1.75] = 12, [-0.25] = 13, [-0.75] = 14,
    [1] = 15, [1 + 2^-52] = 16, [1e20] = 17,
    ["1.25"] = 18, ["[1.25]"] = 19, [true] = 20, [false] = 21
}
local tostring_calls = 0
local mt = {__tostring = function() tostring_calls = tostring_calls + 1; return "forbidden" end}
local table_a, table_b = setmetatable({}, mt), setmetatable({}, mt)
local function_a, function_b = function() end, function() end
local thread_a, thread_b = coroutine.create(function() end), coroutine.create(function() end)
local userdata_a, userdata_b = newproxy(true), newproxy(true)
getmetatable(userdata_a).__tostring = mt.__tostring
getmetatable(userdata_b).__tostring = mt.__tostring
local object_keys = {
    [table_a] = 31, [table_b] = 32,
    [function_a] = 33, [function_b] = 34,
    [thread_a] = 35, [thread_b] = 36,
    [userdata_a] = 37, [userdata_b] = 38
}
local shared_keys = {[table_a] = 41, [function_a] = 43, [thread_a] = 45, [userdata_a] = 47}
_G[1.25], _G["[1.25]"], _G[table_a] = 51, 52, 53
local summary48 = {class = "info", key = "limit", value = string.rep("Q", 48)}
local summary49 = {class = "info", key = "limit", value = string.rep("Q", 49)}
local summary_utf8_exact = {class = "info", key = "limit", value = string.rep("Q", 45) .. "中"}
local summary_utf8_over = {class = "info", key = "limit", value = string.rep("Q", 46) .. "中"}
local summary_escape_exact = {class = "info", key = "limit", value = string.rep("Q", 44) .. string.char(0xFF)}
local summary_escape_over = {class = "info", key = "limit", value = string.rep("Q", 45) .. string.char(0xFF)}
local total = values["k\0a"] + values["k\0b"] + values["\255"] + values["k\\x00a"]
assert(tostring_calls == 0)
return total
