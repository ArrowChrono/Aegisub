script_name = "Collection fixture"
script_description = "Scope and publication ownership"
script_version = "1"
aegisub.register_macro("Collection/Edit", "Version one", function(subs, selected)
    local line = subs[selected[1]]
    line.text = "collection-v1"
    subs[selected[1]] = line
    return selected
end)
aegisub.register_filter("Collection export", "Collection export fixture", 11, function() end)
