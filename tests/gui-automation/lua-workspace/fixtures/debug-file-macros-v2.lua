local function set_effect(subs, selected, active, effect)
  local line = subs[active]
  line.effect = effect
  subs[active] = line
  return selected, active
end

aegisub.register_macro("Workspace File Alpha", "First managed macro", function(subs, selected, active)
  return set_effect(subs, selected, active, "managed-alpha")
end)

aegisub.register_macro("Workspace File Gamma", "New managed macro after saved reload", function(subs, selected, active)
  return set_effect(subs, selected, active, "managed-gamma")
end)
