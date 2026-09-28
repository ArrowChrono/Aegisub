aegisub.register_macro("Workspace Entry Select Code", "Select the Workspace code line", function(subs)
  for index = 1, #subs do
    local line = subs[index]
    if line.class == "dialogue" and line.comment and line.effect == "code once" then
      return {index}, index
    end
  end
  error("Workspace entry code line missing")
end)

aegisub.register_macro("Workspace Entry Validation Probe", "Remain unavailable to the Automation menu", function()
  error("A rejected validation probe must not run")
end, function()
  return false
end, function()
  return false
end)
