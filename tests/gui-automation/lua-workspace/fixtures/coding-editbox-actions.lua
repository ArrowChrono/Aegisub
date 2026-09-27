script_name = "Coding edit box E2E actions"
script_description = "Selects fixture events through the real Automation host"
script_author = "Aegisub"
script_version = "1"

local names = {"CodeA", "Ordinary", "CodeB", "Template", "FalseCode"}
for _, name in ipairs(names) do
    local target = name
    aegisub.register_macro("Coding E2E Select " .. target, script_description,
        function(subs)
            for i = 1, #subs do
                local line = subs[i]
                if line.class == "dialogue" and line.actor == target then
                    return {i}, i
                end
            end
            error("Missing fixture actor " .. target)
        end)
end
