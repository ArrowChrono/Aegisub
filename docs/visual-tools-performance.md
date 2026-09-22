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
  interaction was already active. Ordinary hover is not recorded.
- `video_controller.subtitle_update.request.commit` and `.request.frame`:
  `detail_a` says whether the subtitle deadline admitted the request.
- `visual_tool.interaction_render.request`: admission by the feedback deadline.
- `*.timer_arm`: `detail_a` is the requested one-shot delay in milliseconds.
- `*.timer_fire`: `detail_a` is deadline admission, `detail_b` says whether a
  pending deadline existed before the decision.
- `*.timer_lateness`: `duration_ms` is **signed deadline lateness**, not callback
  execution duration. Negative values mean an early callback. The deadline and
  decision use the same sampled `now`.
- `video_display.scheduled_render`: a queued render request is about to execute.
  Paint scopes and the existing interaction render reason remain available.

The subtitle and feedback gates remain 33 ms and 16 ms respectively. Both use
the GUI event loop; they are not guarantees of frame rate or maximum latency.

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
