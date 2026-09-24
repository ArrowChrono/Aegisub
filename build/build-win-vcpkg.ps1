param(
    [ValidateSet('build', 'test', 'all')]
    [string]$Mode = 'build',
    [string]$VcpkgRoot = '',
    [string]$VcpkgInstalledRoot = '',
    [string]$BuildDir = 'build-dir',
    [string]$Configuration = 'Release',
    [string]$Triplet = 'x64-windows-release',
    [string]$HostTriplet = '',
    [string]$AviSynthIncludeDir = './include/avisynth',
    [string]$FFMS2IncludeDir = './include/ffms2',
    [string]$LibPlaceboIncludeDir = './include/libplacebo',
    [string]$LibassIncludeDir = './include',
    [string]$LibassRuntimeLibrary = './runtimes/ass.dll',
    [string]$CMakeCxxFlags = '/DWIN32 /D_WINDOWS /GR /EHsc /DUNICODE /D_UNICODE /MP',
    [string]$CMakeCFlags = '/DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /MP',
    [string]$VcpkgInstallOptions = '',
    [switch]$Fresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Write-Step([string]$Message) {
    Write-Host ('==> ' + $Message) -ForegroundColor Cyan
}

function Get-VsGenerator {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return 'Visual Studio 17 2022' }

    $installPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $installPath) { return 'Visual Studio 17 2022' }

    if ($installPath -match '\\18\\') { return 'Visual Studio 18 2026' }
    if ($installPath -match '\\17\\') { return 'Visual Studio 17 2022' }
    if ($installPath -match '\\16\\') { return 'Visual Studio 16 2019' }
    return 'Visual Studio 17 2022'
}

function Get-CMake {
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmake) { return $cmake.Source }

    $vsCMake = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\16\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $vsCMake) { throw 'cmake.exe not found.' }
    return $vsCMake
}

function Get-PowerShellExecutable {
    foreach ($candidate in @('pwsh.exe', 'powershell.exe')) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    return 'powershell.exe'
}

function Resolve-RepoPath([string]$PathValue) {
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ''
    }

    if ([System.IO.Path]::IsPathRooted($PathValue)) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $PathValue))
}

function Invoke-Native([string]$FilePath, [string[]]$Arguments) {
    Write-Step (($FilePath, ($Arguments -join ' ')) -join ' ')
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

function Configure-CMake([string]$CMakePath, [string]$GeneratorName) {
    if ($Fresh -and (Test-Path $BuildDir)) {
        Write-Step "Removing $BuildDir"
        Remove-Item $BuildDir -Recurse -Force
    }

    $cmakeArgs = [System.Collections.Generic.List[string]]::new()
    $effectiveHostTriplet = if ($HostTriplet) { $HostTriplet } else { $Triplet }
    foreach ($arg in @(
        '-S', '.',
        '-B', $BuildDir,
        '-G', $GeneratorName,
        '-A', 'x64',
        "-DCMAKE_TOOLCHAIN_FILE=$resolvedVcpkgRoot/scripts/buildsystems/vcpkg.cmake",
        "-DVCPKG_TARGET_TRIPLET=$Triplet",
        "-DVCPKG_HOST_TRIPLET=$effectiveHostTriplet",
        "-DVCPKG_OVERLAY_TRIPLETS=$PSScriptRoot/../cmake",
        "-DVCPKG_MANIFEST_FEATURES=skia",
        "-DVCPKG_INSTALL_OPTIONS=$VcpkgInstallOptions",
        '-DLUA_WITH_LUASOCKET=ON',
        '-DAEGISUB_LUAJIT_SHARED=ON',
        '-DXAUDIO2_REDIST=ON',
        '-DWITH_AVISYNTH=ON',
        "-DAviSynth_INCLUDE_DIR=$resolvedAviSynthIncludeDir",
        "-Dass_INCLUDE_DIR=$resolvedLibassIncludeDir",
        "-Dass_RUNTIME_LIBRARY=$resolvedLibassRuntimeLibrary",
        '-DWITH_FFMS2=ON',
        "-DFFMS2_INCLUDE_DIR=$resolvedFFMS2IncludeDir",
        "-DWITH_LSMASNATIVE=ON",
        "-DWITH_SCENECHANGE=ON",
        '-DWITH_LIBPLACEBO=ON',
        "-DLibPlacebo_INCLUDE_DIR=$resolvedLibPlaceboIncludeDir",
        "-DWITH_FFTW3=ON",
        "-DWITH_PFFFT=ON",
        '-DAEGISUB_MATROSKA_PARSING=ON',
        "-DWITH_DRAWING_SKIA=ON",
        "-DWITH_SKIA_AUDIO_DISPLAY=ON",
        "-DWITH_PLUGIN_BRIDGE=ON",
        "-DZ_VCPKG_POWERSHELL_PATH:FILEPATH=$resolvedPowerShellExecutable",
        "-DCMAKE_CXX_FLAGS=$CMakeCxxFlags",
        "-DCMAKE_C_FLAGS=$CMakeCFlags",
        '-DWITH_TEST=ON'
    )) {
        $cmakeArgs.Add($arg)
    }

    if ($resolvedVcpkgInstalledRoot) {
        $cmakeArgs.Add("-DVCPKG_INSTALLED_DIR=$resolvedVcpkgInstalledRoot")
    }
    else {
        $cmakeArgs.Add('-U')
        $cmakeArgs.Add('VCPKG_INSTALLED_DIR')
    }

    $configureStart = Get-Date
    Invoke-Native $CMakePath $cmakeArgs
    $configureElapsed = (Get-Date) - $configureStart
    Write-Host ("CMake configure + vcpkg: {0:c}" -f $configureElapsed)
}

function Build-Aegisub([string]$CMakePath) {
    $buildStart = Get-Date
    Invoke-Native $CMakePath @(
        '--build', $BuildDir,
        '--config', $Configuration,
        '--target', 'Aegisub',
        '--parallel'
    )

    $exe = Join-Path $BuildDir "$Configuration\Aegisub.exe"
    $luaDll = Join-Path $BuildDir "$Configuration\lua51.dll"
    if (-not (Test-Path $luaDll)) {
        throw "Expected LuaJIT runtime not found: $luaDll"
    }

    $buildElapsed = (Get-Date) - $buildStart
    Write-Host ("Aegisub build: {0:c}" -f $buildElapsed)
    Write-Step "Done: $exe"
}

function Run-Tests([string]$CMakePath) {
    Invoke-Native $CMakePath @(
        '--build', $BuildDir,
        '--config', $Configuration,
        '--target', 'test-aegisub',
        '--parallel'
    )
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
Set-Location $repoRoot

$resolvedVcpkgRoot = if ($VcpkgRoot) {
    Resolve-RepoPath $VcpkgRoot
}
elseif ($env:VCPKG_ROOT) {
    [System.IO.Path]::GetFullPath($env:VCPKG_ROOT)
}
else {
    Join-Path $repoRoot 'vcpkg'
}

$resolvedVcpkgInstalledRoot = if ($VcpkgInstalledRoot) {
    Resolve-RepoPath $VcpkgInstalledRoot
}
elseif ($env:PROJECT_VCPKG_INSTALLED_ROOT) {
    [System.IO.Path]::GetFullPath($env:PROJECT_VCPKG_INSTALLED_ROOT)
}
else {
    ''
}

$resolvedAviSynthIncludeDir = Resolve-RepoPath $AviSynthIncludeDir
$resolvedFFMS2IncludeDir = Resolve-RepoPath $FFMS2IncludeDir
$resolvedLibPlaceboIncludeDir = Resolve-RepoPath $LibPlaceboIncludeDir
$resolvedLibassIncludeDir = Resolve-RepoPath $LibassIncludeDir
$resolvedLibassRuntimeLibrary = Resolve-RepoPath $LibassRuntimeLibrary

$cmake = Get-CMake
$generator = Get-VsGenerator
$resolvedPowerShellExecutable = Get-PowerShellExecutable

switch ($Mode) {
    'build' {
        Configure-CMake $cmake $generator
        Build-Aegisub $cmake
    }
    'test' {
        Run-Tests $cmake
    }
    'all' {
        Configure-CMake $cmake $generator
        Build-Aegisub $cmake
        Run-Tests $cmake
    }
}
