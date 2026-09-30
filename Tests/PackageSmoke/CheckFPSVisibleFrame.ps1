param(
    [Parameter(Mandatory = $true)]
    [string]$ImagePath,

    # Console log (exec_audit.log) of the run that produced the image. The
    # procedural fallback arena renders a frame that passes every pixel test
    # below, so the pixels alone cannot prove the authored scene loaded.
    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [string]$ExpectedSceneName = 'FPS Arena',

    # When set, the authored scene must have been loaded from this directory
    # (e.g. the package's own Assets/Scenes), not from a parent-directory
    # asset root the module's search path could otherwise fall back to.
    [string]$ExpectedSceneDirectory = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$logText = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $LogPath).ProviderPath)
if ($logText -match 'FPS scene identity: procedural fallback arena[^\r\n]*') {
    throw "FPS run used the procedural fallback arena instead of the authored scene: $($Matches[0])"
}
$identities = [regex]::Matches($logText,
    'FPS scene identity: authored scene "(?<name>[^"\r\n]*)" \((?<nodes>\d+) nodes\) from (?<path>[^\r\n]+)')
if ($identities.Count -ne 1) {
    throw "FPS run log must carry exactly one authored scene identity marker; found $($identities.Count) in $LogPath"
}
$identity = $identities[0]
$sceneName = $identity.Groups['name'].Value
$sceneNodes = [int]$identity.Groups['nodes'].Value
$scenePath = $identity.Groups['path'].Value.Trim()
if ($sceneName -cne $ExpectedSceneName) {
    throw "FPS run loaded scene '$sceneName', expected authored scene '$ExpectedSceneName'"
}
if ($sceneNodes -lt 1) {
    throw "FPS authored scene '$sceneName' loaded with no nodes"
}
if ($ExpectedSceneDirectory -ne '') {
    $expectedDirectory = [System.IO.Path]::GetFullPath($ExpectedSceneDirectory).TrimEnd('\', '/')
    $actualDirectory = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($scenePath)).TrimEnd('\', '/')
    if (-not [string]::Equals($expectedDirectory, $actualDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "FPS authored scene loaded from '$actualDirectory', expected '$expectedDirectory'"
    }
}

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

    # A vertical sky/floor gradient alone is not evidence of arena geometry.
    # The authored central structure crosses this middle scanline, so require
    # two substantial horizontal color boundaries away from HUD edges.
    $scanY = [int](($bitmap.Height - 1) * 0.45)
    $previous = $null
    $transitions = 0
    foreach ($xFraction in @(0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85)) {
        $x = [int](($bitmap.Width - 1) * $xFraction)
        $pixel = $bitmap.GetPixel($x, $scanY)
        if ($null -ne $previous) {
            $difference = [Math]::Abs([int]$pixel.R - [int]$previous.R) +
                          [Math]::Abs([int]$pixel.G - [int]$previous.G) +
                          [Math]::Abs([int]$pixel.B - [int]$previous.B)
            if ($difference -ge 48) { $transitions++ }
        }
        $previous = $pixel
    }
    if ($transitions -lt 2) {
        throw "FPS play area lacks central geometry boundaries ($transitions strong horizontal transitions)"
    }

    Write-Output "FPS visible-frame smoke passed: authored scene '$sceneName' ($sceneNodes nodes), $($colors.Count) sampled colors and $transitions central geometry transitions in $($bitmap.Width)x$($bitmap.Height)"
}
finally {
    $bitmap.Dispose()
}
