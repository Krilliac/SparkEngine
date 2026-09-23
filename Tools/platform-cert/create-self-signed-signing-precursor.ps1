<#
.SYNOPSIS
    Owner-run helper for creating the local precursor to stable Windows signing.

.DESCRIPTION
    This script is intentionally interactive and is never used by CI. It creates
    one RSA/SHA-256 Code Signing certificate in the current user's personal
    certificate store, prompts for a SecureString password, and exports one
    encrypted PFX below the user's local application-data directory.

    The PFX is private signing material. Do not commit it, copy it to a source
    tree, place it on removable media, or send it through chat. The detached
    release-signature bundle is a separate stable-release prerequisite.
#>
[CmdletBinding()]
param(
    [switch]$NonInteractive,
    [string]$Subject = 'CN=SparkEngine Stable Release Publisher',
    [int]$ValidityYears = 2,
    [string]$OutputDirectory = $(Join-Path $env:LOCALAPPDATA 'SparkEngine\ReleaseSigning')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-CanonicalExistingPath([string]$Path) {
    return (Get-Item -LiteralPath $Path -Force -ErrorAction Stop).FullName
}

function Test-PathInside([string]$Parent, [string]$Child) {
    $parentWithSeparator = $Parent.TrimEnd('\') + '\'
    return $Child.Equals($Parent, [StringComparison]::OrdinalIgnoreCase) -or
        $Child.StartsWith($parentWithSeparator, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseComponents([string]$Path) {
    $probe = [IO.DirectoryInfo]::new([IO.Path]::GetFullPath($Path))
    while ($null -ne $probe) {
        if (Test-Path -LiteralPath $probe.FullName) {
            $item = Get-Item -LiteralPath $probe.FullName -Force -ErrorAction Stop
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Path contains a reparse point: $($probe.FullName)"
            }
        }
        $parent = $probe.Parent
        if ($null -eq $parent -or $parent.FullName -eq $probe.FullName) {
            break
        }
        $probe = $parent
    }
}

if ($NonInteractive -or -not [Environment]::UserInteractive -or $null -eq $Host.UI -or $null -eq $Host.UI.RawUI) {
    throw 'This helper requires an interactive owner-run PowerShell session; CI and -NonInteractive execution are refused.'
}
if ($ValidityYears -lt 1 -or $ValidityYears -gt 5) {
    throw 'ValidityYears must be between 1 and 5.'
}
if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    throw 'LOCALAPPDATA is required so private signing material stays user-local.'
}
if ([string]::IsNullOrWhiteSpace($Subject) -or $Subject -notmatch '^CN=[^,\r\n]+$') {
    throw 'Subject must be a single non-empty CN value.'
}

$scriptPath = Get-CanonicalExistingPath $PSCommandPath
$scriptRoot = Get-CanonicalExistingPath $PSScriptRoot
$repoRoot = Get-CanonicalExistingPath (Join-Path $scriptRoot '..\..')
$privateRoot = Get-CanonicalExistingPath $env:LOCALAPPDATA
$outputParent = [IO.Path]::GetFullPath($OutputDirectory)
Assert-NoReparseComponents $outputParent
if (-not (Test-PathInside $privateRoot $outputParent)) {
    throw 'PFX output must remain under the current user LOCALAPPDATA private directory.'
}
$outputDrive = Get-PSDrive -Name ([IO.Path]::GetPathRoot($outputParent).TrimEnd('\').TrimEnd(':')) -ErrorAction Stop
if ($outputDrive.DisplayRoot -and $outputDrive.Used -ne $null -and $outputDrive.Description -match '(?i)removable|usb') {
    throw 'The PFX output volume appears removable; choose a user-local fixed volume.'
}
if (Test-Path -LiteralPath $outputParent -PathType Leaf) {
    throw 'OutputDirectory must be a directory, not a file.'
}
if (Test-Path -LiteralPath $outputParent) {
    $existingOutput = Get-Item -LiteralPath $outputParent -Force
    if ($existingOutput.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'OutputDirectory must not be a reparse point or symlink.'
    }
    $outputParent = $existingOutput.FullName
} else {
    New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
    $outputParent = Get-CanonicalExistingPath $outputParent
}
if (Test-PathInside $repoRoot $outputParent) {
    throw 'PFX output must not be inside the SparkEngine repository.'
}
$pfxPath = Join-Path $outputParent 'SparkEngine-Stable-CodeSigning.pfx'
if (Test-Path -LiteralPath $pfxPath) {
    throw "Refusing to overwrite existing PFX: $pfxPath"
}

$existing = @(Get-ChildItem -Path 'Cert:\CurrentUser\My' -ErrorAction Stop |
    Where-Object { $_.Subject -eq $Subject })
if ($existing.Count -ne 0) {
    throw 'A matching subject already exists in CurrentUser personal store; refusing to create or replace a signer.'
}

$certificate = $null
$password = $null
try {
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Subject `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -NotAfter (Get-Date).AddYears($ValidityYears)
    if ($null -eq $certificate -or [string]::IsNullOrWhiteSpace($certificate.Thumbprint) -or -not $certificate.HasPrivateKey) {
        throw 'Certificate creation did not return an exportable private-key certificate.'
    }
    $password = Read-Host -AsSecureString -Prompt 'Enter the password for the exported signing PFX (input is hidden)'
    Export-PfxCertificate -Cert $certificate -FilePath $pfxPath -Password $password -CryptoAlgorithmOption AES256_SHA256 | Out-Null
    if (-not (Test-Path -LiteralPath $pfxPath -PathType Leaf)) {
        throw 'PFX export did not produce the expected file.'
    }
    $pfxItem = Get-Item -LiteralPath $pfxPath -Force
    if ($pfxItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Exported PFX is a reparse point; refusing to report it.'
    }
} catch {
    if (Test-Path -LiteralPath $pfxPath) {
        Remove-Item -LiteralPath $pfxPath -Force -ErrorAction SilentlyContinue
    }
    throw
} finally {
    $password = $null
}

Write-Output ('Public certificate thumbprint: ' + $certificate.Thumbprint.ToUpperInvariant())
Write-Output ('Encrypted PFX path: ' + $pfxPath)
Write-Output 'Stable-release names to provision manually (values are never printed):'
Write-Output '  Environment variable: SPARK_RELEASE_SIGNER_THUMBPRINT'
Write-Output '  Environment variable: SPARK_RELEASE_TRUST_MODEL=self-signed'
Write-Output '  Environment secret:   SPARK_RELEASE_SIGNING_PFX_BASE64'
Write-Output '  Environment secret:   SPARK_RELEASE_SIGNING_PFX_PASSWORD'
Write-Output 'Windows users may see an Unknown Publisher warning because this certificate is self-signed.'
Write-Output 'The detached release-signature bundle and its public-key variables are separate prerequisites.'
