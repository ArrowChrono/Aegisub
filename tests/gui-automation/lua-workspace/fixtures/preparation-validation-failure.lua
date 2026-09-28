aegisub.register_macro("Preparation Validation Failure", "Fail macro validation", function(subs, selected, active)
  return selected, active
end, function(subs, selected, active)
  return false, "preparation-validation-failure"
end)
