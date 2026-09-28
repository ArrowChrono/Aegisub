if jit then
  jit.on()
  assert(not jit.status(), "Workspace preparation let top-level Lua re-enable JIT")
end

local count = 0
while true do count = count + 1 end
