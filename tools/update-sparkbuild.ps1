# update-sparkbuild.ps1 -- Download SparkBuild binaries
#
# By default fetches from the latest GitHub Release.
# Pass -Artifacts to instead pull from the latest successful CI run.
#
# Usage:
#   .\tools\update-sparkbuild.ps1                  (latest release)
#   .\tools\update-sparkbuild.ps1 v1.0.0           (specific release tag)
#   .\tools\update-sparkbuild.ps1 -Artifacts       (latest CI build artifacts)
#   $env:GH_TOKEN="<token>"; .\tools\update-sparkbuild.ps1 -Artifacts

param(
    [string]$Tag = "latest",
    [switch]$Artifacts
)

$ErrorActionPreference = "Stop"
$Repo = "Krilliac/SparkBuild"
$ScriptDir = $PSScriptRoot

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Invoke-GHApi {
    param([string]$Url)
    $headers = @{ "Accept" = "application/vnd.github+json" }
    if ($env:GH_TOKEN) { $headers["Authorization"] = "Bearer $env:GH_TOKEN" }
    (Invoke-WebRequest -Uri $Url -Headers $headers -UseBasicParsing).Content | ConvertFrom-Json
}

function Save-File {
    param([string]$Url, [string]$Dest, [string]$Label = $Dest)
    Write-Host "    Downloading $Label ..."
    $headers = @{}
    if ($env:GH_TOKEN) { $headers["Authorization"] = "Bearer $env:GH_TOKEN" }
    Invoke-WebRequest -Uri $Url -OutFile $Dest -Headers $headers -UseBasicParsing
}

function Set-Symlink {
    param([string]$Target, [string]$Link)
    if (Test-Path $Link) { Remove-Item $Link -Force }
    New-Item -ItemType SymbolicLink -Path $Link -Target $Target | Out-Null
}

# ---------------------------------------------------------------------------
# Artifact download (latest successful CI run)
# ---------------------------------------------------------------------------

function Get-Artifacts {
    Write-Host "[*] Fetching latest CI build artifacts from $Repo ..."
    Write-Host ""

    $runs = Invoke-GHApi "https://api.github.com/repos/$Repo/actions/runs?status=success&per_page=1"
    $runId = $runs.workflow_runs[0].id
    if (-not $runId) {
        Write-Error "Could not find a successful CI run. Check GH_TOKEN or network."
        exit 1
    }
    Write-Host "[*] Latest successful run: $runId"
    Write-Host ""

    $artifactList = Invoke-GHApi "https://api.github.com/repos/$Repo/actions/runs/$runId/artifacts"

    # Artifact name -> local filename
    $artifactMap = [ordered]@{
        "SparkBuild-Windows"     = "SparkBuild.exe"
        "SparkBuild-Linux-gcc"   = "SparkBuild-linux-gcc"
        "SparkBuild-Linux-clang" = "SparkBuild-linux-clang"
        "SparkBuild-macOS"       = "SparkBuild-macos"
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
    New-Item -ItemType Directory -Path $tmp | Out-Null

    foreach ($artifactName in $artifactMap.Keys) {
        $localName = $artifactMap[$artifactName]
        $dest = Join-Path $ScriptDir $localName

        $artifact = $artifactList.artifacts | Where-Object { $_.name -eq $artifactName } | Select-Object -First 1
        Write-Host ("    {0,-30} -> {1}" -f $artifactName, $dest)

        if (-not $artifact) {
            Write-Host "    [~] Not found in this run (skipped)"
            continue
        }

        $zipUrl = "https://api.github.com/repos/$Repo/actions/artifacts/$($artifact.id)/zip"
        $zipFile = Join-Path $tmp "$artifactName.zip"
        $extractDir = Join-Path $tmp $artifactName

        try {
            Save-File -Url $zipUrl -Dest $zipFile -Label $artifactName
            Expand-Archive -Path $zipFile -DestinationPath $extractDir -Force
            $binary = Get-ChildItem -Path $extractDir -File -Recurse | Sort-Object FullName | Select-Object -First 1
            if ($binary) {
                Copy-Item $binary.FullName -Destination $dest -Force
                Write-Host "    [+] OK"
            }
            else {
                Write-Host "    [~] Zip was empty (skipped)"
            }
        }
        catch {
            Write-Host "    [~] Download failed (skipped)"
        }
    }

    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

    # Convenience copy/symlink: SparkBuild-linux -> gcc build (backward compat)
    $gccBin = Join-Path $ScriptDir "SparkBuild-linux-gcc"
    if (Test-Path $gccBin) {
        try {
            Set-Symlink -Target "SparkBuild-linux-gcc" -Link (Join-Path $ScriptDir "SparkBuild-linux")
            Write-Host ""
            Write-Host "[*] Symlink: SparkBuild-linux -> SparkBuild-linux-gcc"
        }
        catch {
            # Symlinks may require elevated permissions on Windows; copy instead
            Copy-Item $gccBin -Destination (Join-Path $ScriptDir "SparkBuild-linux") -Force
            Write-Host "[*] Copied SparkBuild-linux-gcc -> SparkBuild-linux (symlink unavailable)"
        }
    }
}

# ---------------------------------------------------------------------------
# Release download
# ---------------------------------------------------------------------------

function Get-Release {
    Write-Host "[*] Fetching SparkBuild release ($Tag) from $Repo ..."
    Write-Host ""

    # Asset name -> local filename
    $binaries = [ordered]@{
        "SparkBuild.exe"         = "SparkBuild.exe"
        "SparkBuild-linux-gcc"   = "SparkBuild-linux-gcc"
        "SparkBuild-linux-clang" = "SparkBuild-linux-clang"
        "SparkBuild-macos"       = "SparkBuild-macos"
    }

    foreach ($asset in $binaries.Keys) {
        $localName = $binaries[$asset]
        $dest = Join-Path $ScriptDir $localName
        $url = "https://github.com/$Repo/releases/download/$Tag/$asset"

        Write-Host ("    {0,-25} -> {1}" -f $asset, $dest)
        try {
            Save-File -Url $url -Dest $dest -Label $asset
            Write-Host "    [+] OK"
        }
        catch {
            Write-Host "    [~] Not available (skipped)"
        }
    }

    # Convenience copy/symlink: SparkBuild-linux -> gcc build (backward compat)
    $gccBin = Join-Path $ScriptDir "SparkBuild-linux-gcc"
    if (Test-Path $gccBin) {
        try {
            Set-Symlink -Target "SparkBuild-linux-gcc" -Link (Join-Path $ScriptDir "SparkBuild-linux")
            Write-Host ""
            Write-Host "[*] Symlink: SparkBuild-linux -> SparkBuild-linux-gcc"
        }
        catch {
            Copy-Item $gccBin -Destination (Join-Path $ScriptDir "SparkBuild-linux") -Force
            Write-Host "[*] Copied SparkBuild-linux-gcc -> SparkBuild-linux (symlink unavailable)"
        }
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if ($Artifacts) {
    Get-Artifacts
}
else {
    Get-Release
}

Write-Host ""
Write-Host "[*] Done."
