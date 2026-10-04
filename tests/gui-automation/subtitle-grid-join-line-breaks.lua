script_name = "Subtitle grid join with line breaks regression"
script_description = "Real join commands, retained metadata, selection, and undo/redo assertions."
script_author = "Aegisub"
script_version = "1.0"

local initial_document
local prepared_document

local function copy(value)
  if type(value) ~= "table" then return value end
  local result = {}
  for key, item in pairs(value) do
    if key ~= "raw" then result[key] = copy(item) end
  end
  return result
end

local function equal(left, right)
  if type(left) ~= type(right) then return false end
  if type(left) ~= "table" then return left == right end
  for key, value in pairs(left) do
    if not equal(value, right[key]) then return false end
  end
  for key in pairs(right) do
    if left[key] == nil then return false end
  end
  return true
end

local function document(subtitles)
  local result = {}
  for index = 1, #subtitles do result[index] = copy(subtitles[index]) end
  return result
end

local function dialogues(subtitles)
  local result = {}
  for index = 1, #subtitles do
    local line = subtitles[index]
    if line.class == "dialogue" then
      result[#result + 1] = { index = index, line = line }
    end
  end
  return result
end

local function first_selected(subtitles, selected, active)
  local rows = dialogues(subtitles)
  return #rows >= 2 and #selected == 1
    and selected[1] == rows[2].index and active == rows[2].index
end

local function original(subtitles, selected, active)
  return equal(document(subtitles), prepared_document)
    and first_selected(subtitles, selected, active)
end

local function joined(subtitles, selected, active, text)
  if not prepared_document then return false end
  local expected = copy(prepared_document)
  local original_rows = dialogues(expected)
  expected[original_rows[2].index].text = text
  expected[original_rows[2].index].end_time = 11200
  for _, row in ipairs({ 7, 5, 4 }) do
    table.remove(expected, original_rows[row].index)
  end
  return equal(document(subtitles), expected)
    and first_selected(subtitles, selected, active)
end

local function register_check(name, predicate)
  aegisub.register_macro(name, name, function() end, predicate)
end

local evidence_fields = {
  "text", "start_time", "end_time", "comment", "layer", "style", "actor", "effect",
  "margin_l", "margin_r", "margin_t", "margin_b"
}

local function register_record(name, label, mode)
  aegisub.register_macro(name, name, function(subtitles, selected, active)
    local path = aegisub.decode_path("?user/../artifacts/join-line-breaks-evidence.txt")
    local output = assert(io.open(path, mode))
    local rows = dialogues(subtitles)
    local selection = {}
    local active_row = 0
    for row, entry in ipairs(rows) do
      if entry.index == active then active_row = row end
      for _, index in ipairs(selected) do
        if entry.index == index then selection[#selection + 1] = row end
      end
    end
    output:write(label, "\nrows=", #rows, "\nselection=", table.concat(selection, ","),
      "\nactive=", active_row, "\n")
    for row, entry in ipairs(rows) do
      output:write("row=", row)
      for _, field in ipairs(evidence_fields) do
        output:write("\t", field, "=", string.format("%q", tostring(entry.line[field])))
      end
      local keys = {}
      for key in pairs(entry.line.extra) do keys[#keys + 1] = key end
      table.sort(keys)
      for _, key in ipairs(keys) do
        output:write("\textra[", string.format("%q", key), "]=",
          string.format("%q", entry.line.extra[key]))
      end
      output:write("\n")
    end
    output:write("\n")
    assert(output:close())
    return selected, active
  end)
end

aegisub.register_macro("Prepare", "Create deterministic noncontiguous join input", function(subtitles)
  initial_document = document(subtitles)
  local rows = dialogues(subtitles)
  assert(#rows > 0, "default document has no template dialogue")
  local template = copy(rows[1].line)
  local alternate_style
  for index = 1, #subtitles do
    local line = subtitles[index]
    if line.class == "style" and line.name == template.style then
      alternate_style = copy(line)
      break
    end
  end
  assert(alternate_style, "template dialogue has no matching style")
  alternate_style.name = "JoinOther"
  for row = #rows, 1, -1 do subtitles.delete(rows[row].index) end
  subtitles.append(alternate_style)
  local texts = {
    "unselected before",
    "{\\i1}你好 Hello{\\i0}\\N已有换行",
    "{\\b1}untouched middle A",
    "",
    "{\\b1}Second 第二{\\b0}\\Ntail",
    "unselected 中间 B\\Nkeep",
    "{\\pos(320,240)}终点 End",
    "unselected after"
  }
  local times = {
    { 100, 700 }, { 5200, 6800 }, { 9000, 9800 }, { 700, 11200 },
    { 2400, 3900 }, { 300, 15000 }, { 1000, 7200 }, { 16000, 18000 }
  }
  for row, text in ipairs(texts) do
    local line = copy(template)
    line.text = text
    line.start_time = times[row][1]
    line.end_time = times[row][2]
    line.comment = row == 2
    line.layer = row == 2 and 17 or row * 10
    line.style = (row == 4 or row == 5 or row == 7) and "JoinOther" or template.style
    line.actor = row == 2 and "首行 actor" or "actor-" .. row
    line.effect = row == 2 and "join-first-effect" or "effect-" .. row
    line.margin_l = row == 2 and 11 or row * 10
    line.margin_r = row == 2 and 22 or row * 10 + 1
    line.margin_t = row == 2 and 33 or row * 10 + 2
    line.margin_b = line.margin_t
    line.extra = row == 2 and { ["join-test"] = "keep-first", source = "首行" }
      or { ["join-test"] = "row-" .. row, source = "source-" .. row }
    subtitles.append(line)
  end
  aegisub.set_undo_point("Prepare join line breaks regression")
  prepared_document = document(subtitles)
  rows = dialogues(subtitles)
  return { rows[2].index }, rows[2].index
end)

aegisub.register_macro("Select input", "Select four noncontiguous lines with the last line active", function(subtitles)
  assert(equal(document(subtitles), prepared_document), "join input document is not restored")
  local rows = dialogues(subtitles)
  return { rows[7].index, rows[2].index, rows[5].index, rows[4].index }, rows[7].index
end)

register_check("Single selection", original)
register_check("Input selection", function(subtitles, selected, active)
  if not equal(document(subtitles), prepared_document) then return false end
  local rows = dialogues(subtitles)
  return equal(selected, { rows[2].index, rows[4].index, rows[5].index, rows[7].index })
    and active == rows[7].index
end)
register_check("Joined line breaks", function(subtitles, selected, active)
  return joined(subtitles, selected, active,
    "{\\i1}你好 Hello{\\i0}\\N已有换行\\N\\N{\\b1}Second 第二{\\b0}\\Ntail\\N{\\pos(320,240)}终点 End")
end)
register_check("Original after undo", original)
register_check("Joined spaces", function(subtitles, selected, active)
  return joined(subtitles, selected, active,
    "{\\i1}你好 Hello{\\i0}\\N已有换行  {\\b1}Second 第二{\\b0}\\Ntail {\\pos(320,240)}终点 End")
end)
register_check("Initial document restored", function(subtitles)
  return equal(document(subtitles), initial_document)
end)

register_record("Record prepared", "prepared", "w")
register_record("Record line breaks", "line-breaks", "a")
register_record("Record undo", "undo", "a")
register_record("Record redo", "redo", "a")
register_record("Record concatenate", "concatenate", "a")
register_record("Record initial", "initial-restored", "a")
