param(
    [Parameter(Mandatory = $true)]
    [string]$BaselineImage,

    [Parameter(Mandatory = $true)]
    [string]$VariantImage
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$baseline = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $BaselineImage).ProviderPath)
$variant = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $VariantImage).ProviderPath)
try {
    if ($baseline.Width -ne $variant.Width -or $baseline.Height -ne $variant.Height) {
        throw 'FPS material A/B screenshots have different dimensions'
    }
    if ($baseline.Width -lt 320 -or $baseline.Height -lt 180) {
        throw 'FPS material A/B screenshots are too small to inspect the center building'
    }

    # The fixed package smoke camera sees Center_Building in this central region.
    # Compare a two-pixel grid so the test remains fast on hosted WARP runners.
    $left = [int]($baseline.Width * 0.35)
    $right = [int]($baseline.Width * 0.60)
    $top = [int]($baseline.Height * 0.27)
    $bottom = [int]($baseline.Height * 0.62)
    $centerChanged = 0
    $centerTotal = 0
    $outsideChanged = 0
    $outsideTotal = 0
    for ($y = 0; $y -lt $baseline.Height; $y += 2) {
        for ($x = 0; $x -lt $baseline.Width; $x += 2) {
            $a = $baseline.GetPixel($x, $y)
            $b = $variant.GetPixel($x, $y)
            $difference = [Math]::Abs([int]$a.R - [int]$b.R) +
                          [Math]::Abs([int]$a.G - [int]$b.G) +
                          [Math]::Abs([int]$a.B - [int]$b.B)
            $changed = $difference -ge 24
            if ($x -ge $left -and $x -lt $right -and $y -ge $top -and $y -lt $bottom) {
                $centerTotal++
                if ($changed) { $centerChanged++ }
            }
            else {
                $outsideTotal++
                if ($changed) { $outsideChanged++ }
            }
        }
    }

    $centerPercent = 100.0 * $centerChanged / $centerTotal
    $outsidePercent = 100.0 * $outsideChanged / $outsideTotal
    if ($centerPercent -lt 15.0) {
        throw "FPS center building ignored its material change ($([Math]::Round($centerPercent, 2))% central pixels changed)"
    }
    if ($outsidePercent -gt 2.0) {
        throw "FPS material A/B changed unrelated scene pixels ($([Math]::Round($outsidePercent, 2))% outside changed)"
    }

    Write-Output "FPS center material A/B passed: $([Math]::Round($centerPercent, 2))% central pixels changed, $([Math]::Round($outsidePercent, 2))% outside changed"
}
finally {
    $baseline.Dispose()
    $variant.Dispose()
}
