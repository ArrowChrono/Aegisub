aegisub.register_macro("Workspace File Old", "Old saved-file version", function(subs, selected, active)
  local line = subs[active]
  line.effect = "file-old"
  subs[active] = line
  return selected, active
end)
