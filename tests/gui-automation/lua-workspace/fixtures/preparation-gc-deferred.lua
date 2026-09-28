local finalized = false

aegisub.register_macro("Preparation GC Deferred", "Check Workspace avoids forced GC during preparation",
  function(subs, selected, active)
    assert(not finalized, "Workspace forced a full GC before macro execution")
    collectgarbage("restart")
    collectgarbage("collect")
    assert(finalized, "The validation finalizer was not collected on request")
    aegisub.debug.out("preparation-gc-deferred|validated\n")
    return selected, active
  end,
  function()
    collectgarbage("stop")
    local token = newproxy(true)
    getmetatable(token).__gc = function() finalized = true end
    token = nil
    return true
  end)
