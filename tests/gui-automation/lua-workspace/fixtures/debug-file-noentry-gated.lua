local source = debug.getinfo(1, "S").source:gsub("\\", "/"):gsub("^@", "")
local root = assert(source:match("^(.+/)"), "Gated no-entry fixture has no compiled file path")
local entered = assert(io.open(root .. "debug-noentry-gate-entered.txt", "a"))
entered:write("entered\n")
entered:close()

local release = root .. "debug-noentry-gate-release.txt"
local started = os.clock()
while true do
  local marker = io.open(release, "r")
  if marker then
    marker:close()
    break
  end
  assert(os.clock() - started < 45, "Gated no-entry fixture timed out waiting for editor revision")
end
