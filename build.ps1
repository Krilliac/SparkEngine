<#
.SYNOPSIS
  Configure & build SparkEngine from PowerShell.
.PARAMETER Config
  Build configuration: Debug / Release (default = Debug)
.PARAMETER Gen
  CMake generator (default: "Visual Studio 17 2022")
.PARAMETER Editor
  Switch ON the editor module
.PARAMETER Console
  Switch ON the external console
.PARAMETER AngelScript
  Switch ON AngelScript integration
#>

param(
  [ValidateSet("Debug","Release")][string]$config = "Debug",
  [string]$gen  = "Visual Studio 17 2022",
  [switch]$editor,
  [switch]$console,
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

$cmakeOpts = @()
if ($editor)      { $cmakeOpts += "-DENABLE_EDITOR=ON" }      else { $cmakeOpts += "-DENABLE_EDITOR=OFF" }
if ($console)     { $cmakeOpts += "-DENABLE_CONSOLE=ON" }     else { $cmakeOpts += "-DENABLE_CONSOLE=OFF" }
if ($angelscript) { $cmakeOpts += "-DENABLE_ANGELSCRIPT=ON" } else { $cmakeOpts += "-DENABLE_ANGELSCRIPT=OFF" }

cmake .. -G "$gen" @cmakeOpts
cmake --build . --config $config --parallel
Pop-Location
