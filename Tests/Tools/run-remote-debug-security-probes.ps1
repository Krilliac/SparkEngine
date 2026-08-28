<#!
.SYNOPSIS
Compiles and executes the RemoteDebug hostile public-boundary probe with MSVC.

.DESCRIPTION
Run this from a Developer PowerShell whose environment already contains cl.exe.
The probe deliberately defines the historical test macro, exercises only public
loopback calls, injects raw anonymous traffic, and races StopListening against
a protected observer handler. It does not configure or build the full engine.
#>
[CmdletBinding()]
param(
    [string]$Root,
    [string]$OutputDirectory
)

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $env:TEMP 'SparkEngine-RemoteDebugSecurityProbe'
}

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
    throw 'cl.exe is not on PATH. Run from a VS 2022 x64 developer prompt or call vcvars64.bat first.'
}

$source = Join-Path $Root 'Tests\Tools\RemoteDebugSecurityBoundaryProbe.cpp'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Probe source missing: $source"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$binary = Join-Path $OutputDirectory 'RemoteDebugSecurityBoundaryProbe.exe'
$object = Join-Path $OutputDirectory 'RemoteDebugSecurityBoundaryProbe.obj'
$include = Join-Path $Root 'SparkEngine\Source'

& $compiler.Path /nologo /std:c++latest /permissive- /Zc:preprocessor /EHsc /W4 /WX "/I$include" "/Fo$object" $source "/Fe$binary"
if ($LASTEXITCODE -ne 0) {
    throw "MSVC failed to compile RemoteDebugSecurityBoundaryProbe.cpp (exit $LASTEXITCODE)."
}

& $binary
if ($LASTEXITCODE -ne 0) {
    throw "RemoteDebug security probe failed (exit $LASTEXITCODE)."
}
