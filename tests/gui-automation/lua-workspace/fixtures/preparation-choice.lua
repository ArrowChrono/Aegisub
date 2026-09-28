local function unchanged(subs, selected, active)
  return selected, active
end

aegisub.register_macro("Preparation Choice Alpha", "First preparation choice", unchanged)
aegisub.register_macro("Preparation Choice Beta", "Second preparation choice", unchanged)
