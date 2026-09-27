local object = { branch = { name = "雪" } }

function object.branch:tag(suffix)
    return self.name .. ":" .. suffix
end

local path = object.branch.name
local method = object.branch:tag("点")
local n = 4
local joined = 1 .. 2
local leading = n .. .5
local positive = - -n

local function collect(...)
    local first, second = ...
    return first + second
end

local step = 0
::again::
step = step + 1
if step < 2 then goto again end

-- Ω . : .. ... - - 雪
--[=[Unicode Ω . : ... ]=]
local literal = [==[长串 . : .. -- ]=] intact]==]
return table.concat({path, method, joined, leading, tostring(positive), tostring(collect(3, 4)), tostring(step), literal}, "|")
