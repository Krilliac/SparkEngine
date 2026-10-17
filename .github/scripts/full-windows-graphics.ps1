$ErrorActionPreference = 'Stop'
$ErrorActionPreference = 'Stop'
$capability = 'Tools.Graphics.DirectX~~~~0.0.1.0'
if ((Get-WindowsCapability -Online -Name $capability).State -ne 'Installed') {
  Add-WindowsCapability -Online -Name $capability | Out-Null
}
$layer = Join-Path $env:SystemRoot 'System32\d3d11_3SDKLayers.dll'
if (-not (Test-Path -LiteralPath $layer)) {
  throw "D3D11 debug layer missing after installing ${capability}: $layer"
}
Write-Host "D3D11 debug layer present: $layer"

$layers = Join-Path $env:SystemRoot 'System32\d3d12SDKLayers.dll'
if (-not (Test-Path -LiteralPath $layers)) {
  Add-WindowsCapability -Online -Name 'Tools.Graphics.DirectX~~~~0.0.1.0' -ErrorAction Stop | Out-Host
}
if (-not (Test-Path -LiteralPath $layers)) {
  throw "Graphics Tools installed but $layers is missing; D3D12_Validation cannot run"
}
"d3d12SDKLayers.dll $((Get-Item -LiteralPath $layers).VersionInfo.FileVersion)"
