aegisub.register_macro("Workspace Debug Long Loop", "Exercise pause, stop, and detach", function(subs, selected, active)
  local line = subs[active]
  line.effect = "provisional-loop"
  subs[active] = line
  aegisub.progress.task("workspace-debug-loop-started")
  local deadline = os.time() + 20
  local count = 0
  while os.time() < deadline do count = count + 1 end
  line = subs[active]
  line.effect = "loop-complete"
  subs[active] = line
  return selected, active
end)
