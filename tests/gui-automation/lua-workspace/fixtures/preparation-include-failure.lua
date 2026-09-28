include("preparation-missing-include.lua")

aegisub.register_macro("Preparation Missing Include", "Unreachable missing-include macro", function(subs, selected, active)
  return selected, active
end)
