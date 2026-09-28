aegisub.register_macro("Workspace Debug Infinite Loop", "Exercise detach and stop", function(subs, selected, active)
  local line = subs[active]
  line.effect = "provisional-infinite-loop"
  subs[active] = line
  aegisub.progress.task("workspace-debug-loop-started")
  local count = 0
  while true do count = count + 1 end
  return selected, active
end)
