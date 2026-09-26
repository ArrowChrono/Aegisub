aegisub.register_macro("Workspace Debug Select First Code", "Select the first code line", function(subs)
  for index = 1, #subs do
    local line = subs[index]
    if line.class == "dialogue" and line.effect:match("^code") then
      return {index}, index
    end
  end
  error("first code line missing")
end)

aegisub.register_macro("Workspace Debug Select Second Code", "Select the second code line", function(subs)
  local count = 0
  for index = 1, #subs do
    local line = subs[index]
    if line.class == "dialogue" and line.effect:match("^code") then
      count = count + 1
      if count == 2 then
        return {index}, index
      end
    end
  end
  error("second code line missing")
end)

local expected_seed = {
  {comment=true, layer=0, start_time=0, end_time=0, effect="debug-seed", text='decorate = function(value) return "OLD:" .. string.upper(value) end'},
  {comment=true, layer=0, start_time=0, end_time=0, effect="code once", text="helper = function(value) return value end"},
  {comment=true, layer=1, start_time=0, end_time=0, effect="template line notext loop 2", text="L!j!:!decorate(syl.text_stripped)!;"},
  {comment=true, layer=2, start_time=0, end_time=0, effect="template syl notext loop 2", text='!retime("syl",0,0)!S!j!:!decorate(syl.text_stripped)!'},
  {comment=true, layer=3, start_time=0, end_time=0, effect="template furi notext loop 2", text='!retime("syl",0,0)!F!j!:!decorate(syl.text_stripped)!'},
  {comment=false, layer=0, start_time=1000, end_time=2000, effect="", text="{\\k50}alpha|a{\\k50}beta|b"},
  {comment=false, layer=0, start_time=3000, end_time=4500, effect="", text="{\\k75}gamma|g{\\k75}delta|d"}
}

aegisub.register_macro("Workspace Debug Seed", "Create one committed undo anchor", function(subs, selected, active)
  local line = subs[active]
  assert(line.class == "dialogue" and line.effect == "code once", "Seed requires the first code line")
  line.effect = "debug-seed"
  subs[active] = line
  return selected, active
end)

aegisub.register_macro("Workspace Debug Verify Seed Rollback", "Verify all live dialogue fields after cancellation", function(subs, selected, active)
  local count = 0
  for index = 1, #subs do
    local line = subs[index]
    if line.class == "dialogue" then
      count = count + 1
      local expected = assert(expected_seed[count], "Cancellation added a dialogue")
      for _, field in ipairs({"comment", "layer", "start_time", "end_time", "effect", "text"}) do
        assert(line[field] == expected[field], "Cancellation changed dialogue " .. count .. " field " .. field)
      end
      assert(line.style == "Default" and line.actor == "", "Cancellation changed style or actor")
      assert(line.margin_l == 0 and line.margin_r == 0 and line.margin_t == 0 and line.margin_b == 0,
        "Cancellation changed margins")
    end
  end
  assert(count == #expected_seed, "Cancellation removed a dialogue")
  local button = aegisub.dialog.display({
    {class="label", label="Debug seed rollback verified", x=0, y=0}
  }, {"OK"})
  assert(button == "OK", "Rollback acknowledgement was not accepted")
  return selected, active
end)

local expected_original = {}
for index, line in ipairs(expected_seed) do
  local copy = {}
  for key, value in pairs(line) do copy[key] = value end
  expected_original[index] = copy
end
expected_original[1].effect = "code once"

aegisub.register_macro("Workspace Debug Verify Original", "Verify unchanged live subtitles after a failed Lua-file run", function(subs, selected, active)
  local count = 0
  for index = 1, #subs do
    local line = subs[index]
    if line.class == "dialogue" then
      count = count + 1
      local expected = assert(expected_original[count], "Failed file run added a dialogue")
      for _, field in ipairs({"comment", "layer", "start_time", "end_time", "effect", "text"}) do
        assert(line[field] == expected[field], "Failed file run changed dialogue " .. count .. " field " .. field)
      end
      assert(line.style == "Default" and line.actor == "", "Failed file run changed style or actor")
      assert(line.margin_l == 0 and line.margin_r == 0 and line.margin_t == 0 and line.margin_b == 0,
        "Failed file run changed margins")
    end
  end
  assert(count == #expected_original, "Failed file run removed a dialogue")
  local button = aegisub.dialog.display({
    {class="label", label="Debug original live baseline verified", x=0, y=0}
  }, {"OK"})
  assert(button == "OK", "Original baseline acknowledgement was not accepted")
  return selected, active
end)
