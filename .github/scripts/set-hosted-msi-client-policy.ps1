<#
.SYNOPSIS
Give a GitHub-hosted Windows Server runner the Windows 10/11 default for per-user MSI installs.

.DESCRIPTION
The Shipping MSI is per-user and never requests elevation (cmake/SparkCPackOptions.cmake).
Windows 10/11 clients allow non-elevated per-user installs. Windows Server images default
the DisableMsi machine policy to 1, which blocks them: msiexec exits 1625 ("Non-assigned
apps are disabled for non-admin users") before any drill can run.

On a GitHub-hosted Windows Server runner only, this script sets
HKLM\Software\Policies\Microsoft\Windows\Installer\DisableMsi to 0, the client default.
The runner is discarded after the job, so the value is not restored. Self-hosted runners
and client editions are never changed: their policy is the owner's, and a blocked install
there is a real finding.

The original value, the value in effect afterwards and the reason for the decision are
written as JSON to -Evidence, which must not exist yet. Hosted Windows Server evidence is
still not Windows 11 certification.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Evidence
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath $Evidence) { throw "MSI install-policy evidence '$Evidence' must not exist yet" }

$key = 'HKLM:\Software\Policies\Microsoft\Windows\Installer'
$name = 'DisableMsi'

function Get-DisableMsi {
    $item = Get-ItemProperty -LiteralPath $key -Name $name -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $null }
    return [int]$item.$name
}

$os = Get-CimInstance -ClassName Win32_OperatingSystem
# ProductType: 1 = workstation (client), 2 = domain controller, 3 = server.
$isServer = [int]$os.ProductType -ne 1
$runnerEnvironment = [string]$env:RUNNER_ENVIRONMENT
$isHosted = $runnerEnvironment -eq 'github-hosted'
$original = Get-DisableMsi

if (-not $isHosted) {
    $action = 'unchanged'
    $reason = 'not a GitHub-hosted runner; its installer policy belongs to the runner owner'
} elseif (-not $isServer) {
    $action = 'unchanged'
    $reason = 'client edition; the per-user install policy is already the product target'
} elseif ($original -eq 0) {
    $action = 'unchanged'
    $reason = 'DisableMsi is already 0'
} else {
    if (-not (Test-Path -LiteralPath $key)) { New-Item -Path $key -Force | Out-Null }
    New-ItemProperty -LiteralPath $key -Name $name -PropertyType DWord -Value 0 -Force | Out-Null
    $action = 'set-to-client-default'
    $reason = 'GitHub-hosted Windows Server runner; Server defaults block non-elevated per-user MSI installs'
}

$effective = Get-DisableMsi
if ($action -eq 'set-to-client-default' -and $effective -ne 0) {
    throw "DisableMsi reads back as '$effective' after it was set to 0"
}

$parent = Split-Path -Parent $Evidence
if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
[ordered]@{
    scope              = 'hosted-windows-msi-install-policy'
    policy             = "HKLM\Software\Policies\Microsoft\Windows\Installer\$name"
    original           = $original
    effective          = $effective
    action             = $action
    reason             = $reason
    runnerEnvironment  = $runnerEnvironment
    osCaption          = [string]$os.Caption
    osBuild            = [string]$os.BuildNumber
    osProductType      = [int]$os.ProductType
    certifiesWindows11 = $false
} | ConvertTo-Json | Set-Content -LiteralPath $Evidence -Encoding utf8

"DisableMsi: original=$(if ($null -eq $original) { 'absent' } else { $original }) effective=$(if ($null -eq $effective) { 'absent' } else { $effective }) ($action)"
