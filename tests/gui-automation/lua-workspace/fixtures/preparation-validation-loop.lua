aegisub.register_macro("Preparation Validation Loop", "Loop in macro validation", function(subs, selected, active)
  return selected, active
end, function(subs, selected, active)
  if jit then jit.off() end
  local count = 0
  while true do count = count + 1 end
end)
