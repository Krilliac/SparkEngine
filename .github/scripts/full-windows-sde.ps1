$ErrorActionPreference = 'Stop'
$url = 'https://downloadmirror.intel.com/924984/sde-external-10.13.1-2026-07-28-win.tar.xz'
$expectedSha256 = '74e626ede09b0baa5011fc9e51b58627ea92c3fc0bae5fd7db34b490f335f651'
$dest = Join-Path $env:RUNNER_TEMP 'intel-sde-10.13.1'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$archive = Join-Path $dest 'sde-win.tar.xz'
Write-Host "Intel SDE: downloading pinned archive UTC=$([DateTime]::UtcNow.ToString('o'))"
Invoke-WebRequest -Uri $url -OutFile $archive -TimeoutSec 240
Write-Host "Intel SDE: download completed; verifying digest UTC=$([DateTime]::UtcNow.ToString('o')) archiveBytes=$((Get-Item -LiteralPath $archive).Length)"
$actualSha256 = (Get-FileHash -Algorithm SHA256 -Path $archive).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) { throw "Intel SDE archive hash mismatch: $actualSha256" }
Write-Host "Intel SDE: digest verified; extracting archive UTC=$([DateTime]::UtcNow.ToString('o'))"
$unpacked = Join-Path $dest 'unpacked'
python (Join-Path $PSScriptRoot 'extract_sde_archive.py') --archive $archive --destination $unpacked --metadata (Join-Path $env:QUAL_ROOT 'sde-extraction.json')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Intel SDE: extraction completed; locating executable UTC=$([DateTime]::UtcNow.ToString('o'))"
$executables = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter 'sde.exe')
if ($executables.Count -ne 1) { throw "Expected one sde.exe, found $($executables.Count)" }
Add-Content -Path $env:GITHUB_ENV -Value "SPARK_SDE_EXECUTABLE=$($executables[0].FullName.Replace('\', '/'))" -Encoding utf8
