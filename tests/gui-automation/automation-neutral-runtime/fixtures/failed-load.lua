script_name = "Unpublished candidate"
aegisub.register_macro("Neutral/Partial", "Must never publish", function() end)
aegisub.register_filter("Neutral partial filter", "Must never publish", 1, function() end)
error("neutral-fixture-load-failure")
