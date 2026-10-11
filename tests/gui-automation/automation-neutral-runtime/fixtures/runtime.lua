script_name = "Neutral runtime fixture"
script_description = "Real Lua host-neutral lifecycle and document workflow"
script_author = "Aegisub E2E"
script_version = "1.0"

local support = include("support.lua")
local clipboard = require("aegisub.clipboard")
local host_module = require("neutral_host")

local function verify_text_extents(subs)
    if not host_module.has_text_extents then return end
    local style
    for i = 1, #subs do
        if subs[i].class == "style" then
            style = subs[i]
            break
        end
    end
    assert(style, "Text measurement requires an ASS style")
    local function near(actual, expected)
        return math.abs(actual - expected) <= math.max(1, math.abs(expected)) * 0.000000001
    end
    style.scale_x, style.scale_y, style.spacing = 100, 100, 0
    local text = "Aegisub 字幕 café Ω"
    local width, height, descent, leading = aegisub.text_extents(style, text)
    assert(width > 0 and height > 0 and descent > 0 and leading >= 0, "Native text metrics must be positive")
    style.scale_x = 200
    local wide, same_height = aegisub.text_extents(style, text)
    assert(near(wide, width * 2) and near(same_height, height), "Horizontal scale must only scale width")
    style.scale_x, style.scale_y = 100, 150
    local same_width, tall, tall_descent, tall_leading = aegisub.text_extents(style, text)
    assert(near(same_width, width) and near(tall, height * 1.5), "Vertical scale must only scale vertical metrics")
    assert(near(tall_descent, descent * 1.5) and near(tall_leading, leading * 1.5), "Vertical scale must preserve baseline metrics")
    style.scale_y, style.spacing = 100, 2
    local spaced, spaced_height = aegisub.text_extents(style, "Subtitle")
    style.spacing = 5
    local more_spaced, more_spaced_height = aegisub.text_extents(style, "Subtitle")
    assert(near(more_spaced - spaced, 3 * 8) and near(spaced_height, more_spaced_height), "Letter spacing must change width consistently")
    style.spacing = 0
    style.bold, style.italic, style.underline, style.strikeout = true, true, true, true
    local decorated_width, decorated_height = aegisub.text_extents(style, text)
    assert(decorated_width > 0 and decorated_height > 0, "Decorated Unicode text must remain measurable")
    style.scale_x, style.scale_y = 125, 75
    local decorated_scaled_width, decorated_scaled_height = aegisub.text_extents(style, text)
    assert(near(decorated_scaled_width, decorated_width * 1.25) and near(decorated_scaled_height, decorated_height * 0.75), "Font flags must preserve scale semantics")
end

local function controls()
    return {
        { class = "label", name = "heading", label = "Subtitle values", x = 0, y = 0, width = 2 },
        { class = "edit", name = "prefix", text = "initial", hint = "Prefix", x = 0, y = 1 },
        { class = "textbox", name = "notes", text = "initial notes", x = 1, y = 1, height = 2 },
        { class = "intedit", name = "offset", value = 100, min = -500, max = 500, x = 0, y = 2 },
        { class = "floatedit", name = "factor", value = 1.0, min = 0.25, max = 3.0, step = 0.25, x = 0, y = 3 },
        { class = "dropdown", name = "style", value = "Default", items = { "Default", "Emphasis" }, x = 1, y = 3 },
        { class = "checkbox", name = "enabled", label = "Apply edits", value = false, x = 0, y = 4 },
        { class = "color", name = "rgb", value = "#102030", x = 1, y = 4 },
        { class = "coloralpha", name = "rgba", value = "&H40302010&", x = 0, y = 5 },
        { class = "alpha", name = "alpha", value = "&H00&", x = 1, y = 5 }
    }
end

local function validate(subs, selected, active)
    local allowed = #selected == 2 and active == selected[1]
    return allowed, "Selected " .. #selected .. ": " .. (active and subs[active].text or "none")
end

local function active(subs, selected, row)
    return row ~= nil and subs[row].text:find("accepted", 1, true) ~= nil
end

local function edit(subs, selected, row)
    assert(support.marker == "included-v1")
    assert(host_module.identity == "neutral-host-module")
    verify_text_extents(subs)
    assert(aegisub.gettext("ready") == "translated:ready")
    assert(aegisub.file_name() == "session.ass")
    assert(aegisub.frame_from_ms(1000) == 25 and aegisub.ms_from_frame(25) == 1000)
    local w, h = aegisub.video_size()
    assert(w == 1280 and h == 720)
    assert(clipboard.set("neutral clipboard"))
    assert(clipboard.get() == "neutral clipboard")
    local first = subs[selected[1]]
    first.text = "staged before prompt"
    subs[selected[1]] = first
    aegisub.set_undo_point("Neutral staged checkpoint")
    local button, values = aegisub.dialog.display(controls(), { "Apply", "Dismiss" }, { ok = "Apply", cancel = "Dismiss" })
    if button == false then aegisub.cancel() end
    assert(button == "Apply" and values.prefix == "accepted:|%" and values.notes == "two\nlines",
        "Dialog text mismatch: " .. tostring(button) .. ", " .. tostring(values.prefix) .. ", " .. tostring(values.notes))
    assert(values.offset == 250 and values.factor == 1.5 and values.style == "Emphasis" and values.enabled == true,
        "Dialog typed values mismatch: " .. tostring(values.offset) .. ", " .. tostring(values.factor) .. ", " .. tostring(values.style) .. ", " .. tostring(values.enabled))
    assert(values.rgb == "#336699" and values.rgba == "#12345678" and values.alpha == "&H80&",
        "Dialog color mismatch: " .. tostring(values.rgb) .. ", " .. tostring(values.rgba) .. ", " .. tostring(values.alpha))
    local files = aegisub.dialog.open("Choose input", "", "", "ASS (*.ass)|*.ass", true, true)
    assert(#files == 2 and files[1] == "input-one.ass" and files[2] == "input-two.ass")
    assert(aegisub.dialog.save("Choose output", "", "output.ass", "ASS (*.ass)|*.ass", true) == "chosen-output.ass")
    for i, index in ipairs(selected) do
        local line = subs[index]
        line.text = values.prefix .. ":" .. i
        line.start_time = line.start_time + values.offset
        line.end_time = line.end_time + values.offset
        line.style = values.style
        subs[index] = line
    end
    aegisub.set_undo_point("Neutral accepted values")
    local copy = subs[selected[2]]
    copy.text = "Generated: " .. values.notes:gsub("\n", "\\N")
    copy.actor = "Neutral host"
    subs.append(copy)
    aegisub.progress.title("Neutral edit")
    aegisub.progress.task("Committed prompt values")
    aegisub.progress.set(100)
    return { selected[2], #subs }, #subs
end

local function cancel_progress(subs, selected)
    local line = subs[selected[1]]
    line.text = "must roll back cancellation"
    subs[selected[1]] = line
    aegisub.set_undo_point("Must not publish cancelled checkpoint")
    for i = 1, 10 do
        aegisub.progress.task("Cancelable stage " .. i)
        aegisub.progress.set(i * 10)
        if aegisub.progress.is_cancelled() then aegisub.cancel() end
    end
    error("neutral-fixture-cancel-not-delivered")
end

local function fail(subs, selected)
    local line = subs[selected[1]]
    line.text = "must roll back runtime failure"
    subs[selected[1]] = line
    aegisub.set_undo_point("Must not publish failed checkpoint")
    error("neutral-fixture-runtime-failure")
end

local function workspace(subs, selected, row)
    local text, uri = aegisub.__workspace_code_source(row)
    assert(text and uri and uri:find("=aegisub-workspace://", 1, true) == 1)
    local chunk = assert(loadstring(text, uri))
    local line = subs[selected[1]]
    line.text = chunk()
    line.comment = false
    line.effect = "workspace output"
    subs.append(line)
    return { #subs }, #subs
end

aegisub.register_macro("Neutral/Edit", "Edit selected subtitles", edit, validate, active)
aegisub.register_macro("Neutral/Cancel progress", "Cancel staged edits", cancel_progress)
aegisub.register_macro("Neutral/Failure", "Fail staged edits", fail)
aegisub.register_macro("Neutral/Workspace", "Run bound source", workspace)

aegisub.register_filter("Neutral export", "Host-neutral export values", 37, function(subs, values)
    assert(values.prefix == "export:|%" and values.offset == -125 and values.enabled == true)
    assert(values.notes == "export\nnotes" and values.factor == 1.5 and values.style == "Emphasis")
    assert(values.rgb == "#336699" and values.rgba == "#12345678" and values.alpha == "&H80&")
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" and not line.comment then
            line.text = values.prefix .. line.text
            line.start_time = line.start_time + values.offset
            subs[i] = line
        end
    end
end, function() return controls() end)
