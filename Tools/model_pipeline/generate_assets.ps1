[CmdletBinding()]
param(
    [string]$BlenderPath,
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
} else {
    $RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
}

if (-not $BlenderPath) {
    if ($env:SPARK_BLENDER) {
        $BlenderPath = $env:SPARK_BLENDER
    } elseif (Test-Path -LiteralPath 'D:\Applications\Blender 5.2 Portable\blender.exe') {
        $BlenderPath = 'D:\Applications\Blender 5.2 Portable\blender.exe'
    } else {
        $command = Get-Command blender.exe -ErrorAction SilentlyContinue
        if ($command) {
            $BlenderPath = $command.Source
        }
    }
}

if (-not $BlenderPath -or -not (Test-Path -LiteralPath $BlenderPath -PathType Leaf)) {
    throw 'Blender was not found. Pass -BlenderPath or set SPARK_BLENDER.'
}

$generator = Join-Path $PSScriptRoot 'generate_starter_models.py'
& $BlenderPath --background --factory-startup --python-exit-code 1 --python $generator -- --repo-root $RepoRoot
if ($LASTEXITCODE -ne 0) {
    throw "Blender asset generation failed with exit code $LASTEXITCODE"
}

python (Join-Path $PSScriptRoot 'validate_models.py') --repo-root $RepoRoot --allow-worktree
if ($LASTEXITCODE -ne 0) {
    throw "Model validation failed with exit code $LASTEXITCODE"
}
