# Shared cycle build for game repositories. Does not launch the game.
#
# Mirrors external/nesrecomp/tools/build_cycle_windows.ps1 (the engine's own
# Windows recipe) but for THIS repository's root CMakeLists.txt, which adds
# MODS / GAME_ID / HOST_EXTRAS and therefore cannot use the engine's
# runner/cyc/project entry point.
#
# The release artifact carries NO ROM: the launcher asks the player for their
# own copy at first launch. The ROM is needed HERE, because the build
# recompiles the cartridge's code into the executable.
#
#   tools\release\build.ps1 -Rom "roms\Super Dodge Ball (USA).nes"
#
# Native tools run through a hidden process wrapper (no console window, no
# focus theft), the same way the engine's Windows recipe does it.
param(
    [string]$Rom = '',
    [string]$BuildDir = 'build-release',
    [string]$Version = '',
    [string]$Revision = '',
    [int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
if (-not $Rom) { $Rom = Join-Path $Root 'roms/Super Dodge Ball (USA).nes' }
if (-not (Test-Path -LiteralPath $Rom)) {
    throw "No ROM at $Rom.`nThe release build recompiles the cartridge's code, so it needs the ROM at build time.`nThe published archive does NOT contain it -- the launcher asks the player for their own copy at first launch.`nDrop a legally obtained copy at roms\Super Dodge Ball (USA).nes, or pass -Rom PATH."
}
$Rom = (Resolve-Path -LiteralPath $Rom).Path
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }

$cmake = Join-Path $env:ProgramFiles 'CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source }
if (-not $cmake -or -not (Test-Path -LiteralPath $cmake)) { throw "CMake was not found (looked in $env:ProgramFiles\CMake\bin and on PATH)" }

# The shipped executable's name; one file holds it, so the build, the
# packager and the Flatpak manifest cannot drift apart.
$ExeName = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'exe.name') -Raw).Trim()
if (-not $ExeName) { throw "tools/release/exe.name is empty" }

$GameConfig = Join-Path $Root 'src/game.toml'
$RecompUi = Join-Path $Root 'external/recomp-ui'
if (-not (Test-Path -LiteralPath (Join-Path $RecompUi 'recomp_ui.cmake'))) {
    throw "recomp-ui checkout missing at $RecompUi (git submodule update --init external/recomp-ui)"
}
# The engine vendors a Windows-x64 SDL2 2.30.0 and puts its cmake dir on
# CMAKE_PREFIX_PATH, which is exactly what find_package(SDL2 CONFIG) looks
# for. Name it explicitly so a runner that also has SDL2 installed cannot
# change what gets linked.
$Sdl2Dir = Join-Path $Root 'external/nesrecomp/runner/external/SDL2/cmake'
if (-not (Test-Path -LiteralPath (Join-Path $Sdl2Dir 'sdl2-config.cmake'))) {
    throw "vendored SDL2 missing at $Sdl2Dir"
}

# The launcher's hero art (the panel on the left of the game card). Without
# it the launcher draws a generic cartridge placeholder.
$BoxArt = Join-Path $Root 'docs/assets/boxart.tga'
if (-not (Test-Path -LiteralPath $BoxArt)) { throw "launcher box art missing at $BoxArt" }

if (-not $Version) {
    $versionFile = Join-Path $Root 'VERSION'
    if (Test-Path -LiteralPath $versionFile) { $Version = (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim() }
}
if (-not $Version) { $Version = 'development' }
if (-not $Revision) {
    try { $Revision = (& git -C $Root rev-parse --short HEAD 2>$null).Trim() } catch { }
    if (-not $Revision) { $Revision = 'unknown' }
}

function Invoke-CycleBuild([string[]]$ToolArgs) {
    $quoted = foreach ($value in $ToolArgs) {
        '"' + [regex]::Replace([regex]::Replace($value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $cmake
    $info.Arguments = $quoted -join ' '
    $info.WorkingDirectory = $Root
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        $null = $process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        Write-Output $stdout.GetAwaiter().GetResult()
        Write-Output $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode) { throw "CMake failed ($($process.ExitCode))" }
    } finally { $process.Dispose() }
}

Write-Output "=== Configuring release build ==="
Write-Output "    version  $Version"
Write-Output "    revision $Revision"
Write-Output "    rom      $Rom"
Write-Output "    build    $build"

$configure = @(
    '-S', $Root, '-B', $build,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DNESRECOMP_ROM=$Rom",
    "-DNESRECOMP_GAME_CONFIG=$GameConfig",
    '-DNESRECOMP_HEADLESS=OFF',
    "-DNESRECOMP_RECOMP_UI=$RecompUi",
    "-DNESRECOMP_GAME_VERSION=$Version",
    "-DNESRECOMP_BUILD_REVISION=$Revision",
    "-DNESRECOMP_EXE_NAME=$ExeName",
    "-DNESRECOMP_BOXART=$BoxArt",
    "-DSDL2_DIR=$Sdl2Dir",
    # Static CRT: the published ZIP then needs no Visual C++ redistributable
    # beside it, which is the single most common "it will not start" report
    # for a Windows release.
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
)
Invoke-CycleBuild $configure

Write-Output '=== Building ==='
Invoke-CycleBuild @('--build', $build, '--config', 'Release', '--parallel', $Parallel)

Write-Output ''
Write-Output "Release binary: $(Join-Path $build "Release/$ExeName.exe")"
Write-Output 'Package it with: tools\release\package.py --build-dir build-release --platform windows'
