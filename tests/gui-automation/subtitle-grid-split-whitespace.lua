script_name = "Subtitle grid split by whitespace regression"
script_description = "Real whitespace split command, exact metadata, selection, folding, and history assertions."
script_author = "Aegisub"
script_version = "1.0"

local fold_key = "_aegi_folddata"
local initial_document
local prepared_document
local split_document
local split_selection
local split_active
local input_rows = { 2, 4, 5, 6, 8, 9, 10, 11 }
local noop_rows = { 4, 5, 9, 10 }

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

local function selection(subtitles, row_numbers, active_row)
  local rows = dialogues(subtitles)
  local selected = {}
  for _, row in ipairs(row_numbers) do selected[#selected + 1] = rows[row].index end
  return selected, rows[active_row].index
end

local function prepared(subtitles, selected, active, row_numbers, active_row)
  if not equal(document(subtitles), prepared_document) then return false end
  local expected_selection, expected_active = selection(subtitles, row_numbers, active_row)
  return equal(selected, expected_selection) and active == expected_active
end

local function register_check(name, predicate)
  aegisub.register_macro(name, name, function() end, predicate)
end

local evidence_fields = {
  "text", "start_time", "end_time", "comment", "layer", "style", "actor", "effect",
  "margin_l", "margin_r", "margin_t", "margin_b"
}

local function escaped(value)
  local result = { '"' }
  local text = tostring(value)
  for index = 1, #text do
    local byte = text:byte(index)
    result[#result + 1] = byte >= 32 and byte <= 126 and byte ~= 34 and byte ~= 92
      and string.char(byte) or string.format("\\x%02X", byte)
  end
  result[#result + 1] = '"'
  return table.concat(result)
end

local function register_record(name, label, mode)
  aegisub.register_macro(name, name, function(subtitles, selected, active)
    local path = aegisub.decode_path("?user/../artifacts/split-whitespace-evidence.txt")
    local output = assert(io.open(path, mode))
    local rows = dialogues(subtitles)
    local selected_rows = {}
    local active_row = 0
    for row, entry in ipairs(rows) do
      if entry.index == active then active_row = row end
      for _, index in ipairs(selected) do
        if entry.index == index then selected_rows[#selected_rows + 1] = row end
      end
    end
    assert(output:write(label, "\nrows=", #rows, "\nselection=", table.concat(selected_rows, ","),
      "\nactive=", active_row, "\n"))
    for row, entry in ipairs(rows) do
      assert(output:write("row=", row))
      for _, field in ipairs(evidence_fields) do
        assert(output:write("\t", field, "=", escaped(entry.line[field])))
      end
      local keys = {}
      for key in pairs(entry.line.extra) do keys[#keys + 1] = key end
      table.sort(keys)
      for _, key in ipairs(keys) do
        assert(output:write("\textra[", escaped(key), "]=", escaped(entry.line.extra[key])))
      end
      assert(output:write("\n"))
    end
    assert(output:write("\n"))
    assert(output:close())
    return selected, active
  end)
end

aegisub.register_macro("Prepare", "Create noncontiguous Unicode whitespace split input", function(subtitles)
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
  alternate_style.name = "WhitespaceOther"
  for row = #rows, 1, -1 do subtitles.delete(rows[row].index) end
  subtitles.append(alternate_style)
  local whitespace = {
    "\t", "\n", "\v", "\f", "\r", " ",
    string.char(0xC2, 0x85), string.char(0xC2, 0xA0), string.char(0xE1, 0x9A, 0x80),
    string.char(0xE2, 0x80, 0x80), string.char(0xE2, 0x80, 0x81),
    string.char(0xE2, 0x80, 0x82), string.char(0xE2, 0x80, 0x83),
    string.char(0xE2, 0x80, 0x84), string.char(0xE2, 0x80, 0x85),
    string.char(0xE2, 0x80, 0x86), string.char(0xE2, 0x80, 0x87),
    string.char(0xE2, 0x80, 0x88), string.char(0xE2, 0x80, 0x89),
    string.char(0xE2, 0x80, 0x8A), string.char(0xE2, 0x80, 0xA8),
    string.char(0xE2, 0x80, 0xA9), string.char(0xE2, 0x80, 0xAF),
    string.char(0xE2, 0x81, 0x9F), string.char(0xE3, 0x80, 0x80)
  }
  local tokens = {}
  local all_whitespace = {}
  for index = 1, #whitespace + 1 do
    tokens[index] = string.format("w%02d", index)
    all_whitespace[#all_whitespace + 1] = tokens[index]
    if whitespace[index] then all_whitespace[#all_whitespace + 1] = whitespace[index] end
  end
  local texts = {
    "unselected before\tkeep",
    "  你好  Hello\t😀　 ",
    "{\\b1}unselected middle A",
    "",
    " \t单词　",
    table.concat(all_whitespace),
    "unselected 中间 B\\Nkeep",
    "{\\fnComic Sans}甲\\N乙\\h尾\tZ",
    table.concat(whitespace),
    "one" .. string.char(0xE2, 0x80, 0x8B) .. string.char(0xEF, 0xBB, 0xBF) .. "word",
    "boundary end",
    "unselected after with spaces"
  }
  local parts = {
    [2] = { "你好", "Hello", "😀" },
    [6] = tokens,
    [8] = { "{\\fnComic", "Sans}甲\\N乙\\h尾", "Z" },
    [11] = { "boundary", "end" }
  }
  for row, text in ipairs(texts) do
    local line = copy(template)
    line.text = text
    line.start_time = (13 - row) * 700
    line.end_time = line.start_time + row * 200
    line.comment = row == 2 or row == 8
    line.layer = row * 3
    line.style = row % 2 == 0 and "WhitespaceOther" or template.style
    line.actor = "actor-" .. row .. "-演员"
    line.effect = "effect-" .. row
    line.margin_l = row * 10
    line.margin_r = row * 10 + 1
    line.margin_t = row * 10 + 2
    line.margin_b = line.margin_t
    line.extra = { ["whitespace-test"] = "retain-" .. row, source = "source-" .. row }
    if row == 2 then line.extra[fold_key] = "0;0;1" end
    if row == 11 then line.extra[fold_key] = "1;0;1" end
    subtitles.append(line)
  end
  aegisub.set_undo_point("Prepare whitespace split regression")
  prepared_document = document(subtitles)
  split_document = {}
  split_selection = {}
  local selected_rows = {}
  for _, row in ipairs(input_rows) do selected_rows[row] = true end
  local row = 0
  for _, original in ipairs(prepared_document) do
    if original.class == "dialogue" then row = row + 1 end
    local fragments = original.class == "dialogue" and parts[row] or nil
    for fragment = 1, fragments and #fragments or 1 do
      local line = copy(original)
      if fragments then line.text = fragments[fragment] end
      if fragment > 1 then line.extra[fold_key] = nil end
      split_document[#split_document + 1] = line
      if original.class == "dialogue" and selected_rows[row] then
        split_selection[#split_selection + 1] = #split_document
      end
      if original.class == "dialogue" and row == 8 and fragment == 1 then
        split_active = #split_document
      end
    end
  end
  return selection(subtitles, input_rows, 8)
end)

aegisub.register_macro("Select input", "Select noncontiguous split rows in non-document order", function(subtitles)
  assert(equal(document(subtitles), prepared_document), "split input document is not restored")
  return selection(subtitles, { 11, 8, 2, 10, 6, 4, 9, 5 }, 8)
end)

aegisub.register_macro("Select no-op", "Select empty whitespace-only and single-fragment lines", function(subtitles)
  assert(equal(document(subtitles), prepared_document), "no-op input document is not restored")
  return selection(subtitles, noop_rows, 9)
end)

register_check("Input unchanged", function(subtitles, selected, active)
  return prepared(subtitles, selected, active, input_rows, 8)
end)
register_check("No-op unchanged", function(subtitles, selected, active)
  return prepared(subtitles, selected, active, noop_rows, 9)
end)
register_check("Split exact", function(subtitles, selected, active)
  return equal(document(subtitles), split_document)
    and equal(selected, split_selection) and active == split_active
end)
register_check("Initial restored", function(subtitles)
  return equal(document(subtitles), initial_document)
end)

register_record("Record prepared", "prepared", "w")
register_record("Record no-op", "no-op", "a")
register_record("Record split", "split", "a")
register_record("Record repeat", "repeat-no-op", "a")
register_record("Record undo", "undo", "a")
register_record("Record redo", "redo", "a")
register_record("Record initial", "initial-restored", "a")
