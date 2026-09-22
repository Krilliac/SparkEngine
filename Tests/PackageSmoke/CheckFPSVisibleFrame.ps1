param(
    [Parameter(Mandatory = $true)]
    [string]$ImagePath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resolved = (Resolve-Path -LiteralPath $ImagePath).ProviderPath
$bitmap = [System.Drawing.Bitmap]::FromFile($resolved)
try {
    if ($bitmap.Width -lt 320 -or $bitmap.Height -lt 180) {
        throw "FPS frame is too small for the visual smoke: $($bitmap.Width)x$($bitmap.Height)"
    }

    # Sample the central play area, avoiding HUD edges. A close-up of the
    # center building's untextured wall is one color across this whole grid;
    # a usable arena view includes geometry, floor, and sky.
    $colors = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($yFraction in @(0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85)) {
        foreach ($xFraction in @(0.15, 0.25, 0.35, 0.45, 0.55, 0.65, 0.75, 0.85)) {
            $x = [int](($bitmap.Width - 1) * $xFraction)
            $y = [int](($bitmap.Height - 1) * $yFraction)
            [void]$colors.Add($bitmap.GetPixel($x, $y).ToArgb())
        }
    }

    if ($colors.Count -lt 3) {
        throw "FPS play area is visually uniform ($($colors.Count) sampled colors); camera may be inside geometry"
    }

    Write-Output "FPS visible-frame smoke passed: $($colors.Count) sampled colors in $($bitmap.Width)x$($bitmap.Height)"
}
finally {
    $bitmap.Dispose()
}
