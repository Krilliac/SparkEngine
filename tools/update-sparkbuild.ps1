# update-sparkbuild.ps1 -- Download SparkBuild binaries from GitHub Releases
#
# Downloads Windows, Linux, and macOS binaries.
#
# Usage:  .\tools\update-sparkbuild.ps1            (fetches "latest" release)
#         .\tools\update-sparkbuild.ps1 v1.0.0     (fetches a specific version)

param(
    [string]$Tag = "latest"
)

$ErrorActionPreference = "Stop"
$repo = "Krilliac/SparkBuild"

# Map of asset name -> local filename
$binaries = @(
    @{ Asset = "SparkBuild.exe";   Local = "SparkBuild.exe"   }
    @{ Asset = "SparkBuild-linux"; Local = "SparkBuild-linux" }
    @{ Asset = "SparkBuild-macos"; Local = "SparkBuild-macos" }
)

Write-Host "[*] Fetching SparkBuild binaries ($Tag) from $repo ..."
Write-Host ""

foreach ($bin in $binaries) {
    $asset = $bin.Asset
    $dest  = Join-Path $PSScriptRoot $bin.Local
    $url   = "https://github.com/$repo/releases/download/$Tag/$asset"

    Write-Host ("    {0,-20} -> {1}" -f $asset, $dest)

    try {
        Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
        Write-Host "    [+] OK"
    } catch {
        Write-Host "    [~] Not available (skipped)"
    }
}

Write-Host ""
Write-Host "[*] Done."
