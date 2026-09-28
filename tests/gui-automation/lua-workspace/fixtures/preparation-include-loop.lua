include("preparation-include-loop-body.lua")

aegisub.register_macro("Preparation Include Loop", "Unreachable include-loop macro", function(subs, selected, active)
  return selected, active
end)
