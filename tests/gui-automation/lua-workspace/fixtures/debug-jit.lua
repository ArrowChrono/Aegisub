local original_mode = "on"
local outcome = "completed"
local jit_util = require("jit.util")
local expected_engine = original_mode == "on"
assert(original_mode == "on" or original_mode == "off", "Unexpected JIT mode")
assert(outcome == "completed" or outcome == "failed" or outcome == "cancelled", "Unexpected JIT outcome")

local function hot_loop(limit, announce)
  local total = 0
  if announce then
    aegisub.progress.task("jit-loop-started|" .. original_mode .. "|" .. outcome)
  end
  for index = 1, limit do
    total = total + index % 97
  end
  return total
end

jit.flush()
if expected_engine then jit.on() else jit.off() end
assert(jit.status() == expected_engine, "Top-level JIT mode was not set")
local prewarmed = false
local warmed_trace = 0
if expected_engine then
  local candidates = {}
  local function trace_event(event, trace, func)
    if event == "start" then
      candidates[trace] = func == hot_loop
    elseif event == "abort" then
      candidates[trace] = nil
    end
  end
  jit.attach(trace_event, "trace")
  local warm_result = 0
  for iteration = 1, 16 do warm_result = hot_loop(512, false) end
  jit.attach(trace_event)
  assert(warm_result == 23658, "Prewarm business result is incorrect")
  for trace, matches in pairs(candidates) do
    if matches and jit_util.traceinfo(trace) then
      prewarmed = true
      warmed_trace = trace
      break
    end
  end
  assert(prewarmed, "The hot_loop function did not produce a retained JIT trace")
else
  assert(jit_util.traceinfo(1) == nil, "JIT-off fixture retained a compiled trace")
end

local invocation_count = 0
local observed_workspace_mode = nil

aegisub.register_macro("Workspace JIT Exercise", "Verify a scoped JIT lifecycle with real source and subtitle changes", function(subs, selected, active)
  local workspace_mode = jit.status()
  assert(workspace_mode == false, "Workspace did not disable the JIT engine")
  assert(jit_util.traceinfo(1) == nil, "Workspace did not flush pre-existing JIT traces")
  invocation_count = invocation_count + 1
  observed_workspace_mode = workspace_mode
  aegisub.debug.out(0, "jit-enter|" .. original_mode .. "|" .. outcome
    .. "|original=" .. tostring(expected_engine) .. "|workspace=" .. tostring(workspace_mode)
    .. "|prewarmed=" .. tostring(prewarmed) .. "|trace=" .. warmed_trace .. "\n")
  local line = subs[active]
  assert(line.class == "dialogue" and line.comment and line.effect == "code once", "JIT fixture requires the original first code line")
  line.effect = "jit-" .. original_mode .. "-provisional-" .. outcome
  subs[active] = line
  if outcome == "failed" then
    aegisub.set_undo_point("Provisional JIT failure")
    error("workspace-jit-" .. original_mode .. "-intentional-failure")
  elseif outcome == "cancelled" then
    aegisub.set_undo_point("Provisional JIT cancellation")
    hot_loop(1000000000, true)
    error("JIT cancellation fixture reached its natural end without Stop")
  end
  local value = hot_loop(17, false)
  assert(value == 153, "Completed JIT fixture produced the wrong business value")
  line = subs[active]
  line.effect = "jit-" .. original_mode .. "-completed-" .. value
  subs[active] = line
  return selected, active
end)

aegisub.register_macro("Workspace JIT Verify", "Verify JIT restoration in the same managed Lua state without reloading", function(subs, selected, active)
  local restored = jit.status()
  assert(invocation_count == 1, "Verifier did not observe exactly one invocation in the same Lua state")
  assert(observed_workspace_mode == false, "Verifier did not observe scoped interpreted execution")
  assert(restored == expected_engine, "Workspace did not restore the original JIT engine mode")
  assert(prewarmed == expected_engine, "Verifier lost the target-function prewarm evidence")
  local label = "JIT restored|" .. original_mode .. "|" .. outcome .. "|engine=" .. tostring(restored)
    .. "|prewarmed=" .. tostring(prewarmed) .. "|workspace=" .. tostring(observed_workspace_mode)
    .. "|runs=" .. invocation_count
  local button = aegisub.dialog.display({{class="label", label=label, x=0, y=0}}, {"OK"})
  assert(button == "OK", "JIT restoration acknowledgement was not accepted")
  return selected, active
end)
