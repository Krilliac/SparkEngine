param(
    [Parameter(Mandatory = $true)]
    [string]$WorkRoot
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Join-Path ([IO.Path]::GetFullPath($WorkRoot)) ('checker-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
$image = Join-Path $root 'frame.png'
$log = Join-Path $root 'exec_audit.log'
$checker = Join-Path $PSScriptRoot 'CheckFPSVisibleFrame.ps1'
$sceneDirectory = Join-Path $root 'Assets/Scenes'
$scene = Join-Path $sceneDirectory 'level1.scene'
$authored = '    > FPS scene identity: authored scene "FPS Arena" (12 nodes) from ' + $scene
$fallback = '    > FPS scene identity: procedural fallback arena (authored scene failed to load from ' + $scene + ')'

# Three vertical bands satisfy the existing diversity and geometry thresholds.
# These fixtures test the checker only; the runtime probe supplies real frames.
$bitmap = [Drawing.Bitmap]::new(640, 360)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.Clear([Drawing.Color]::Black)
    $graphics.FillRectangle([Drawing.Brushes]::White, 220, 0, 210, 360)
    $graphics.FillRectangle([Drawing.Brushes]::Red, 430, 0, 210, 360)
    $bitmap.Save($image, [Drawing.Imaging.ImageFormat]::Png)
}
finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}

$cases = @(
    @{ Name = 'authored'; Text = $authored; Error = '' },
    @{ Name = 'missing'; Text = 'ordinary startup log'; Error = 'exactly one authored scene identity marker' },
    @{ Name = 'fallback'; Text = $fallback; Error = 'used the procedural fallback arena' },
    @{ Name = 'mixed'; Text = "$authored`n$fallback"; Error = 'used the procedural fallback arena' },
    @{ Name = 'duplicate'; Text = "$authored`n$authored"; Error = 'exactly one authored scene identity marker' },
    @{ Name = 'wrong name'; Text = $authored.Replace('FPS Arena', 'Other Arena'); Error = 'expected authored scene' },
    @{ Name = 'empty scene'; Text = $authored.Replace('(12 nodes)', '(0 nodes)'); Error = 'loaded with no nodes' },
    @{ Name = 'wrong root'; Text = $authored.Replace($sceneDirectory, (Join-Path $root 'foreign/Scenes'));
       Error = "expected '$sceneDirectory'" }
)
foreach ($case in $cases) {
    [IO.File]::WriteAllText($log, $case.Text, [Text.UTF8Encoding]::new($false))
    $failure = ''
    try {
        & $checker -ImagePath $image -LogPath $log -ExpectedSceneDirectory $sceneDirectory | Out-Null
    }
    catch {
        $failure = $_.Exception.Message
    }
    if ($case.Error -eq '') {
        if ($failure -ne '') { throw "$($case.Name) unexpectedly failed: $failure" }
    }
    elseif (-not $failure.Contains($case.Error)) {
        throw "$($case.Name) expected '$($case.Error)', got '$failure'"
    }
}

# UTF-8 paths must round-trip through the audit parser even on Windows PS 5.1.
$unicodeDirectory = Join-Path $root ('package-' + [char]0x03A9 + '/Assets/Scenes')
[IO.File]::WriteAllText($log, $authored.Replace($sceneDirectory, $unicodeDirectory), [Text.UTF8Encoding]::new($false))
& $checker -ImagePath $image -LogPath $log -ExpectedSceneDirectory $unicodeDirectory | Out-Null

# An authored marker must never bypass the original pixel assertions.
$bitmap = [Drawing.Bitmap]::new(640, 360)
try { $bitmap.Save($image, [Drawing.Imaging.ImageFormat]::Png) }
finally { $bitmap.Dispose() }
$failure = ''
try {
    & $checker -ImagePath $image -LogPath $log -ExpectedSceneDirectory $unicodeDirectory | Out-Null
}
catch { $failure = $_.Exception.Message }
if (-not $failure.Contains('visually uniform')) {
    throw "authored uniform frame expected pixel rejection, got '$failure'"
}
Write-Output 'FPS visible-frame checker: 10 behavioral cases passed'
