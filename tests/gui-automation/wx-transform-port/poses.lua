script_name = "Shared visual transform native contract"
script_description = "Source selection and fixed-pose setup for the owned wx GUI driver"
script_author = "Aegisub"
script_version = "1.0"

local function sources(subs)
  local ids = {}
  for i = 1, #subs do
    local line = subs[i]
    if line.class == "dialogue" and line.actor:match("^Source[ABC]$") then
      ids[#ids + 1] = i
    end
  end
  assert(#ids == 3, "expected exactly three transform sources")
  return ids
end

aegisub.register_macro("Visual Port Select Sources", "Select real sources before folding", function(subs)
  local ids = sources(subs)
  return ids, ids[1]
end)

aegisub.register_macro("Visual Port Select Active Source", "Select one source for unchanged-pose pixel comparisons", function(subs)
  local id = sources(subs)[1]
  return { id }, id
end)

for _, pose in ipairs({
  { "Neutral", 0, 0, 0, 0, 0, 480 },
  { "NormalXYZShear", 15, 25, 30, 0.2, -0.1, 480 },
  { "EdgeOn", 90, 0, 30, 0.2, -0.1, 480 },
  { "NearCamera", 75, 80, 15, 0.2, -0.1, 38400 }
}) do
  local p = pose
  aegisub.register_macro("Visual Port Pose " .. p[1], "Set a fixed owned comparison pose", function(subs)
    local ids = sources(subs)
    local line = subs[ids[1]]
    local text = line.text:gsub("\\fr[xyz][%-%d%.]+", ""):gsub("\\fa[xy][%-%d%.]+", "")
    local tags = string.format("\\frx%g\\fry%g\\frz%g\\fax%g\\fay%g", p[2], p[3], p[4], p[5], p[6])
    line.text = text:gsub("^{", "{" .. tags, 1)
    subs[ids[1]] = line
    for i = 1, #subs do
      local info = subs[i]
      if info.class == "info" and info.key == "LayoutResY" then
        info.value = tostring(p[7])
        subs[i] = info
        break
      end
    end
    aegisub.set_undo_point("Visual port fixed pose")
    return sources(subs), sources(subs)[1]
  end)
end
