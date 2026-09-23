# Visual-tool preview timing

Use `AEGISUB_PERF_TRACE=video` to collect video timing events. Keep the input
protocol, fixture, executable, renderer, trace selection, and display conditions
constant when comparing runs. A requested mouse interval is not a guarantee of
the operating system's dispatch cadence. The posted-message benchmark does not
hold the operating system's mouse button; foreground native input is a separate
protocol, not an interchangeable result.

## Correlating the preview pipeline

`video_preview_pipeline` records numeric metadata, never subtitle text:

- `provider_version`, `content_version`, `request_version`: the existing packet
  delivery version. Join stages using the entire tuple, not just frame number.
- `delivery_class`: 0 = EveryFrame, 1 = VisualSubtitleIntermediate,
  2 = VisualSubtitleFinal. Submit events describe the incoming update options;
  worker and packet events describe the effective class after pending updates
  have been merged.
- `interaction_id`: existing visual interaction identifier. It is diagnostic
  context, **not** an alternative to the strict content/request validity checks.
- `frame`: -1 if the submitter does not have a pending frame context. A subtitle
  submission can reuse the worker's current frame without a new frame request.
- `duration_ms`: present on completed worker render events; measures the existing
  `ProcRenderPacket` scope, not all worker work or input-to-display latency.

Typical stages are:

| Stage | Meaning |
| --- | --- |
| `submit_load`, `submit_update` | Subtitle snapshot merged into pending work; version and timestamp captured under the pending mutex |
| `worker_take` | Worker took the latest pending snapshot |
| `worker_skip_no_frame`, `worker_skip_no_change`, `worker_skip_stale` | Worker did not enter the renderer; the stage states why |
| `worker_render_begin` | Entering the render path, before the trace call and render duration timer |
| `worker_render_deliver`, `worker_render_drop` | Render completed; passed or failed the worker validity gate |
| `host_enqueue`, `host_replace`, `host_dequeue` | Main-thread delivery scheduling; replacement identifies the displaced packet |
| `host_drop_old_interaction`, `host_drop_closed` | Delivery rejected by visual batch ordering/final rules |
| `gui_receive`, `gui_accept`, `gui_stale` | Main preview callback entered and applied its existing validity gate |
| `display_receive`, `display_replace` | Canvas received a packet or replaced an older pending packet |
| `display_upload_begin`, `display_upload_end` | Packet upload and display-state update boundaries |
| `display_present`, `display_swap_failed` | Swap succeeded or failed for a newly uploaded packet, before presentation listeners |

The trace also distinguishes current/stale video and subtitle errors on the
worker. `worker_render_deliver` does **not** mean GUI acceptance or presentation.
An enqueue can be written after a dequeue by another thread: timestamps are
captured at the relevant boundary, and trace writes occur outside business
mutexes. Sort by `t_monotonic_ns` and join by version rather than NDJSON line order.
For renderer cost, use `duration_ms`; a begin-to-end stage span can also contain
trace-lock/write overhead.

Not every submission reaches every stage. Pending work, host batches, and the
canvas can coalesce updates. Seeks and lifecycle operations also invalidate work.
An absent final stage is not automatically a renderer failure. These events do
not assign a new revision to each mouse motion or undo commit, so they must not
be described as per-motion input-to-photon measurements.

## Pacing and input markers

Existing `video_ui_duration` entries include:

- `visual_tool.input.down`, `.motion`, `.up`: GUI input-handler arrival;
  `detail_a` is the event's left-button state and `detail_b` says whether a visual
  interaction was already active. Ordinary hover is not recorded by these input
  markers; feedback-request scopes below distinguish hover and leave requests.
- `video_controller.subtitle_update.request.commit` and `.request.frame`:
  `detail_a` says whether the subtitle deadline admitted the request.
- `visual_tool.interaction_render.request`: admission by the feedback deadline.
- `*.timer_arm`: `detail_a` is the remaining one-shot delay rounded up to
  milliseconds (at least 1). The timer is armed at the original steady-clock
  deadline, without this diagnostic rounding.
- `*.timer_fire`: `detail_a` is deadline admission, `detail_b` says whether a
  pending deadline existed before the decision.
- `*.timer_lateness`: `duration_ms` is **signed deadline lateness**, not callback
  execution duration. Negative values mean an early callback. The deadline and
  decision use the same sampled `now`.
- `video_display.scheduled_render`: a queued render request is about to execute.
  Paint scopes and the existing interaction render reason remain available.
- On Windows, `video_display.context_pre_current`: `detail_a` says whether the
  thread's HGLRC already matched this display, and `detail_b` says whether its HDC
  matched. Both identities are sampled before activation; the event is emitted
  after the corresponding `context_activate` scope to avoid trace writes between
  the sample and the native bind. No native handle values are recorded. A slow
  activation alone does not prove that it was redundant or identify driver waits.
  The activation scope reports whether the target binding is ready; success does
  not imply a native bind was issued when the existing binding could be reused.

On Windows, video display activation reuses the binding only if live native
queries match both the display's HGLRC and its HDC. A valid context object alone,
or a remembered successful bind, is not sufficient: audio drawing, another
window, or detach/re-dock can change the current binding. Mismatches still use
the ordinary activation and failure path. Resource destruction and context-owned
texture retirement retain their existing ownership rules. Compare complete
presentation and final-packet latency, not just activation duration, when
evaluating this path: eliminating a bind must not merely move a wait elsewhere.

The subtitle and feedback gates remain 33 ms and 16 ms respectively. Their
one-shot `UiDeadlineTimer` waits off-thread and posts the callback to the GUI
dispatcher only once the steady-clock deadline has arrived. It remains running
while that callback is queued. Restart, Stop, and destruction invalidate queued
callbacks by generation; an old callback cannot consume a newer arm. All
subtitle, renderer, and display work still runs on its existing threads.

On Windows, these two timers prefer a high-resolution waitable timer, fall back
to an ordinary waitable timer when unavailable, and use a condition-variable
deadline wait if native creation or waiting fails. They do not use `SetTimer`,
its 10 ms minimum timeout parameter, or low-priority `WM_TIMER` delivery. This
also avoids a stopped wx native timer ID being reused while its old message is
still queued. No global timer-resolution request, polling loop, or busy wait is
used. Playback, audio, idle prefetch, and keyboard-nudge undo timers are unchanged.

This removes a wakeup limitation, not Windows scheduling latency: a busy GUI
thread, graphics driver, or renderer can still delay presentation, and fallback
timers can have lower precision. Neither gate guarantees frame rate or maximum
latency. The strict pacer admission and packet validity checks remain in place.

## Render-request provenance

The video profile can distinguish requests, queued callbacks and request
consumption without changing their scheduling. The following
`video_ui_duration` phases use `detail_a` and `detail_b`:

| Phase | `detail_a` | `detail_b` |
| --- | --- | --- |
| `video_display.render_request` | Request sequence | Request kind |
| `video_display.render_queue` | Queue sequence | Request sequence at enqueue |
| `video_display.render_callback` | Captured queue sequence | Current request sequence |
| `video_display.render_dispatch.*` | Current request sequence | State flags |
| `video_display.render_consume` | Current request sequence | State flags |

Request kinds are 1: `Render`, 2: `RenderNow`, 3: deferred subtitle packet,
4: render reentry, 5: frame-dependent overlay follow-up, 6: idle promotion
of feedback, 7: paused pure tool feedback, and 8: release-window feedback.
Kinds 1 through 6 record `render_requested = true` writes; kinds 7 and 8 mark
only tool feedback dirty. Neither guarantees a new queue entry or presentation.
The dispatch suffix is
`backend_change`, `packet`, `now`, `paint`, `idle`, or `scheduled`.

State flags are bitwise: 1 playing, 2 tool interacting, 4 pending packet,
8 packet deferred for interaction, 16 render in progress, 32 render requested,
64 tool feedback dirty, and 128 render scheduled. `render_consume` is immediately
before the existing request clear, after the existing in-progress/scheduled
assignments. A reentrant return does not consume requests. Consumption does not
imply a successful upload or Swap; retain the packet and render-result checks.

The request and queue sequences are trace-only, display-local positive integers.
They wrap to 1 after `INT_MAX`; reject exact attribution across a wrap, missing
events, or indistinguishable displays rather than guessing. These records do not
carry a display identity: exact reconstruction is limited to a verified
single-display run. An older queued callback can consume newer requests. Keep
all requests since the last consume boundary rather than a single last cause.

Three synchronous wrappers record a zero-duration `.begin` event and a duration
scope with the same base phase as its end event:

- `visual_tool.feedback_request`: `detail_a` is mouse flags (down 1, double-click
  2, up 4, moving 8, dragging 16, leaving 32, entering 64, left held 128).
  `detail_b` is interaction flags (active on entry 1, currently holding 2,
  currently dragging 4).
- `video_display.tool_feedback_request`: `detail_a` is playing and `detail_b`
  is interacting. During playback it can only mark feedback dirty, with no
  immediate render request.
- `video_display.subtitle_commit_render_request`: `detail_a` is commit type,
  `detail_b` is 0 (not interacting). It covers the branch that requests a repaint,
  including a holding tool's local release commit before pacing ends. The latter
  is pure tool feedback; other commits retain their ordinary repaint route.

Use the explicit begin/end nesting on the GUI thread to attribute requests.
Duration is sampled before the end event is emitted, so `event timestamp minus
duration` is only an approximate start, not an exact scope boundary. Unwrapped
`Render` requests remain generic/unknown; proximity to mouse-up is not proof of
a hover, commit or release origin. Active interaction-render reasons and the
`scheduled_render` marker retain their meanings. Pre-merge reference builds use
interaction-render reason 2 for the unconditional release redraw; the bounded
release path instead emits the release-feedback events below.

These observers themselves do not make scheduling decisions or insert GL
synchronization. Compare complete Final delivery and necessary repaint behavior,
not only individual GL-call durations.

## Bounded release feedback

A normal paused release with a submitted Final opens one fixed 16 ms merge
window for pure tool feedback. The Final is still submitted immediately, and
incoming packets retain the existing validity and presentation paths. Release
and subsequent hover/leave feedback can share that packet's render rather than
forcing an old-scene swap just before it. This is a new interval measured from
the release handoff, not the remaining active-drag pacing deadline. GUI/driver
stalls can delay execution beyond it; it is not a hard input-to-photon bound.

Both queued and idle dispatch check the same pure-feedback eligibility. Ordinary
repaint requests, paint/expose, packets and geometry changes bypass this gate.
No-Final clicks keep immediate feedback. A holding tool's final local commit is
classified as feedback only while its own commit scope and pacing session are
still active; external subtitle edits are not reclassified.

The queued feedback intent is consumed on a render attempt, preventing failed
renders from creating an idle retry loop. Separately, a successful Swap satisfies
the window's current feedback need. Only a newly presented matching Final,
deadline expiry or semantic invalidation ends the window; an ordinary successful
paint does not allow the next hover to bypass it. Repeated feedback never extends
the deadline. At expiry the window ends before at most one fallback redraw if
feedback still needs a successful presentation, including after a failed attempt.

The display lazily owns a `UiDeadlineTimer` for this one-shot fallback. New
interactions, cancellation/capture loss, seeks, geometry/tool/provider/backend
changes, playback and unload invalidate the release window. Timer cancellation
does not discard ordinary repaint requests. No extra redraw loop, busy wait,
global timer-resolution setting or new packet acceptance rule is introduced.

The `video_display.release_feedback.*` events distinguish `arm` (`detail_a` is
the interval in ms), `immediate` (no merge window), `presented` (`detail_a` is 1
only when the matching new Final closes the window), `deadline` (`detail_a` says
whether a fallback attempt is needed), and `cancel`. An ordinary successful
presentation reports `presented` with 0 and leaves the deadline fixed. These are
diagnostics, not substitutes for the exact Final packet chain.

## Reported presentation configuration

When video tracing is enabled, the first successful activation of each video
context emits two configuration observations. Unloading the context resets this
observation; it is not repeated per upload or automatically refreshed when the
window moves between monitors. Neither observation changes presentation policy.

- `video_display.swap_interval.reported`: `detail_a` is 0 on non-WGL platforms,
  1 when the WGL extension list is unavailable, 2 when `WGL_EXT_swap_control` is
  absent, or 3 when its getter is unavailable. These unknown states omit
  `detail_b`; they do not mean interval 0. Status 4 reports a nonnegative interval
  directly in `detail_b`. Status 5 encodes a negative interval as
  `-(interval + 1)`, so decode it as `-1 - detail_b`; this preserves adaptive -1
  despite the trace API omitting negative detail fields.
- `video_display.nominal_refresh_hz`: `detail_a` is 0 when no valid display is
  available, 1 when its refresh is unknown, or 2 when `detail_b` contains a
  positive nominal refresh rate in Hz.

The swap interval is the driver's reported value for the current drawable,
not proof of effective vsync, GPU queue depth, compositor behavior, or driver
overrides. The nominal integer refresh is not an exact refresh period or a VRR
measurement. No swap-interval setter, extra context activation, GL error query,
flush, fence, or swap is issued by these observers. Compare their payloads as
part of the environment, rather than treating mere event presence as a match.

## Observation overhead

Video-only tracing retains explicit lifecycle memory snapshots, including the
first presented frame, but does not request periodic worker memory collection.
Use `video,memory` to include that collection. The memory and audio profiles keep
their existing periodic sampling behavior. Memory collection can synchronously
wait for the worker, so exclude startup/lifecycle activity and do not mix trace
profiles in a timing comparison.

Video duration scopes no longer force a disk flush merely because a scope took
at least 8 ms. Events still use the existing bounded buffer (64 entries, 64 KiB,
or 250 ms checked on the next append), plus explicit flushes and shutdown.
Buffering is not asynchronous or zero-overhead: serialization, the shared trace
mutex, and bounded flushes can still perturb the result. Retain trace-off
correctness checks and report this limit.

Neither these diagnostics nor matching interaction identifiers relax stale
packet rejection, final-update ordering, seek isolation, or provider lifetime
checks. Those protections must remain independently tested when changing preview
scheduling.
