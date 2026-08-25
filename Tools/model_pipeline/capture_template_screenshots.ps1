[CmdletBinding()]
param(
    [string]$EnginePath,
    [string]$RepoRoot,
    [string]$PreviewRoot,
    [ValidateSet('fps_starter', 'mmo_starter', 'multiplayer_arena', 'platformer_kit', 'rpg_starter', 'third_person_starter', 'top_down_starter')]
    [string[]]$Name = @('fps_starter', 'mmo_starter', 'multiplayer_arena', 'platformer_kit', 'rpg_starter', 'third_person_starter', 'top_down_starter'),
    [double]$CaptureAtSeconds = 2.0,
    [double]$ExitAfterSeconds = 5.0
)

$ErrorActionPreference = 'Stop'
if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
} else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}
if (-not $EnginePath) {
    $EnginePath = Join-Path $RepoRoot 'build\bin\Debug\SparkEngine.exe'
}
$EnginePath = (Resolve-Path -LiteralPath $EnginePath).Path

if (Get-Process SparkEngine -ErrorAction SilentlyContinue) {
    throw 'A SparkEngine process is already running; close it before deterministic capture.'
}

$cases = @(
    @{ Name='fps_starter'; Template='FPSStarter'; Scene='Arena.sparkscene' },
    @{ Name='mmo_starter'; Template='MMOStarter'; Scene='Frontier.sparkscene' },
    @{ Name='multiplayer_arena'; Template='MultiplayerArena'; Scene='Arena.sparkscene' },
    @{ Name='platformer_kit'; Template='PlatformerKit'; Scene='Level01.sparkscene' },
    @{ Name='rpg_starter'; Template='RPGStarter'; Scene='Village.sparkscene' },
    @{ Name='third_person_starter'; Template='ThirdPersonStarter'; Scene='Adventure.sparkscene' },
    @{ Name='top_down_starter'; Template='TopDownStarter'; Scene='Skirmish.sparkscene' }
)
$cases = $cases | Where-Object { $_.Name -in $Name }

$tempRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'SparkEngineModelPipeline'))
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
if (-not $PreviewRoot) {
    $previewRoot = Join-Path $RepoRoot 'docs\images\model-pipeline'
} else {
    $previewRoot = [IO.Path]::GetFullPath($PreviewRoot)
}
New-Item -ItemType Directory -Path $previewRoot -Force | Out-Null

foreach ($case in $cases) {
    $scene = (Resolve-Path -LiteralPath (Join-Path $RepoRoot "Templates\$($case.Template)\Scenes\$($case.Scene)")).Path
    $output = Join-Path $previewRoot "$($case.Name)_engine.png"
    $runDirectory = [IO.Path]::GetFullPath((Join-Path $tempRoot $case.Name))
    if (-not $runDirectory.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing unsafe temporary directory: $runDirectory"
    }
    New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
    $execPath = Join-Path $runDirectory 'capture.exec'
    $commandOutput = $output.Replace('\', '/')
    $escapedOutput = $commandOutput.Replace('"', '\"')
    [IO.File]::WriteAllText($execPath, "t$CaptureAtSeconds gfx_screenshot `"$escapedOutput`"`n", [Text.UTF8Encoding]::new($false))
    $before = if (Test-Path -LiteralPath $output) { (Get-Item -LiteralPath $output).LastWriteTimeUtc } else { [datetime]::MinValue }

    $arguments = @(
        '-no-subprocess',
        '-scene', ('"' + $scene + '"'),
        '-exec', ('"' + $execPath + '"'),
        '-test-seconds', $ExitAfterSeconds.ToString([Globalization.CultureInfo]::InvariantCulture),
        '-window-size', '1280x720'
    )
    $process = Start-Process -FilePath $EnginePath -ArgumentList $arguments -WorkingDirectory $runDirectory -PassThru
    try {
        Wait-Process -Id $process.Id -Timeout ([math]::Ceiling($ExitAfterSeconds + 15))
        $process.Refresh()
    }
    finally {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $process.Id -Timeout 5 -ErrorAction SilentlyContinue
        }
    }
    if ($process.ExitCode -ne 0) {
        throw "$($case.Name) runtime exited with code $($process.ExitCode)"
    }
    if (-not (Test-Path -LiteralPath $output) -or (Get-Item -LiteralPath $output).LastWriteTimeUtc -le $before) {
        $audit = Join-Path $runDirectory 'exec_audit.log'
        $tail = if (Test-Path -LiteralPath $audit) { (Get-Content -LiteralPath $audit -Tail 30) -join "`n" } else { '(no audit log)' }
        throw "$($case.Name) did not produce a fresh screenshot.`n$tail"
    }
    $png = [IO.File]::ReadAllBytes($output)
    $signature = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
    if ($png.Length -lt 33 -or (Compare-Object $signature $png[0..7])) {
        throw "$($case.Name) produced an invalid PNG: $output"
    }
    $width = ([int]$png[16] -shl 24) -bor ([int]$png[17] -shl 16) -bor ([int]$png[18] -shl 8) -bor [int]$png[19]
    $height = ([int]$png[20] -shl 24) -bor ([int]$png[21] -shl 16) -bor ([int]$png[22] -shl 8) -bor [int]$png[23]
    if ($width -ne 1264 -or $height -ne 681 -or $png.Length -lt 4096) {
        throw "$($case.Name) produced an unexpected capture (${width}x${height}, $($png.Length) bytes): $output"
    }
    Write-Output "captured $($case.Name) -> $output"
}
