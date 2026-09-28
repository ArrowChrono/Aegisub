aegisub.register_macro("Preparation Validation Error", "Throw from macro validation", function(subs, selected, active)
  return selected, active
end, function()
  error("preparation-validation-runtime-error")
end)
