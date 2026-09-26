aegisub.register_macro("Workspace File Saved", "New saved-file version", function(subs, selected, active)
  local line = subs[active]
  line.effect = "file-saved"
  subs[active] = line
  return selected, active
end)
