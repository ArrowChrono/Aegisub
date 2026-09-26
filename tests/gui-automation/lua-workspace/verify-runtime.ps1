#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Exe = 'build-dir/RelWithDebInfo/Aegisub.exe',
    [Parameter(Mandatory)][string]$Artifacts
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not (Test-Path -LiteralPath 'CMakeLists.txt')) { throw 'Run from the repository root' }
$exePath = [IO.Path]::GetFullPath($Exe)
$artifactRoot = [IO.Path]::GetFullPath($Artifacts)
if (Test-Path -LiteralPath $artifactRoot) { throw 'Use a fresh artifact directory' }
[IO.Directory]::CreateDirectory($artifactRoot) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$cases = @('runtime', 'runtime-noblank', 'runtime-furi')
$caseExpectations = @{
    'runtime' = @{ OriginalEventCount = 5; TemplateCount = 3; LastScope = 'syl'; LastSyllableText = 'delta' }
    'runtime-noblank' = @{ OriginalEventCount = 5; TemplateCount = 3; LastScope = 'syl'; LastSyllableText = 'delta' }
    'runtime-furi' = @{ OriginalEventCount = 6; TemplateCount = 4; LastScope = 'furi'; LastSyllableText = 'd' }
}
$results = [Collections.Generic.List[object]]::new()
$started = [DateTimeOffset]::UtcNow

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function TimeMs([string]$Value) {
    Require ($Value -match '^(\d+):(\d{2}):(\d{2})\.(\d{2})$') "Malformed ASS time: $Value"
    return (([int]$Matches[1] * 60 + [int]$Matches[2]) * 60 + [int]$Matches[3]) * 1000 + [int]$Matches[4] * 10
}

function Events([string]$Path) {
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ($line -notmatch '^(Dialogue|Comment): ') { continue }
        $kind = $Matches[1]
        $fields = $line.Substring($line.IndexOf(':') + 1).TrimStart().Split([char]',', 10)
        Require ($fields.Count -eq 10) 'Malformed ASS event'
        [pscustomobject][ordered]@{
            Kind = $kind; Layer = [int]$fields[0]; StartMs = (TimeMs $fields[1]); EndMs = (TimeMs $fields[2])
            Style = $fields[3]; Actor = $fields[4]; MarginL = [int]$fields[5]; MarginR = [int]$fields[6]
            MarginV = [int]$fields[7]; Effect = $fields[8]; Text = $fields[9]
        }
    }
}

function RunScenario([string]$Directory) {
    $start = [Diagnostics.ProcessStartInfo]::new($exePath)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('--headless', 'run', '--scenario', 'tests/gui-automation/lua-workspace/runtime-templater.json',
            '--input', "script=$Directory/kara-templater.lua", '--input', "subtitle=$Directory/input.ass",
            '--input', "output=$Directory/output.ass", '--artifacts', "$Directory/run")) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit(120000)
        if ($timedOut) {
            $process.Kill($true)
            Require ($process.WaitForExit(5000)) 'Timed-out headless process did not terminate'
        }
        [IO.File]::WriteAllText("$Directory/stdout.log", $stdout.GetAwaiter().GetResult(), $utf8)
        [IO.File]::WriteAllText("$Directory/stderr.log", $stderr.GetAwaiter().GetResult(), $utf8)
        @{ ExitCode = $process.ExitCode; TimedOut = $timedOut; TimeoutSeconds = 120 } |
            ConvertTo-Json | Set-Content -LiteralPath "$Directory/execution.json" -Encoding utf8NoBOM
        Require (-not $timedOut) 'Headless templater exceeded 120 seconds'
        Require ($process.ExitCode -eq 0) "Headless templater exited $($process.ExitCode)"
    }
    finally { $process.Dispose() }
}

try {
    foreach ($case in $cases) {
        $caseExpectation = $caseExpectations[$case]
        $originalEventCount = $caseExpectation.OriginalEventCount
        $templateCount = $caseExpectation.TemplateCount
        $directory = Join-Path $artifactRoot $case
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        $fixture = "tests/gui-automation/lua-workspace/fixtures/$case.ass"
        $expectedPath = "tests/gui-automation/lua-workspace/fixtures/$case.expected.json"
        Copy-Item -LiteralPath $fixture -Destination "$directory/input.ass"
        Copy-Item -LiteralPath 'automation/autoload/kara-templater.lua' -Destination "$directory/kara-templater.lua"
        Copy-Item -LiteralPath 'automation/include' -Destination "$directory/include" -Recurse
        Copy-Item -LiteralPath 'tests/gui-automation/lua-workspace/fixtures/runtime-actions.lua' -Destination "$directory/runtime-actions.lua"
        $expected = Get-Content -LiteralPath $expectedPath -Raw | ConvertFrom-Json
        $before = @(Events "$directory/input.ass")
        Require ($before.Count -eq $originalEventCount -and $before.Count -eq $templateCount + 2) 'Runtime fixture has an unexpected template or timed-line count'
        Require ($expected.generated.Count -eq $expected.generated_count) 'Independent expectation has an inconsistent generated count'
        RunScenario $directory
        $run = Get-Content -LiteralPath "$directory/run/result.json" -Raw | ConvertFrom-Json
        Require ($run.passed -and -not $run.timed_out) 'Automation scenario did not pass'
        $after = @(Events "$directory/output.ass")
        Require ($after.Count -eq $originalEventCount + $expected.generated_count) 'Unexpected generated ASS event count'
        for ($i = 0; $i -lt $originalEventCount; ++$i) {
            $original = $before[$i] | Select-Object *
            if ($i -ge $templateCount) { $original.Kind = 'Comment'; $original.Effect = 'karaoke' }
            Require (($original | ConvertTo-Json -Compress) -ceq ($after[$i] | ConvertTo-Json -Compress)) "Original event $i changed unexpectedly"
        }
        $records = @([IO.File]::ReadAllLines("$directory/run/automation-runtime.ndjson") | ForEach-Object { $_ | ConvertFrom-Json })
        $generated = @($records | Where-Object { $_.invocation.kind -eq 'macro_run' -and $_.template_debug -and $_.template_debug.kind -eq 'generated-line' })
        Require ($generated.Count -eq $expected.generated_count) 'A generated line lacks its own trace publication'
        for ($i = 0; $i -lt $expected.generated_count; ++$i) {
            $want = $expected.generated[$i]
            $actual = $after[$i + $originalEventCount]
            $wantStyle = if ($want.PSObject.Properties['style']) { $want.style } else { 'Default' }
            Require ($actual.Kind -ceq 'Dialogue' -and $actual.Effect -ceq 'fx' -and $actual.Style -ceq $wantStyle -and
                $actual.Actor -ceq '' -and $actual.MarginL -eq 0 -and $actual.MarginR -eq 0 -and $actual.MarginV -eq 0 -and
                $actual.Text -ceq $want.text -and $actual.Layer -eq $want.layer -and
                $actual.StartMs -eq $want.start_ms -and $actual.EndMs -eq $want.end_ms) "Generated ASS event $i differs from its independent expectation"
            $trace = $generated[$i].template_debug.generated
            $last = $trace.last_line
            Require ($trace.count -eq $i + 1 -and $last.generated_index -eq $i + 1 -and
                $last.text -ceq $want.text -and $last.layer -eq $want.layer -and $last.style -ceq $wantStyle -and $last.effect -ceq 'fx' -and
                $last.start_time -eq $want.start_ms -and $last.end_time -eq $want.end_ms) "Trace publication $i is stale or does not match the generated ASS event"
            if ($want.PSObject.Properties['scope']) {
                Require ($last.scope_kind -ceq $want.scope -and $generated[$i].template_debug.scope_kind -ceq $want.scope) "Trace publication $i has an unexpected template scope"
            }
        }
        $lastContext = $generated[-1].template_debug
        Require ($lastContext.phase -ceq 'syl-text' -and $lastContext.scope_kind -ceq $caseExpectation.LastScope -and
            $lastContext.loop_index -eq 2 -and $lastContext.loop_count -eq 2 -and
            $lastContext.syllable_index -eq 2 -and $lastContext.syllable_text -ceq $caseExpectation.LastSyllableText) 'Last Context does not identify the final syllable/loop'
        $lastContext | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$directory/last-generated-context.json" -Encoding utf8NoBOM
        $results.Add([ordered]@{
            Name = $case; Status = 'passed'; GeneratedCount = $generated.Count
            FixtureSha256 = (Get-FileHash -LiteralPath $fixture).Hash
            ExpectedSha256 = (Get-FileHash -LiteralPath $expectedPath).Hash
            TraceSha256 = (Get-FileHash -LiteralPath "$directory/run/automation-runtime.ndjson").Hash
            OutputSha256 = (Get-FileHash -LiteralPath "$directory/output.ass").Hash
        })
    }
}
catch {
    $failureMessage = $_.Exception.Message
    $done = @($results | ForEach-Object { $_.Name })
    $failed = $cases | Where-Object { $_ -notin $done } | Select-Object -First 1
    if ($failed) { $results.Add([ordered]@{ Name = $failed; Status = 'failed'; Error = $failureMessage }) }
    $done = @($results | ForEach-Object { $_.Name })
    foreach ($remaining in $cases | Where-Object { $_ -notin $done }) {
        $results.Add([ordered]@{ Name = $remaining; Status = 'not-run' })
    }
    throw
}
finally {
    [ordered]@{
        StartedUtc = $started; FinishedUtc = [DateTimeOffset]::UtcNow
        ExeSha256 = (Get-FileHash -LiteralPath $exePath).Hash
        TemplaterSha256 = (Get-FileHash -LiteralPath 'automation/autoload/kara-templater.lua').Hash
        ActionsSha256 = (Get-FileHash -LiteralPath 'tests/gui-automation/lua-workspace/fixtures/runtime-actions.lua').Hash
        IncludeHashes = @(Get-ChildItem -LiteralPath 'automation/include' -File -Recurse | Sort-Object FullName | ForEach-Object {
            @{ Path = [IO.Path]::GetRelativePath((Get-Location).Path, $_.FullName); Sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
        })
        VerifierSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash
        Cases = $results
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$artifactRoot/verification.json" -Encoding utf8NoBOM
}
