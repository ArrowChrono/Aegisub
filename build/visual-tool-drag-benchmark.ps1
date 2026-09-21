[CmdletBinding()]
param(
    [string]$AegisubExe = "build-dir/RelWithDebInfo/Aegisub.exe",
    [string]$Video = "build-dir/artifacts/visual-tool-drag/baseline-640x480-square.mp4",
    [string]$Driver = "tests/gui-automation/visual-tool-drag-uia.cs",
    [ValidateSet("throughput-matrix", "paced-hold")]
    [string]$RunMode = "throughput-matrix",
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$Label = "current",
    [ValidateRange(100, 100000)]
    [int]$Motions = 128,
    [ValidateRange(1, 16)]
    [int]$MotionRepeat = 1,
    # Requested pacing for synthetic PostMessage input, not physical-button state.
    # Real/system mouse events can interrupt capture; early loss fails the run
    # and cannot by itself establish a product regression.
    [ValidateRange(0, 1000)]
    [int]$MotionIntervalMilliseconds = 0,
    [ValidateNotNullOrEmpty()]
    [string]$SubtitleProvider = "libass",
    [ValidateRange(100, 250)]
    [int]$HoldMilliseconds = 150,
    [ValidateRange(2, 1000000)]
    [int]$SmallEvents = 32,
    [ValidateRange(2, 1000000)]
    [int]$LargeEvents = 10000,
    [ValidateRange(1, 20)]
    [int]$Repetitions = 1,
    [ValidateSet("markers", "legacy")]
    [string]$TraceWindow = "markers",
    [ValidateRange(1, 600)]
    [int]$DriverTimeoutSeconds = 45,
    [ValidateRange(1, 1800)]
    [int]$RunTimeoutSeconds = 150,
    [ValidateRange(1, 7200)]
    [int]$TotalTimeoutSeconds = 1200,
    [ValidateSet("off", "on")]
    [string[]]$TraceModes = @("off", "on"),
    [string]$ArtifactsRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw "visual-tool-drag-benchmark.ps1 requires PowerShell 7 or newer."
}
if ($TraceModes.Count -eq 0) {
    throw "TraceModes must contain at least one of: off, on."
}
$TraceModes = @($TraceModes | Select-Object -Unique)
if ($RunMode -eq "paced-hold" -and $TraceWindow -ne "markers") {
    throw "RunMode paced-hold requires TraceWindow markers."
}

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$AllowedArtifactsRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $RepoRoot "build-dir/artifacts"))

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $relative = [System.IO.Path]::GetRelativePath($Parent, $Path)
    return -not [System.IO.Path]::IsPathRooted($relative) -and
        $relative -ne ".." -and
        -not $relative.StartsWith("..$([System.IO.Path]::DirectorySeparatorChar)", [System.StringComparison]::Ordinal) -and
        -not $relative.StartsWith("..$([System.IO.Path]::AltDirectorySeparatorChar)", [System.StringComparison]::Ordinal)
}

function Get-DisplayPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-PathWithin -Path $Path -Parent $RepoRoot) {
        return [System.IO.Path]::GetRelativePath($RepoRoot, $Path).Replace('\', '/')
    }
    return "<external>/$([System.IO.Path]::GetFileName($Path))"
}

function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowEmptyString()][Parameter(Mandatory = $true)][string]$Content
    )

    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        [System.Text.UTF8Encoding]::new($false))
}

function Get-OptionalProperty {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function New-FailureRow {
    param(
        [Parameter(Mandatory = $true)][int]$RunIndex,
        [Parameter(Mandatory = $true)][string]$InputMode,
        [Parameter(Mandatory = $true)][string]$Size,
        [Parameter(Mandatory = $true)][string]$Selection,
        [Parameter(Mandatory = $true)][string]$Trace,
        [Parameter(Mandatory = $true)][string]$ErrorMessage
    )

    return [pscustomobject]@{
        label = $Label
        run_index = $RunIndex
        scenario = if ($InputMode -eq "throughput") { "$Size-$Selection" } else { "$Size-$Selection-$InputMode" }
        input_mode = $InputMode
        size = $Size
        selection = $Selection
        trace = $Trace
        trace_window_mode = $TraceWindow
        status = "failed"
        error = $ErrorMessage
        build_label = $null
        git_revision = $script:GitRevision
        git_dirty = $script:GitDirty
        renderer = $null
        subtitle_provider = $null
        requested_subtitle_provider = $SubtitleProvider
        video_width = $null
        video_height = $null
        subtitle_use_stc = $null
        event_count = $null
        motion_count = $null
        motion_repeat = $MotionRepeat
        motion_interval_requested_ms = $MotionIntervalMilliseconds
        post_ms = $null
        drain_ms = $null
        motions_per_second = $null
        capture_released = $null
        motion_barrier_reached = $null
        hold_requested_ms = $null
        hold_elapsed_ms = $null
        capture_retained_during_hold = $null
        drag_valid = $null
        undo_restored = $null
        trace_window_source = $null
        trace_window_ms = $null
        commit_count = $null
        coalesced_commit_count = $null
        skipped_no_change_commit_count = $null
        commit_ratio = $null
        commit_p50_ms = $null
        commit_p95_ms = $null
        commit_max_ms = $null
        marked_line_count = $null
        actual_text_change_count = $null
        unchanged_text_count = $null
        subtitle_update_p95_ms = $null
        display_render_count = $null
        presented_render_count = $null
        post_commit_presented_render_count = $null
        repaint_render_count = $null
        display_render_p95_ms = $null
        delivered_packets = $null
        dropped_packets = $null
        process_immediate_flushes = $null
        process_buffered_flushes = $null
        window_slow_scope_count = $null
        scene_cache_fill_count = $null
        scene_cache_fill_total_ms = $null
        scene_cache_reuse_count = $null
        scene_cache_reuse_total_ms = $null
        scene_cache_direct_count = $null
        scene_cache_direct_total_ms = $null
        scene_cache_direct_waiting_count = $null
        scene_cache_direct_warmup_count = $null
        scene_cache_direct_playback_count = $null
        scene_cache_direct_policy_count = $null
        scene_cache_direct_fallback_count = $null
        overlay_full_upload_count = $null
        overlay_full_upload_total_ms = $null
        overlay_dirty_upload_count = $null
        overlay_dirty_upload_total_ms = $null
        overlay_reuse_count = $null
        overlay_reuse_total_ms = $null
        overlay_hide_count = $null
        overlay_hide_total_ms = $null
        overlay_estimated_upload_bytes = $null
    }
}

function Invoke-DriverRun {
    param(
        [Parameter(Mandatory = $true)][string]$DotNet,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$VideoPath,
        [Parameter(Mandatory = $true)][string]$DriverPath,
        [Parameter(Mandatory = $true)][string]$RunArtifacts,
        [Parameter(Mandatory = $true)][string]$InputMode,
        [Parameter(Mandatory = $true)][string]$Size,
        [Parameter(Mandatory = $true)][string]$Selection,
        [Parameter(Mandatory = $true)][string]$Trace,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    [System.IO.Directory]::CreateDirectory($RunArtifacts) | Out-Null
    $arguments = @(
        $DriverPath,
        "--",
        "--exe", $Executable,
        "--video", $VideoPath,
        "--artifacts", $RunArtifacts,
        "--input-mode", $InputMode,
        "--size", $Size,
        "--selection", $Selection,
        "--trace", $Trace,
        "--trace-window", $TraceWindow,
        "--motions", $Motions.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        "--motion-repeat", $MotionRepeat.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        "--motion-interval-ms", $MotionIntervalMilliseconds.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        "--subtitle-provider", $SubtitleProvider,
        "--hold-ms", $HoldMilliseconds.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        "--small-events", $SmallEvents.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        "--large-events", $LargeEvents.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        "--timeout-seconds", $DriverTimeoutSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $DotNet
    $startInfo.WorkingDirectory = $RepoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $started = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        if (-not $process.Start()) {
            throw "Could not start the visual-tool drag driver."
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            $terminated = $false
            try {
                $process.Kill($true)
                $terminated = $process.WaitForExit(5000)
            }
            catch {
                Write-Warning "Could not terminate timed-out driver process $($process.Id): $_"
                $terminated = $process.HasExited
            }
            if (-not $terminated) {
                Write-Utf8NoBom `
                    -Path (Join-Path $RunArtifacts "driver.stdout.log") `
                    -Content "Driver output unavailable because the timed-out process did not exit."
                Write-Utf8NoBom `
                    -Path (Join-Path $RunArtifacts "driver.stderr.log") `
                    -Content "Timed-out process tree remained alive after the bounded termination wait."
                throw "Driver timed out after $TimeoutSeconds seconds and process $($process.Id) did not exit."
            }
        }
        $process.WaitForExit()
        if (-not $stdoutTask.Wait(5000) -or -not $stderrTask.Wait(5000)) {
            throw "Driver output streams did not close within the bounded drain timeout."
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        Write-Utf8NoBom -Path (Join-Path $RunArtifacts "driver.stdout.log") -Content $stdout
        Write-Utf8NoBom -Path (Join-Path $RunArtifacts "driver.stderr.log") -Content $stderr
        if ($timedOut) {
            throw "Driver timed out after $TimeoutSeconds seconds."
        }
        if ($process.ExitCode -ne 0) {
            $detail = if ([string]::IsNullOrWhiteSpace($stderr)) { $stdout.Trim() } else { $stderr.Trim() }
            throw "Driver exited with code $($process.ExitCode): $detail"
        }
    }
    finally {
        $started.Stop()
        $process.Dispose()
    }

    $resultPath = Join-Path $RunArtifacts "drag-result.json"
    if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        throw "Driver succeeded without producing drag-result.json."
    }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if ($result.input_mode -ne $InputMode -or
        $result.motion_count -ne $Motions -or
        $result.motion_repeat -ne $MotionRepeat -or
        $result.motion_interval_requested_ms -ne $MotionIntervalMilliseconds -or
        $result.requested_subtitle_provider -ne $SubtitleProvider -or
        $result.input.capture_released -ne $true -or
        $result.correctness.drag_valid -ne $true -or
        $result.correctness.undo_restored -ne $true) {
        throw "Driver result failed matrix correctness validation."
    }
    if (-not [string]::Equals(
        [string]$result.environment.SubtitleProvider,
        $SubtitleProvider,
        [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Driver did not verify the requested subtitles provider '$SubtitleProvider'."
    }
    if ($Trace -eq "on") {
        $commitCount = [int]$result.trace_metrics.Commit.Count
        if ($commitCount -lt 1 -or $commitCount -gt $Motions) {
            throw "Trace-on result contained $commitCount visual_tool.commit samples; expected between 1 and $Motions."
        }
        $expectedCoalesced = $Motions - $commitCount
        $expectedRatio = $commitCount / [double]$Motions
        if ([int]$result.trace_metrics.CoalescedCommitCount -ne $expectedCoalesced -or
            [int]$result.trace_metrics.SkippedNoChangeCommitCount -ne $expectedCoalesced -or
            [Math]::Abs([double]$result.trace_metrics.CommitRatio - $expectedRatio) -gt 1e-12) {
            throw "Trace-on result reported inconsistent commit coalescing metrics."
        }
        $displayRenderCount = [int]$result.trace_metrics.DisplayRender.Count
        if ([int]$result.trace_metrics.PresentedRenderCount +
            [int]$result.trace_metrics.RepaintRenderCount -ne $displayRenderCount) {
            throw "Trace-on result reported inconsistent display render counts."
        }
        $environment = $result.environment
        if ([string]::IsNullOrWhiteSpace([string]$environment.BuildLabel) -or
            $environment.BuildLabel -eq "unknown" -or
            -not [string]::Equals([string]$environment.Renderer, "OpenGL", [System.StringComparison]::OrdinalIgnoreCase) -or
            [int]$environment.VideoWidth -ne 640 -or
            [int]$environment.VideoHeight -ne 480 -or
            $environment.SubtitleUseStc -ne $true) {
            throw "Trace-on result reported incomplete or unexpected benchmark environment metadata."
        }
        $actualTextChangeCount = [int]$result.trace_metrics.ActualTextChangeCount
        $overrideCount = $actualTextChangeCount + [int]$result.trace_metrics.UnchangedTextCount
        $markedLineCount = [int]$result.trace_metrics.MarkedLineCount
        $expectedOverrideCount = $Motions * [int]$result.selected_line_count
        if ($overrideCount -ne $expectedOverrideCount -or
            $markedLineCount -lt $actualTextChangeCount -or
            $markedLineCount -gt $overrideCount) {
            throw "Trace-on result reported inconsistent visual-tool override counts."
        }
        $sceneCacheOutcomeCount = [int]$result.trace_metrics.SceneCacheFill.Count +
            [int]$result.trace_metrics.SceneCacheReuse.Count +
            [int]$result.trace_metrics.SceneCacheDirect.Count
        if ($sceneCacheOutcomeCount -ne $displayRenderCount) {
            throw "Trace-on result reported incomplete scene-cache render outcomes."
        }
        $overlayActionCount = [int]$result.trace_metrics.OverlayFullUpload.Count +
            [int]$result.trace_metrics.OverlayDirtyUpload.Count +
            [int]$result.trace_metrics.OverlayReuse.Count +
            [int]$result.trace_metrics.OverlayHide.Count
        if (($InputMode -eq "paced-hold" -and $overlayActionCount -lt 1) -or
            (([int]$result.trace_metrics.OverlayFullUpload.Count +
                [int]$result.trace_metrics.OverlayDirtyUpload.Count) -gt 0 -and
                [long]$result.trace_metrics.OverlayEstimatedUploadBytes -lt 1)) {
            throw "Trace-on result reported incomplete overlay upload metrics."
        }
        $expectedWindowSource = if ($TraceWindow -eq "markers") {
            "interaction-markers"
        }
        else {
            "legacy-first-render-after-final-commit"
        }
        if ($result.trace_metrics.WindowSource -ne $expectedWindowSource) {
            throw "Trace-on result used an unexpected trace window source."
        }
        if ($InputMode -eq "paced-hold") {
            if ($result.input.motion_barrier_reached -ne $true -or
                $result.input.capture_retained_during_hold -ne $true -or
                [int]$result.input.hold_requested_ms -ne $HoldMilliseconds -or
                [double]$result.input.hold_elapsed_ms -lt $HoldMilliseconds) {
                throw "Paced-hold result failed barrier, hold-duration, or capture-retention validation."
            }
            if ([int]$result.trace_metrics.PresentedRendersAfterFinalCommitBeforeInteractionEnd -lt 1) {
                throw "Paced-hold trace had no presented render after the final commit and before interaction_end."
            }
        }
    }
    return $result
}

$resolvedExecutable = Resolve-RepoPath $AegisubExe
$resolvedVideo = Resolve-RepoPath $Video
$resolvedDriver = Resolve-RepoPath $Driver
foreach ($requiredFile in @($resolvedExecutable, $resolvedVideo, $resolvedDriver)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required benchmark file was not found: $(Get-DisplayPath $requiredFile)"
    }
}

$dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnetCommand) {
    throw "dotnet was not found. The UIA benchmark requires the local .NET SDK."
}

$script:GitRevision = "unknown"
$script:GitDirty = $null
$gitCommand = Get-Command git -ErrorAction SilentlyContinue
if ($gitCommand) {
    $revisionOutput = & $gitCommand.Source rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $revisionOutput) {
        $script:GitRevision = ($revisionOutput | Select-Object -First 1).Trim()
        $statusOutput = & $gitCommand.Source status --porcelain 2>$null
        if ($LASTEXITCODE -eq 0) {
            $script:GitDirty = @($statusOutput).Count -gt 0
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ArtifactsRoot)) {
    $runName = "{0}-{1}-{2}" -f (
        [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss", [System.Globalization.CultureInfo]::InvariantCulture)),
        $PID,
        $Label
    $resolvedArtifacts = Join-Path $AllowedArtifactsRoot "visual-tool-drag/matrix/$runName"
}
else {
    $resolvedArtifacts = Resolve-RepoPath $ArtifactsRoot
}
$resolvedArtifacts = [System.IO.Path]::GetFullPath($resolvedArtifacts)
if (-not (Test-PathWithin -Path $resolvedArtifacts -Parent $AllowedArtifactsRoot)) {
    throw "ArtifactsRoot must be inside build-dir/artifacts."
}
if (Test-Path -LiteralPath $resolvedArtifacts) {
    throw "ArtifactsRoot already exists; benchmark runs never overwrite artifacts: $(Get-DisplayPath $resolvedArtifacts)"
}
[System.IO.Directory]::CreateDirectory($resolvedArtifacts) | Out-Null

$scenarios = if ($RunMode -eq "paced-hold") {
    @([pscustomobject]@{ InputMode = "paced-hold"; Size = "small"; Selection = "single" })
}
else {
    @(
        [pscustomobject]@{ InputMode = "throughput"; Size = "small"; Selection = "single" },
        [pscustomobject]@{ InputMode = "throughput"; Size = "small"; Selection = "multi" },
        [pscustomobject]@{ InputMode = "throughput"; Size = "large"; Selection = "single" },
        [pscustomobject]@{ InputMode = "throughput"; Size = "large"; Selection = "multi" }
    )
}
$effectiveTraceModes = if ($RunMode -eq "paced-hold") { @("on") } else { $TraceModes }
$matrixTimer = [System.Diagnostics.Stopwatch]::StartNew()
$rows = [System.Collections.Generic.List[object]]::new()

for ($runIndex = 1; $runIndex -le $Repetitions; ++$runIndex) {
    foreach ($trace in $effectiveTraceModes) {
        foreach ($scenario in $scenarios) {
            $remainingSeconds = $TotalTimeoutSeconds - [int][Math]::Ceiling($matrixTimer.Elapsed.TotalSeconds)
            if ($remainingSeconds -le 0) {
                $rows.Add((New-FailureRow `
                    -RunIndex $runIndex `
                    -InputMode $scenario.InputMode `
                    -Size $scenario.Size `
                    -Selection $scenario.Selection `
                    -Trace $trace `
                    -ErrorMessage "Matrix total timeout reached before this run started."))
                continue
            }

            $runTimeout = [Math]::Min($RunTimeoutSeconds, $remainingSeconds)
            $scenarioName = if ($scenario.InputMode -eq "throughput") {
                "$($scenario.Size)-$($scenario.Selection)-trace-$trace"
            }
            else {
                "$($scenario.Size)-$($scenario.Selection)-$($scenario.InputMode)-trace-$trace"
            }
            $runDirectory = "run-{0:D2}" -f $runIndex
            $runArtifacts = Join-Path $resolvedArtifacts "$runDirectory/$scenarioName"
            Write-Host "[$Label run=$runIndex] $scenarioName (timeout=${runTimeout}s)"
            try {
                $result = Invoke-DriverRun `
                    -DotNet $dotnetCommand.Source `
                    -Executable $resolvedExecutable `
                    -VideoPath $resolvedVideo `
                    -DriverPath $resolvedDriver `
                    -RunArtifacts $runArtifacts `
                    -InputMode $scenario.InputMode `
                    -Size $scenario.Size `
                    -Selection $scenario.Selection `
                    -Trace $trace `
                    -TimeoutSeconds $runTimeout
                $traceMetrics = Get-OptionalProperty $result "trace_metrics"
                $commit = Get-OptionalProperty $traceMetrics "Commit"
                $subtitleUpdate = Get-OptionalProperty $traceMetrics "SubtitleUpdate"
                $displayRender = Get-OptionalProperty $traceMetrics "DisplayRender"
                $environment = Get-OptionalProperty $result "environment"
                $sceneCacheFill = Get-OptionalProperty $traceMetrics "SceneCacheFill"
                $sceneCacheReuse = Get-OptionalProperty $traceMetrics "SceneCacheReuse"
                $sceneCacheDirect = Get-OptionalProperty $traceMetrics "SceneCacheDirect"
                $sceneCacheDirectWaiting = Get-OptionalProperty $traceMetrics "SceneCacheDirectWaiting"
                $sceneCacheDirectWarmup = Get-OptionalProperty $traceMetrics "SceneCacheDirectWarmup"
                $sceneCacheDirectPlayback = Get-OptionalProperty $traceMetrics "SceneCacheDirectPlayback"
                $sceneCacheDirectPolicy = Get-OptionalProperty $traceMetrics "SceneCacheDirectPolicy"
                $sceneCacheDirectFallback = Get-OptionalProperty $traceMetrics "SceneCacheDirectFallback"
                $overlayFullUpload = Get-OptionalProperty $traceMetrics "OverlayFullUpload"
                $overlayDirtyUpload = Get-OptionalProperty $traceMetrics "OverlayDirtyUpload"
                $overlayReuse = Get-OptionalProperty $traceMetrics "OverlayReuse"
                $overlayHide = Get-OptionalProperty $traceMetrics "OverlayHide"
                $rows.Add([pscustomobject]@{
                    label = $Label
                    run_index = $runIndex
                    scenario = $result.scenario
                    input_mode = $result.input_mode
                    size = $result.size
                    selection = $result.selection
                    trace = $result.trace
                    trace_window_mode = $result.trace_window_mode
                    status = "passed"
                    error = ""
                    build_label = Get-OptionalProperty $environment "BuildLabel"
                    git_revision = $script:GitRevision
                    git_dirty = $script:GitDirty
                    renderer = Get-OptionalProperty $environment "Renderer"
                    subtitle_provider = Get-OptionalProperty $environment "SubtitleProvider"
                    requested_subtitle_provider = $result.requested_subtitle_provider
                    video_width = Get-OptionalProperty $environment "VideoWidth"
                    video_height = Get-OptionalProperty $environment "VideoHeight"
                    subtitle_use_stc = Get-OptionalProperty $environment "SubtitleUseStc"
                    event_count = $result.event_count
                    motion_count = $result.motion_count
                    motion_repeat = $result.motion_repeat
                    motion_interval_requested_ms = $result.motion_interval_requested_ms
                    post_ms = [double]$result.input.post_ms
                    drain_ms = [double]$result.input.drain_ms
                    motions_per_second = [double]$result.input.motions_per_second
                    capture_released = [bool]$result.input.capture_released
                    motion_barrier_reached = [bool]$result.input.motion_barrier_reached
                    hold_requested_ms = [int]$result.input.hold_requested_ms
                    hold_elapsed_ms = [double]$result.input.hold_elapsed_ms
                    capture_retained_during_hold = [bool]$result.input.capture_retained_during_hold
                    drag_valid = [bool]$result.correctness.drag_valid
                    undo_restored = [bool]$result.correctness.undo_restored
                    trace_window_source = Get-OptionalProperty $traceMetrics "WindowSource"
                    trace_window_ms = Get-OptionalProperty $traceMetrics "WindowMilliseconds"
                    commit_count = Get-OptionalProperty $commit "Count"
                    coalesced_commit_count = Get-OptionalProperty $traceMetrics "CoalescedCommitCount"
                    skipped_no_change_commit_count = Get-OptionalProperty $traceMetrics "SkippedNoChangeCommitCount"
                    commit_ratio = Get-OptionalProperty $traceMetrics "CommitRatio"
                    commit_p50_ms = Get-OptionalProperty $commit "P50Milliseconds"
                    commit_p95_ms = Get-OptionalProperty $commit "P95Milliseconds"
                    commit_max_ms = Get-OptionalProperty $commit "MaxMilliseconds"
                    marked_line_count = Get-OptionalProperty $traceMetrics "MarkedLineCount"
                    actual_text_change_count = Get-OptionalProperty $traceMetrics "ActualTextChangeCount"
                    unchanged_text_count = Get-OptionalProperty $traceMetrics "UnchangedTextCount"
                    subtitle_update_p95_ms = Get-OptionalProperty $subtitleUpdate "P95Milliseconds"
                    display_render_count = Get-OptionalProperty $displayRender "Count"
                    presented_render_count = Get-OptionalProperty $traceMetrics "PresentedRenderCount"
                    post_commit_presented_render_count = Get-OptionalProperty $traceMetrics "PresentedRendersAfterFinalCommitBeforeInteractionEnd"
                    repaint_render_count = Get-OptionalProperty $traceMetrics "RepaintRenderCount"
                    display_render_p95_ms = Get-OptionalProperty $displayRender "P95Milliseconds"
                    delivered_packets = Get-OptionalProperty $traceMetrics "DeliveredPackets"
                    dropped_packets = Get-OptionalProperty $traceMetrics "DroppedPackets"
                    process_immediate_flushes = Get-OptionalProperty $traceMetrics "ProcessImmediateFlushes"
                    process_buffered_flushes = Get-OptionalProperty $traceMetrics "ProcessBufferedFlushes"
                    window_slow_scope_count = Get-OptionalProperty $traceMetrics "WindowSlowScopeCount"
                    scene_cache_fill_count = Get-OptionalProperty $sceneCacheFill "Count"
                    scene_cache_fill_total_ms = Get-OptionalProperty $sceneCacheFill "TotalMilliseconds"
                    scene_cache_reuse_count = Get-OptionalProperty $sceneCacheReuse "Count"
                    scene_cache_reuse_total_ms = Get-OptionalProperty $sceneCacheReuse "TotalMilliseconds"
                    scene_cache_direct_count = Get-OptionalProperty $sceneCacheDirect "Count"
                    scene_cache_direct_total_ms = Get-OptionalProperty $sceneCacheDirect "TotalMilliseconds"
                    scene_cache_direct_waiting_count = Get-OptionalProperty $sceneCacheDirectWaiting "Count"
                    scene_cache_direct_warmup_count = Get-OptionalProperty $sceneCacheDirectWarmup "Count"
                    scene_cache_direct_playback_count = Get-OptionalProperty $sceneCacheDirectPlayback "Count"
                    scene_cache_direct_policy_count = Get-OptionalProperty $sceneCacheDirectPolicy "Count"
                    scene_cache_direct_fallback_count = Get-OptionalProperty $sceneCacheDirectFallback "Count"
                    overlay_full_upload_count = Get-OptionalProperty $overlayFullUpload "Count"
                    overlay_full_upload_total_ms = Get-OptionalProperty $overlayFullUpload "TotalMilliseconds"
                    overlay_dirty_upload_count = Get-OptionalProperty $overlayDirtyUpload "Count"
                    overlay_dirty_upload_total_ms = Get-OptionalProperty $overlayDirtyUpload "TotalMilliseconds"
                    overlay_reuse_count = Get-OptionalProperty $overlayReuse "Count"
                    overlay_reuse_total_ms = Get-OptionalProperty $overlayReuse "TotalMilliseconds"
                    overlay_hide_count = Get-OptionalProperty $overlayHide "Count"
                    overlay_hide_total_ms = Get-OptionalProperty $overlayHide "TotalMilliseconds"
                    overlay_estimated_upload_bytes = Get-OptionalProperty $traceMetrics "OverlayEstimatedUploadBytes"
                })
            }
            catch {
                $rows.Add((New-FailureRow `
                    -RunIndex $runIndex `
                    -InputMode $scenario.InputMode `
                    -Size $scenario.Size `
                    -Selection $scenario.Selection `
                    -Trace $trace `
                    -ErrorMessage $_.Exception.Message))
                Write-Warning "[$Label] $scenarioName failed: $($_.Exception.Message)"
            }
        }
    }
}
$matrixTimer.Stop()

$summary = [ordered]@{
    version = 6
    label = $Label
    run_mode = $RunMode
    git_revision = $script:GitRevision
    git_dirty = $script:GitDirty
    executable = Get-DisplayPath $resolvedExecutable
    executable_sha256 = (Get-FileHash -LiteralPath $resolvedExecutable -Algorithm SHA256).Hash
    video = Get-DisplayPath $resolvedVideo
    video_sha256 = (Get-FileHash -LiteralPath $resolvedVideo -Algorithm SHA256).Hash
    driver = Get-DisplayPath $resolvedDriver
    driver_sha256 = (Get-FileHash -LiteralPath $resolvedDriver -Algorithm SHA256).Hash
    runner = Get-DisplayPath $PSCommandPath
    runner_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    motions = $Motions
    motion_repeat = $MotionRepeat
    motion_interval_requested_ms = $MotionIntervalMilliseconds
    requested_subtitle_provider = $SubtitleProvider
    hold_milliseconds = if ($RunMode -eq "paced-hold") { $HoldMilliseconds } else { 0 }
    small_events = $SmallEvents
    large_events = $LargeEvents
    repetitions = $Repetitions
    trace_window_mode = $TraceWindow
    elapsed_seconds = $matrixTimer.Elapsed.TotalSeconds
    trace_on_note = "Trace-on is diagnostic only because trace flushing can perturb UI-thread timings."
    results = @($rows)
}
$summaryPath = Join-Path $resolvedArtifacts "summary.json"
$csvPath = Join-Path $resolvedArtifacts "summary.csv"
Write-Utf8NoBom -Path $summaryPath -Content ($summary | ConvertTo-Json -Depth 10)
$rows | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding utf8NoBOM

Write-Host "summary=$(Get-DisplayPath $summaryPath)"
Write-Host "csv=$(Get-DisplayPath $csvPath)"
$failures = @($rows | Where-Object status -ne "passed")
if ($failures.Count -gt 0) {
    throw "$($failures.Count) benchmark scenario(s) failed. See summary.json for details."
}
