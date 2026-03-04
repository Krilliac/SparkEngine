# update-sparkbuild.ps1 -- Download the latest SparkBuild.exe from GitHub Releases
#
# Usage:  .\tools\update-sparkbuild.ps1            (fetches "latest" pre-release)
#         .\tools\update-sparkbuild.ps1 v1.0.0     (fetches a specific version)

param(
    [string]$Tag = "latest"
)

$ErrorActionPreference = "Stop"
$repo  = "Krilliac/SparkBuild"
$dest  = Join-Path $PSScriptRoot "SparkBuild.exe"

if ($Tag -eq "latest") {
    $url = "https://github.com/$repo/releases/download/latest/SparkBuild.exe"
} else {
    $url = "https://github.com/$repo/releases/download/$Tag/SparkBuild.exe"
}

Write-Host "[*] Downloading SparkBuild.exe ($Tag) ..."
Write-Host "    $url"

try {
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    Write-Host "[+] Updated: $dest"
} catch {
    Write-Error "[-] Download failed: $_"
    exit 1
}
