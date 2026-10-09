# Prepared SceneChange request E2E

This scenario verifies an API contract: a prepared keyframe request retains its
selected backend and cache token while a later request observes the current
preference. It deliberately changes the preference programmatically between
preparation and execution. It is **not a reproduction of a normal wx confirmation
dialog interaction**; that modal dialog disables the other windows in the same
process, including the ordinary Preferences route.

The test uses one real `LsmasNative` video provider and both real SceneChange
provider DLLs. It does not mock the decoder, scanner, keyframe writer, or backend
selector. An independent `agi::Options` instance with `FLUSH_SKIP` supplies the
test preference; it never reads or writes the user's profile. No application
setting is saved and temporarily restored to implement request execution.

The shared provider and `AsyncVideoProvider` API no longer exposes
`CanGenerateSceneChangeKeyframes` or `GetSceneChangeKeyframeCacheToken`.
Call `PrepareSceneChangeKeyframes` to obtain an optional request value, derive
the cache token from that value, and pass the same request as the first argument
to `GenerateSceneChangeKeyframes`. Project's public menu-facing
`CanGenerateSceneChangeKeyframes` remains available. A request freezes only the
backend API snapshot: it does not bind a video or borrow its handle. Keep the
intended provider/video instance alive for execution; this scenario retains the
same provider throughout all preparations and scans.

## Build and runtime prerequisites

Run commands from the repository root. Build `scenechange-provider-smoke` using
the existing build directory and configuration, normally:

```powershell
cmake --build build-dir --config RelWithDebInfo --target scenechange-provider-smoke --parallel
```

Use the configured CMake executable if it is not on `PATH`. Keep the existing
adjacent runtime DLL layout, including LsmasNative and both scxvid and WWXD
providers. Do not replace the DLLs with stubs or configure an alternate backend
to make a missing runtime pass.

On Windows, launch the following media preparation and smoke commands through
an owned, inactive private-desktop supervisor. The existing native GUI driver in
`tests/gui-automation/driver/PrivateDesktop.cs` provides that isolation; an
existing bounded private-command wrapper may also be reused. Do not switch to
the desktop, send global input, or access the normal application profile. Use a
finite outer deadline, and treat timeout or a nonzero exit status as failure.

## Representative fixture

Use a fresh artifact directory and generate a lossless 640-by-360, 25 fps video
containing four distinct 20-second segments: black, white, blue, and moving
`testsrc2` content. The result contains 2,000 frames, static scene cuts, and motion.
The four scans exercise 8,000 real decoded/analyzed frames on the same provider.

```powershell
$artifacts = 'build-dir/artifacts/scenechange-prepared-backend'
if (Test-Path $artifacts) { throw 'Use a fresh artifact directory' }
New-Item -ItemType Directory $artifacts | Out-Null
ffmpeg -nostdin -y -f lavfi -i "color=black:s=640x360:r=25:d=20" -f lavfi -i "color=white:s=640x360:r=25:d=20" -f lavfi -i "color=blue:s=640x360:r=25:d=20" -f lavfi -i "testsrc2=s=640x360:r=25:d=20" -filter_complex "[0:v][1:v][2:v][3:v]concat=n=4:v=1:a=0,format=yuv420p[v]" -map "[v]" -c:v ffv1 -threads 2 "$artifacts/scenes.mkv"
if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed' }
```

Preserve the fixture and its SHA-256. A previously generated identical fixture
can be reused without rerunning ffmpeg, but smoke outputs must be new.

## Execute and retain evidence

The new mode takes exactly three arguments after the executable. Existing
`<video> <output.kf.txt> [expected-frames] [backend] [cancel-at]` argument meanings
are unchanged. Run this block inside the owned private-desktop worker, with an
outer supervisor deadline of 180 seconds for the smoke run:

```powershell
$ErrorActionPreference = 'Stop'
$artifacts = 'build-dir/artifacts/scenechange-prepared-backend'
$exe = 'build-dir/RelWithDebInfo/scenechange-provider-smoke.exe'
$process = $null
$result = [ordered]@{ passed = $false; timeout_seconds = 120; exit_code = $null }
try {
    $process = Start-Process -FilePath $exe -ArgumentList @("$artifacts/scenes.mkv", "$artifacts/request", '--prepared-switch') -WindowStyle Hidden -PassThru -RedirectStandardOutput "$artifacts/smoke.stdout.log" -RedirectStandardError "$artifacts/smoke.stderr.log"
    if (!$process.WaitForExit(120000)) {
        $process.Kill($true)
        throw 'Prepared backend smoke exceeded 120 seconds'
    }
    $process.Refresh()
    $result.exit_code = $process.ExitCode
    if ($process.ExitCode -ne 0) { throw "Prepared backend smoke failed: $($process.ExitCode)" }
    $result.passed = $true
} finally {
    if ($process) { $process.Dispose() }
    $result.revision = git rev-parse HEAD
    $result.executable_sha256 = (Get-FileHash $exe).Hash
    $result.video_sha256 = (Get-FileHash "$artifacts/scenes.mkv").Hash
    $result | ConvertTo-Json | Set-Content "$artifacts/result.json"
}
```

Keep the private-desktop supervisor's ownership, timeout, and exit-status
evidence alongside `result.json`, both output logs, and the four generated
`request.*.kf.txt` files. Record the built target and configuration and preserve
the source diff or source hashes for an uncommitted build; the revision alone
does not identify uncommitted source. No successful output from an earlier build
is evidence for a later one.

## Assertions

| Output suffix | Preference at preparation | Preference at execution | Request token | Actual scanner |
| --- | --- | --- | --- | --- |
| `scxvid-prepared` | `scxvid` | `wwxd` | `scxvid` | `scxvid` |
| `wwxd-fresh` | `wwxd` | `wwxd` | `wwxd` | `wwxd-provider` |
| `wwxd-prepared` | `wwxd` | `scxvid` | `wwxd` | `wwxd-provider` |
| `scxvid-fresh` | `scxvid` | `scxvid` | `scxvid` | `scxvid` |

Each run must finish scanning the complete frame count with monotonic progress,
publish a readable, nonempty, strictly increasing keyframe vector within the
video frame range, and produce exactly one scanner event in section
`provider/lsmasnative/scenechange` with message
`Scanning scene changes with backend: <expected backend>`.

That production event comes from the actual `Api` passed into
`ScanSceneChangeKeyframes`, which supplies the native create/frame-commit calls;
it is not synthesized by the test from a cache token or current preference.
The test observes it through a log emitter and independently compares it with
the table above. The event alone is not success: full progress and successful
keyframe publication are also required. The entire prepared and fresh vectors
must match for each backend. Different algorithms are not required to produce
different cuts on a particular fixture.

The stdout log reports each scenario, execution-time preference, request token,
observed scanner event, frame count, and full keyframe list. Only completion of
all four cases prints `prepared-switch=passed` and exits with status zero.

This test covers the provider request boundary, not Project's GUI confirmation,
cache transaction, or manifest handling. Continue running the original provider
generation/cancellation checks and the separate native cache transaction E2Es
in `tests/gui-automation/scenechange-cache-native.md` for cancellation and failed
publication behavior.
