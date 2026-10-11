script_name = "Fresh host snapshots"
script_version = "1"

local function describe_host()
    local width, height = aegisub.video_size()
    local properties = aegisub.project_properties()
    return table.concat({
        aegisub.file_name(), aegisub.decode_path("?script"),
        width, height, aegisub.frame_from_ms(1000), aegisub.ms_from_frame(10),
        properties.style_storage
    }, "|")
end

aegisub.register_macro("Snapshots/Read host", "Current invocation host", function(subs, selected)
    local line = subs[selected[1]]
    line.text = describe_host()
    subs[selected[1]] = line
    return selected, selected[1]
end, function()
    return true, describe_host()
end)

aegisub.register_filter("Snapshot export", "Explicit export host", 19, function(subs)
    for i = 1, #subs do
        local line = subs[i]
        if line.class == "dialogue" and not line.comment then
            line.text = describe_host()
            subs[i] = line
        end
    end
end)
