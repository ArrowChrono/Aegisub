local function inner(value)
  local next_value = value + 1
  return next_value
end
local function outer(value)
  local inner_value = inner(value)
  return inner_value * 2
end
aegisub.register_macro("Workspace Debug Steps", "Exercise nested stepping", function(subs, selected, active)
  local result = outer(3)
  local line = subs[active]
  line.effect = "step:" .. result
  subs[active] = line
  return selected, active
end)
