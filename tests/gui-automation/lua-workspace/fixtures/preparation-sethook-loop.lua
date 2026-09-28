local accepted, reason = pcall(debug.sethook, function() end, "", 1)
assert(not accepted and string.find(reason, "cannot replace the Lua Workspace cancellation hook", 1, true),
  "Workspace preparation did not reject replacement of its cancellation hook")

local count = 0
while true do count = count + 1 end
