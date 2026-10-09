# Native SceneChange cache transaction E2E

Run from the repository root on Windows with .NET 10, the real wx Aegisub
application, LsmasNative, and the WWXD SceneChange runtime. Keep the application's
normal adjacent runtime layout; a GUI-test build without SceneChange DLLs cannot
exercise this scenario. Each invocation needs a **new, empty** artifact directory
under `build-dir/artifacts`.

## Reproducible multi-scene fixture

Create four distinct 20-second scenes at 640×360 and 25 fps, then repeat the
sequence five times. The final fixture has 10,000 frames and 20 scene segments,
including both static cuts and moving test-pattern content. Its scan is long
enough to observe and cancel actual nonzero progress, rather than cancelling a
confirmation or accepting an already completed operation.

```powershell
New-Item -ItemType Directory -Force build-dir/artifacts/scenechange-fixture | Out-Null
ffmpeg -nostdin -y -f lavfi -i "color=black:s=640x360:r=25:d=20" -f lavfi -i "color=white:s=640x360:r=25:d=20" -f lavfi -i "color=blue:s=640x360:r=25:d=20" -f lavfi -i "testsrc2=s=640x360:r=25:d=20" -filter_complex "[0:v][1:v][2:v][3:v]concat=n=4:v=1:a=0,format=yuv420p[v]" -map "[v]" -c:v ffv1 -threads 2 build-dir/artifacts/scenechange-fixture/scenes.mkv
ffmpeg -nostdin -y -stream_loop 4 -i build-dir/artifacts/scenechange-fixture/scenes.mkv -c copy build-dir/artifacts/scenechange-fixture/scenes-long.mkv
```

The ffmpeg commands only prepare media. The driver launches every Aegisub host
and its own worker on an owned private, inactive WinSta0 desktop, using the
existing `PrivateDesktop` supervisor. It never calls `SwitchDesktop`, sends
global keyboard/mouse input, uses the clipboard, or accesses the normal user
profile. Native messages target only verified, live host-owned HWNDs. Windows
TaskDialog body text and common-button identities are observed through UIA;
button clicks are local `BM_CLICK` posts. File names are entered into the native
Open/Save file dialogs, and menu commands come from the actual default Video menu.

```powershell
dotnet build tests/gui-automation/scenechange-cache-native.cs -o build-dir/artifacts/scenechange-driver-bin
dotnet run --file tests/gui-automation/scenechange-cache-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/scenechange-fixture/scenes-long.mkv build-dir/artifacts/scenechange-cancel cancel
dotnet run --file tests/gui-automation/scenechange-cache-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/scenechange-fixture/scenes-long.mkv build-dir/artifacts/scenechange-publication publication
dotnet run --file tests/gui-automation/scenechange-cache-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/scenechange-fixture/scenes-long.mkv build-dir/artifacts/scenechange-publication-cancel publication-cancel
```

Each invocation has a 180-second total watchdog and the supervisor has a
210-second outer deadline. Individual GUI waits are bounded; generation has a
75-second wait. A timeout is a failure, not a reason to rerun the same scenario
or extend its deadline. The only ignored polling error is Win32 invalid-window
handle 1400 while a transient dialog is being destroyed; this does not repeat a
user action or an assertion. Coordinate shared builds and native GUI runs with
other sessions. A deliberately requested new generation after cancellation or
release of the lock is a separate user operation, not a test retry.

## Shared setup and independent observations

All modes use a fresh isolated profile with `Video/Provider=LsmasNative`,
`Provider/SceneChange/Backend=wwxd`, and automatic audio opening disabled.

1. Open the real video through **Video → Open Video...** and accept the
   missing-cache generation prompt. Require actual progress UI, successful
   completion, a WWXD-tagged cache containing at least four ordered keyframes,
   and a manifest entry identifying both the cache and video.
2. Export the current in-memory keyframes through **Save Keyframes...** and
   compare the entire vector with the published cache.
3. Close and reopen the same video. Require reuse without a generation prompt,
   identical cache/manifest bytes, and identical exported in-memory keyframes.
   Cache modification time is deliberately not an oracle: successful reuse
   calls `Touch` for cache eviction accounting.
4. Open **Show Video Details** and require its actual decoder field to be
   `LsmasNative`, rather than trusting configuration alone.
5. Replace the generated cache bytes with a legal, recognizable existing
   keyframe list `[0, 7, 31, 101]`, then load that file through **Open Keyframes...**.
   Export it to prove the active in-memory list is the sentinel. Require the
   sentinel to differ from the real scene scan. This controlled existing-cache
   fixture makes a later successful replacement distinguishable from returning
   the old cache as false success. The video, cache filename, and manifest
   identity are unchanged.

No internal Project state, scene detector, or keyframe writer is stubbed or
invoked directly. All scans and in-memory changes go through the real GUI
commands and LsmasNative/SceneChange path. The full vectors, cache bytes, and
manifest bytes are retained; there is no metadata filtering or partial-list
oracle.

## `cancel`: preserve the last usable transaction

Choose **Generate Scene Change Keyframes...** and accept the existing-cache
regeneration prompt. Observe the progress gauge strictly between 0 and 270 on
its 300-unit scale, capture the active dialog, and check that range and the
enabled Cancel button again immediately before clicking Cancel. The gauge is
animated UI progress, not an exact count of frames already scanned.

Require cancellation to finish without a publication error, with the previous
cache and manifest **byte-for-byte unchanged** and the native exported active
keyframes still exactly the sentinel. Then explicitly regenerate again and
require successful replacement with the original real scan vector, both on
disk and in memory. The manifest must retain its correct identity.

The unfixed Project cancellation handler removes the previously usable cache
and prunes its manifest despite the sentinel remaining active in memory. The
driver reports the disk and manifest failures separately, retaining all three
observations before failing the scene.

## `publication`: distinguish failed replacement from success

Hold the final cache open with
`FileStream(FileMode.Open, FileAccess.Read, FileShare.Read)`. This permits readers
but denies replacement/deletion of the final path; it does not lock the cache
directory or prevent creating the writer's temporary output. Regenerate through
the same real menu and confirmation.

Require an explicit **Error generating keyframes** dialog, not merely an idle
window, a readable old file, or a progress log. After acknowledging that error,
require unchanged cache bytes, manifest bytes, and active sentinel keyframes.
Also require no unpublished keyframe `_tmp_` file to remain. If the progress
runner presents a logged exception with a Close button, the driver records and
closes it, but **still requires** the explicit generation error afterward; Close
alone is not treated as successful error propagation.

Release the file handle and explicitly regenerate again. Require no error, real
scan output replacing the sentinel on disk, the same complete vector exported
from memory, and unchanged correct manifest identity. This proves recovery
after the external failure is removed.

The unfixed keyframe writer can swallow the final replacement error in its
destructor, leave a temporary file, and allow the caller to reload the old cache
as if generation succeeded. An explicit writer close must also propagate past
the GUI progress runner; simply displaying a progress log is insufficient.

## `publication-cancel`: retain a task failure when the runner is cancelled

Hold the same final-cache read lock, verify there is no stale temporary output,
and regenerate. Before the scan completes, observe the actual native Cancel
button. Then poll for this cache's new `_tmp_` file and require its complete
keyframe vector to equal the initial real scan. The writer only creates that
file after the full scan returns; the locked final path keeps it present while
the writer retries replacement for up to 900 milliseconds.

Immediately verify that temporary output still exists and Cancel is enabled,
then post the observed native button command. Do not take a screenshot, perform
UIA traversal, or write evidence files before this post. Record the observed
filename, control state, and elapsed time afterward, and require the actual
disabled **Cancelling...** control to prove the progress runner accepted the
request while publication was in progress.

The resulting task-level publication failure must still produce an explicit
**Error generating keyframes** dialog. Treating it as ordinary, error-free
cancellation is a failure: the progress runner's cancellation exception must
not hide the task's already-recorded filesystem error. Require unchanged cache,
manifest, and active keyframes, no temporary output left behind, and successful
replacement after releasing the lock and explicitly generating again. This is
distinct from `cancel`, which cancels during scanning before any publishable
output exists and must not display a publication error.

The total watchdog and outer process deadline are unchanged; observing the
publication window has a bounded 75-second wait. This mode targets the actual
replacement retry window rather than choosing a fixed delay or retrying a scene
until the timing happens to work.

## Artifacts and scope

Artifacts include `result.json` with individual checks, `driver.log`, the exact
producer source, executable/video/source/runtime-DLL hashes, backend selection,
action records, supervisor/process/private-desktop identity evidence, the
isolated host profile and its actual cache/manifest, full before/after snapshots,
native window/control JSON, TaskDialog UIA text, PNGs of confirmations and
generation boundaries, cancellation gauge samples, publication-cancel timing
and control state, lock semantics, and native
keyframe exports. Failed runs return nonzero and preserve their observations.

These scenarios verify WWXD-backed cache publication, reuse, cancellation,
recoverable Windows replacement failure, and GUI-visible state. They do not
claim coverage of other SceneChange backends, every media format, cache eviction
limits, power loss, or unrelated video/audio/grid behavior. The initial detector
result is used for transaction comparison rather than as an independent oracle
for the quality of the scene-detection algorithm.
