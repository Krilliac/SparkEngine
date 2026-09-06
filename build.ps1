<#
.SYNOPSIS
  Configure & build SparkEngine from PowerShell.
.PARAMETER Config
  Build configuration: Debug / Release (default = Debug)
.PARAMETER Gen
  CMake generator (default: "Visual Studio 17 2022")
.PARAMETER Editor
  Force ENABLE_EDITOR=ON (the project already defaults it ON)
.PARAMETER AngelScript
  Force ENABLE_ANGELSCRIPT=ON (the project already defaults it ON)
.NOTES
  There is no ENABLE_CONSOLE option in the build system; the console is
  controlled by ENABLE_CONSOLE_IN_SHIPPING, which only affects Shipping
  (MinSizeRel) builds. Use the windows-shipping preset for that.
#>

param(
  [ValidateSet("Debug","Release")][string]$config = "Debug",
  [string]$gen  = "Visual Studio 17 2022",
  [switch]$editor,
  [switch]$angelscript
)

$ErrorActionPreference = "Stop"

# -----------------------------------------------------------------
# Check and initialize git submodules
# -----------------------------------------------------------------
Write-Host ""
Write-Host "=== Checking git submodules ===" -ForegroundColor Cyan

$submodules = @(
    @{ Path = "ThirdParty\Utils\miniz";                        Name = "miniz";          Desc = "Compression (crash dumps, save files)" },
    @{ Path = "ThirdParty\UI\imgui";                           Name = "Dear ImGui";     Desc = "Editor UI, debug overlays" },
    @{ Path = "ThirdParty\ECS\entt";                           Name = "EnTT";           Desc = "Entity component system" },
    @{ Path = "ThirdParty\Scripting\angelscript-mirror";       Name = "AngelScript";    Desc = "Hot-reload scripting" },
    @{ Path = "ThirdParty\Networking\curl";                    Name = "curl";           Desc = "HTTP networking (optional)" }
)

$missingSubmodules = 0
foreach ($sub in $submodules) {
    $fullPath = Join-Path $PSScriptRoot $sub.Path
    if ((Test-Path $fullPath) -and ((Get-ChildItem $fullPath -Force | Measure-Object).Count -eq 0)) {
        Write-Host "  [MISSING] $($sub.Name) ($($sub.Path)) - $($sub.Desc)" -ForegroundColor Yellow
        $missingSubmodules++
    } elseif (-not (Test-Path $fullPath)) {
        Write-Host "  [MISSING] $($sub.Name) ($($sub.Path)) - $($sub.Desc)" -ForegroundColor Yellow
        $missingSubmodules++
    } else {
        Write-Host "  [OK]      $($sub.Name)" -ForegroundColor Green
    }
}

if ($missingSubmodules -gt 0) {
    Write-Host ""
    Write-Host "WARNING: $missingSubmodules submodule(s) not initialized." -ForegroundColor Yellow
    Write-Host "  The engine will build but with DEGRADED functionality." -ForegroundColor Yellow
    Write-Host "  Initializing submodules now..." -ForegroundColor Yellow
    Write-Host ""
    git submodule update --init --recursive
    Write-Host ""
    Write-Host "Submodules initialized successfully." -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Building SparkEngine ($config) ===" -ForegroundColor Cyan
Write-Host ""

$buildDir = "build"
if (!(Test-Path $buildDir)) { New-Item $buildDir -ItemType Directory | Out-Null }
Push-Location $buildDir

# Switches are additive only. Passing -DENABLE_EDITOR=OFF whenever the switch
# was omitted silently inverted the project defaults (both options default ON),
# so an unadorned build.ps1 produced a different engine than a preset build.
$cmakeOpts = @()
if ($editor)      { $cmakeOpts += "-DENABLE_EDITOR=ON" }
if ($angelscript) { $cmakeOpts += "-DENABLE_ANGELSCRIPT=ON" }

cmake .. -G "$gen" @cmakeOpts

# With a Visual Studio generator, --parallel becomes MSBuild /m (one project
# node per core) while every cl.exe already runs /MP (one compiler per core),
# so the two multiply into N^2 compilers and the build goes memory-bound.
# /m:1 keeps one project at a time and lets /MP schedule inside it, which is
# what a build dominated by a single ~1000-file library wants anyway.
if ($gen -like "Visual Studio*") {
  cmake --build . --config $config --parallel 1
} else {
  cmake --build . --config $config --parallel
}
Pop-Location
