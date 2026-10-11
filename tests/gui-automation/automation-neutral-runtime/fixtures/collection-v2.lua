script_name = "Collection fixture"
script_description = "Scope and publication ownership"
script_version = "2"
aegisub.register_macro("Collection/Edit", "Version two", function(subs, selected)
    local line = subs[selected[1]]
    line.text = "collection-v2"
    subs[selected[1]] = line
    return selected
end)
aegisub.register_filter("Collection export", "Collection export fixture", 11, function() end)
