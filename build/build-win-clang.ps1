param(
    [ValidateSet('configure', 'build', 'test', 'all')]
    [string]$Mode = 'all',
    [string]$BuildDir = 'build-dir-clang',
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$VsInstallPath = '',
    [string]$WindowsSdkVersion = '10.0.26100.0',
    [string[]]$CMakeArguments = @(),
    [ValidateRange(1, 7200)]
    [int]$TestTimeoutSeconds = 900,
    [ValidateRange(1, 1024)]
    [int]$Jobs = [Environment]::ProcessorCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Invoke-Native([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if ((Get-Location).Path -ne $repoRoot) { throw 'Run this script from the repository root' }
if (-not $VsInstallPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsInstallPath) { throw 'Visual Studio C++ build tools not found' }
}
Import-Module (Join-Path $VsInstallPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
$devArgs = '-arch=x64 -host_arch=x64'
if ($WindowsSdkVersion) { $devArgs += " -winsdk=$WindowsSdkVersion" }
Enter-VsDevShell -VsInstallPath $VsInstallPath -SkipAutomaticLocation -DevCmdArguments $devArgs

$llvmBin = Join-Path $VsInstallPath 'VC/Tools/Llvm/x64/bin'
$env:AEGISUB_CLANG_CL = Join-Path $llvmBin 'clang-cl.exe'
if (-not (Test-Path -LiteralPath $env:AEGISUB_CLANG_CL)) { throw 'Install the Visual Studio C++ Clang tools component' }
$cmakeBin = Join-Path $VsInstallPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$ninjaBin = Join-Path $VsInstallPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:PATH = "$llvmBin;$cmakeBin;$ninjaBin;$env:PATH"
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$ninja = (Get-Command ninja.exe -ErrorAction Stop).Source
foreach ($tool in @('rc.exe', 'mt.exe', 'ml64.exe')) { Get-Command $tool -ErrorAction Stop | Out-Null }

$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $cacheFile) {
    $cache = Get-Content -LiteralPath $cacheFile
    if (-not ($cache -match '^CMAKE_GENERATOR:INTERNAL=Ninja$') -or
        -not ($cache -match '^CMAKE_CXX_COMPILER:[^=]+=.*clang-cl\.exe$')) {
        throw 'Existing cache is not a Ninja clang-cl build; use a separate BuildDir'
    }
    $cachedCompiler = ($cache -match '^CMAKE_CXX_COMPILER:[^=]+=')[0] -replace '^[^=]+=', ''
    if ([IO.Path]::GetFullPath($cachedCompiler) -ne [IO.Path]::GetFullPath($env:AEGISUB_CLANG_CL)) {
        throw 'Existing cache uses a different LLVM installation; use a separate BuildDir'
    }
}
if ($Mode -in @('configure', 'all')) {
    if (Test-Path -LiteralPath $cacheFile) {
        if (-not ($cache -match "^CMAKE_BUILD_TYPE:[^=]+=$Configuration$")) {
            throw 'Existing cache has a different configuration; use a separate BuildDir'
        }
    }
    if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $VsInstallPath 'VC/vcpkg' }
    $env:VCPKG_ROOT = (Resolve-Path -LiteralPath $VcpkgRoot).Path
    $toolchain = Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $toolchain)) { throw 'VcpkgRoot does not contain vcpkg' }
    & ./build/version.ps1 .
    $shell = (Get-Process -Id $PID).Path
    Invoke-Native $cmake (@(
        '-S', '.', '-B', $BuildDir, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_C_COMPILER=$env:AEGISUB_CLANG_CL",
        "-DCMAKE_CXX_COMPILER=$env:AEGISUB_CLANG_CL",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_OVERLAY_TRIPLETS=$repoRoot/cmake",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-clang-cl',
        '-DVCPKG_HOST_TRIPLET=x64-windows-clang-cl',
        '-DVCPKG_MANIFEST_FEATURES=skia', '-DVCPKG_APPLOCAL_DEPS=ON',
        "-DZ_VCPKG_PWSH_PATH=$shell", "-DZ_VCPKG_POWERSHELL_PATH=$shell",
        '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF',
        '-DCLANG_TIDY:FILEPATH=', '-DCMAKE_CXX_CLANG_TIDY=',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCMAKE_NINJA_CMCLDEPS_RC=0',
        '-DCMAKE_RC_COMPILE_OBJECT=<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /fo <OBJECT> <SOURCE>',
        '-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE',
        '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /DUNICODE /D_UNICODE',
        '-DXAUDIO2_REDIST=ON', '-DAEGISUB_LUAJIT_SHARED=ON', '-DLUA_WITH_LUASOCKET=ON',
        '-DWITH_AVISYNTH=ON', '-DAviSynth_INCLUDE_DIR=include/avisynth',
        '-DWITH_FFMS2=ON', '-DFFMS2_INCLUDE_DIR=include/ffms2',
        '-DWITH_LIBPLACEBO=ON', '-DLibPlacebo_INCLUDE_DIR=include',
        '-Dass_INCLUDE_DIR=include',
        '-DWITH_LSMASNATIVE=ON', '-DWITH_SCENECHANGE=ON',
        '-DWITH_FFTW3=ON', '-DWITH_PFFFT=ON', '-DAEGISUB_MATROSKA_PARSING=ON',
        '-DWITH_DRAWING_SKIA=ON', '-DWITH_SKIA_AUDIO_DISPLAY=ON', '-DWITH_SKIA_SUBTITLE_GRID=ON',
        '-DWITH_SKIA_VIDEO_TOOLS=OFF', '-DWITH_PLUGIN_BRIDGE=ON',
        '-DWITH_TEST=ON', '-DWITH_SMOKE=ON', '-DBUILD_TESTING=ON'
    ) + $CMakeArguments)
}
if ($Mode -in @('build', 'test', 'all')) {
    if (-not (Test-Path -LiteralPath $cacheFile)) { throw 'Configure the build directory first' }
    $targets = if ($Mode -eq 'build') { @('Aegisub') }
               elseif ($Mode -eq 'test') { @('gtest-run', 'font-collector-state-test') }
               else { @('Aegisub', 'gtest-run', 'font-collector-state-test') }
    Invoke-Native $cmake (@('--build', $BuildDir, '--parallel', "$Jobs", '--target') + $targets)
}
if ($Mode -in @('test', 'all')) {
    $testArgs = @('--test-dir', $BuildDir, '-L', 'gtest', '--no-tests=error',
        '--timeout', '60', '--stop-on-failure', '--output-on-failure',
        '--output-junit', 'clang-unit-tests.xml')
    $startInfo = [Diagnostics.ProcessStartInfo]::new($ctest)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    foreach ($arg in $testArgs) { $startInfo.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        if (-not $process.WaitForExit($TestTimeoutSeconds * 1000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "CTest exceeded the total timeout of $TestTimeoutSeconds seconds"
        }
        if ($process.ExitCode -ne 0) { throw "CTest failed with exit code $($process.ExitCode)" }
    }
    finally { $process.Dispose() }
}
