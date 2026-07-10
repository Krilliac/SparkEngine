[CmdletBinding()]
param(
    [switch]$ClientOnly
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root 'build\windows-release\bin'
$engine = Join-Path $bin 'SparkEngine.exe'
$game = Join-Path $bin 'SparkGameMMOFPS.dll'
$serverConfig = Join-Path $PSScriptRoot 'Terrafront\server.cfg'
$clientConfig = Join-Path $PSScriptRoot 'Terrafront\client.cfg'
$port = 27020

foreach ($required in @($engine, $game, $serverConfig, $clientConfig)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required TERRAFRONT file is missing: $required"
    }
}

$listener = Get-NetUDPEndpoint -LocalPort $port -ErrorAction SilentlyContinue
$server = $null
if (-not $listener -and -not $ClientOnly) {
    $serverArgs = @(
        '-headless', '-no-subprocess',
        '-game', $game,
        '-exec', $serverConfig,
        '-threads', '2'
    )
    $server = Start-Process -FilePath $engine -ArgumentList $serverArgs `
        -WorkingDirectory $root -WindowStyle Hidden -PassThru

    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    do {
        Start-Sleep -Milliseconds 250
        $listener = Get-NetUDPEndpoint -LocalPort $port -ErrorAction SilentlyContinue
    } while (-not $listener -and -not $server.HasExited -and [DateTime]::UtcNow -lt $deadline)

    if ($server.HasExited) {
        throw "TERRAFRONT dedicated server exited with code $($server.ExitCode)."
    }
    if (-not $listener) {
        Stop-Process -Id $server.Id -ErrorAction SilentlyContinue
        throw "TERRAFRONT dedicated server did not bind UDP port $port within 8 seconds."
    }
}

$clientArgs = @(
    '-game', $game,
    '-exec', $clientConfig,
    '-threads', '2'
)
$client = Start-Process -FilePath $engine -ArgumentList $clientArgs `
    -WorkingDirectory $root -PassThru

[pscustomobject]@{
    ServerPid = if ($server) { $server.Id } else { $null }
    ClientPid = $client.Id
    Endpoint = "127.0.0.1:$port"
}
