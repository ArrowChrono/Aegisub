script_name = "wx boundary input fixture"
script_description = "Select the three owned source rows before folding"
script_author = "Aegisub"
script_version = "1.0"

aegisub.register_macro("Visual Port Select Sources", "Select real sources before folding", function(subs)
  local ids = {}
  for i = 1, #subs do
    local line = subs[i]
    if line.class == "dialogue" and line.actor:match("^Source[ABC]$") then
      ids[#ids + 1] = i
    end
  end
  assert(#ids == 3, "expected exactly three transform sources")
  return ids, ids[1]
end)
