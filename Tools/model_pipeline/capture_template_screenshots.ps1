[CmdletBinding()]
param(
    [string]$EnginePath,
    [string]$RepoRoot,
    [string]$PreviewRoot,
    [ValidateSet('blank_3d', 'empty_project', 'fps_starter', 'mmo_starter', 'multiplayer_arena', 'platformer_kit', 'rpg_starter', 'third_person_starter', 'top_down_starter')]
    [string[]]$Name = @('blank_3d', 'empty_project', 'fps_starter', 'mmo_starter', 'multiplayer_arena', 'platformer_kit', 'rpg_starter', 'third_person_starter', 'top_down_starter'),
    [ValidateSet('hero', 'wide', 'detail')]
    [string[]]$Shot = @('hero', 'wide', 'detail'),
    [double]$CaptureAtSeconds = 2.0,
    [double]$ExitAfterSeconds = 8.0,
    [int]$WindowWidth = 1920,
    [int]$WindowHeight = 1080
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
$engineFile = Get-Item -LiteralPath $EnginePath
$engineSha256 = (Get-FileHash -LiteralPath $EnginePath -Algorithm SHA256).Hash.ToLowerInvariant()
$engineVersion = $engineFile.VersionInfo.ProductVersion
if ([string]::IsNullOrWhiteSpace($engineVersion)) {
    $engineVersion = $engineFile.VersionInfo.FileVersion
}

if (Get-Process SparkEngine -ErrorAction SilentlyContinue) {
    throw 'A SparkEngine process is already running; close it before deterministic capture.'
}

$cases = @(
    @{ Name='blank_3d'; Template='Blank3D'; Scene='Default.sparkscene'; Hero='6.8 4.4 -8.2|0 1.5 2.3'; Wide='11 7.5 -14|0 1.7 2.7'; Detail='4.2 3 -5.2|0 1.5 0.5' },
    @{ Name='empty_project'; Template='EmptyProject'; Scene='RuntimePreview.sparkscene'; Hero='6 3.5 -7|0 1 0'; Wide='8 6 -11|0 1.1 0'; Detail='3.2 2.3 -4|0 0.9 0' },
    @{ Name='fps_starter'; Template='FPSStarter'; Scene='Arena.sparkscene'; Hero='0 4 -10|0 1.3 8'; Wide='12 8 -13|0 1.5 8'; Detail='-6 2.6 -2|-5 1.1 8' },
    @{ Name='mmo_starter'; Template='MMOStarter'; Scene='Frontier.sparkscene'; Hero='0 4.8 -9|0 1.5 9'; Wide='10 7 -11|0 1.5 9'; Detail='-6 3 -4|-3 1.5 8' },
    @{ Name='multiplayer_arena'; Template='MultiplayerArena'; Scene='Arena.sparkscene'; Hero='0 5.5 -11|0 1 2'; Wide='11 8.5 -15|0 1 1'; Detail='-4.5 2.8 -5|-2.5 1 0' },
    @{ Name='platformer_kit'; Template='PlatformerKit'; Scene='Level01.sparkscene'; Hero='6 4 -10|6 1.8 0'; Wide='10 8 -17|9 2.5 0'; Detail='3 2.5 -7|5 1.5 0' },
    @{ Name='rpg_starter'; Template='RPGStarter'; Scene='Village.sparkscene'; Hero='0 3.2 -7.5|0 1 1.6'; Wide='11 7.5 -14|0 1.5 3.5'; Detail='-3.8 2.3 -4.3|-0.8 0.8 1' },
    @{ Name='third_person_starter'; Template='ThirdPersonStarter'; Scene='Adventure.sparkscene'; Hero='0 3.5 -6|0 1 5'; Wide='8 5.8 -7.5|0 1.3 6'; Detail='-2.8 2.2 -0.5|0 1 7.5' },
    @{ Name='top_down_starter'; Template='TopDownStarter'; Scene='Skirmish.sparkscene'; Hero='0 13 -10|0 0 1.5'; Wide='10 14 -12|0 0 1.5'; Detail='6.5 9 -7|1.5 0 1.5' }
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
$sourceCommit = (& git -C $RepoRoot rev-parse HEAD).Trim()
$workingTreeDirty = [bool]((& git -C $RepoRoot status --porcelain --untracked-files=no) -join '')

foreach ($case in $cases) {
    $scene = (Resolve-Path -LiteralPath (Join-Path $RepoRoot "Templates\$($case.Template)\Scenes\$($case.Scene)")).Path
    $assetManifest = (Resolve-Path -LiteralPath (Join-Path $RepoRoot "Templates\$($case.Template)\Assets\manifest.json")).Path
    $sceneSha256 = (Get-FileHash -LiteralPath $scene -Algorithm SHA256).Hash.ToLowerInvariant()
    $assetManifestSha256 = (Get-FileHash -LiteralPath $assetManifest -Algorithm SHA256).Hash.ToLowerInvariant()
    $runDirectory = [IO.Path]::GetFullPath((Join-Path $tempRoot $case.Name))
    if (-not $runDirectory.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing unsafe temporary directory: $runDirectory"
    }
    New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
    $execPath = Join-Path $runDirectory 'capture.exec'
    $commands = [Collections.Generic.List[string]]::new()
    $outputs = @{}
    $captureIndex = 0
    foreach ($shotName in $Shot) {
        $camera = $case[$shotName.Substring(0, 1).ToUpperInvariant() + $shotName.Substring(1)] -split '\|'
        if ($camera.Count -ne 2) { throw "Invalid $shotName camera preset for $($case.Name)" }
        $cameraAt = 0.8 + ($captureIndex * 2.0)
        $captureAt = $CaptureAtSeconds + ($captureIndex * 2.0)
        $output = Join-Path $previewRoot "$($case.Name)_${shotName}_engine.png"
        $outputs[$shotName] = @{
            Path = $output
            Before = if (Test-Path -LiteralPath $output) { (Get-Item -LiteralPath $output).LastWriteTimeUtc } else { [datetime]::MinValue }
            Camera = $camera
            CaptureAt = $captureAt
        }
        $escapedOutput = $output.Replace('\', '/').Replace('"', '\"')
        $commands.Add("t$cameraAt cam_fov 65")
        $commands.Add("t$cameraAt cam_pos $($camera[0])")
        $commands.Add("t$cameraAt cam_lookat $($camera[1])")
        $commands.Add("t$captureAt gfx_screenshot `"$escapedOutput`"")
        ++$captureIndex
    }
    [IO.File]::WriteAllText($execPath, (($commands -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))

    $arguments = @(
        '-no-subprocess',
        '-scene', ('"' + $scene + '"'),
        '-exec', ('"' + $execPath + '"'),
        '-test-seconds', $ExitAfterSeconds.ToString([Globalization.CultureInfo]::InvariantCulture),
        '-window-size', "${WindowWidth}x${WindowHeight}"
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
    foreach ($shotName in $Shot) {
        $capture = $outputs[$shotName]
        $output = $capture.Path
        if (-not (Test-Path -LiteralPath $output) -or (Get-Item -LiteralPath $output).LastWriteTimeUtc -le $capture.Before) {
            $audit = Join-Path $runDirectory 'exec_audit.log'
            $tail = if (Test-Path -LiteralPath $audit) { (Get-Content -LiteralPath $audit -Tail 40) -join "`n" } else { '(no audit log)' }
            throw "$($case.Name)/$shotName did not produce a fresh screenshot.`n$tail"
        }
        $png = [IO.File]::ReadAllBytes($output)
        $signature = [byte[]](137, 80, 78, 71, 13, 10, 26, 10)
        if ($png.Length -lt 33 -or (Compare-Object $signature $png[0..7])) {
            throw "$($case.Name)/$shotName produced an invalid PNG: $output"
        }
        $width = ([int]$png[16] -shl 24) -bor ([int]$png[17] -shl 16) -bor ([int]$png[18] -shl 8) -bor [int]$png[19]
        $height = ([int]$png[20] -shl 24) -bor ([int]$png[21] -shl 16) -bor ([int]$png[22] -shl 8) -bor [int]$png[23]
        if ($width -lt 1200 -or $height -lt 675 -or $png.Length -lt 4096) {
            throw "$($case.Name)/$shotName produced an undersized capture (${width}x${height}, $($png.Length) bytes): $output"
        }
        $capture.Sha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()

        $metadata = [ordered]@{
            schema = 2
            commit = $sourceCommit
            workingTreeDirty = $workingTreeDirty
            template = $case.Template
            scene = $case.Scene
            sceneSha256 = $sceneSha256
            assetManifestSha256 = $assetManifestSha256
            imageSha256 = $capture.Sha256
            shot = $shotName
            cameraPosition = $capture.Camera[0]
            cameraLookAt = $capture.Camera[1]
            captureAtSeconds = $capture.CaptureAt
            requestedWindow = "${WindowWidth}x${WindowHeight}"
            capturedImage = "${width}x${height}"
            engine = $EnginePath
            engineSha256 = $engineSha256
        }
        if (-not [string]::IsNullOrWhiteSpace($engineVersion)) {
            $metadata['engineVersion'] = $engineVersion
        }
        [IO.File]::WriteAllText("$output.json", (($metadata | ConvertTo-Json -Depth 3) + "`n"), [Text.UTF8Encoding]::new($false))
        Write-Output "captured $($case.Name)/$shotName -> $output"
    }
    $duplicateGroups = @($outputs.GetEnumerator() | Group-Object { $_.Value.Sha256 } | Where-Object Count -gt 1)
    if ($duplicateGroups.Count -gt 0) {
        $duplicateShots = ($duplicateGroups | ForEach-Object { ($_.Group.Name | Sort-Object) -join ', ' }) -join '; '
        throw "$($case.Name) produced duplicate screenshot content for: $duplicateShots"
    }
}
