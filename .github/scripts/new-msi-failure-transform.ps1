<#
.SYNOPSIS
Build a qualification-only MSI transform that forces an in-script failure after files are written.

.DESCRIPTION
The interrupted-activation drill in qualify-windows-msi.py must prove that a
failed or interrupted install or upgrade never replaces the working install.
Shipping a failure hook in the production MSI is not acceptable, so this script
derives a transform instead: it copies the verified MSI, adds a deferred
custom action SparkQualFail (type 50 | msidbCustomActionTypeInScript) that runs
`cmd.exe /c exit 1` immediately after InstallFiles, and writes the difference
as a transform. Applying it with TRANSFORMS=<Out> makes Windows Installer fail
after the file copy and run its rollback script, exiting 1603.

The input MSI is only read; its bytes and digest are unchanged, so the caller's
private-copy digest checks remain valid. -Out must not exist.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Msi,
    [Parameter(Mandatory = $true)] [string]$Out
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

$msiOpenDatabaseModeReadOnly = 0
$msiOpenDatabaseModeTransact = 1
$customActionType = 50 -bor 1024

$source = (Resolve-Path -LiteralPath $Msi -ErrorAction Stop).Path
$sourceItem = Get-Item -LiteralPath $source -ErrorAction Stop
Require (-not ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Input MSI must not be a reparse point.'
$output = [IO.Path]::GetFullPath($Out)
Require (-not [IO.File]::Exists($output) -and -not [IO.Directory]::Exists($output)) 'Transform output already exists; refusing overwrite.'
$cmd = Join-Path ([Environment]::GetFolderPath('System')) 'cmd.exe'
Require ([IO.File]::Exists($cmd)) "System cmd.exe not found at $cmd."
Require (-not $cmd.Contains("'")) 'System cmd.exe path cannot be embedded in MSI SQL.'

$work = Join-Path ([IO.Path]::GetDirectoryName($output)) ('.spark-qual-fail-' + [Guid]::NewGuid().ToString('N') + '.msi')
Copy-Item -LiteralPath $source -Destination $work
$installer = $null
$original = $null
$modified = $null
try {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $original = $installer.OpenDatabase($source, $msiOpenDatabaseModeReadOnly)
    $modified = $installer.OpenDatabase($work, $msiOpenDatabaseModeTransact)

    # COM void methods emit $null into the pipeline; [void] keeps function results exact.
    function Invoke-Sql([string]$Query) {
        $view = $modified.OpenView($Query)
        try { [void]$view.Execute() } finally { [void]$view.Close() }
    }
    function Get-Sequence([string]$Action) {
        $view = $modified.OpenView("SELECT ``Sequence`` FROM ``InstallExecuteSequence`` WHERE ``Action`` = '$Action'")
        try {
            [void]$view.Execute()
            $record = $view.Fetch()
            if ($null -eq $record) { return $null }
            return [int]$record.IntegerData(1)
        } finally { [void]$view.Close() }
    }
    function Test-SequenceUsed([int]$Sequence) {
        $view = $modified.OpenView("SELECT ``Action`` FROM ``InstallExecuteSequence`` WHERE ``Sequence`` = $Sequence")
        try {
            [void]$view.Execute()
            return $null -ne $view.Fetch()
        } finally { [void]$view.Close() }
    }

    $installFiles = Get-Sequence 'InstallFiles'
    $installFinalize = Get-Sequence 'InstallFinalize'
    Require ($null -ne $installFiles -and $null -ne $installFinalize) 'MSI InstallExecuteSequence lacks InstallFiles or InstallFinalize.'
    Require ($null -eq (Get-Sequence 'SparkQualFail')) 'MSI already contains a SparkQualFail action.'
    $sequence = $installFiles + 1
    while (Test-SequenceUsed $sequence) { $sequence++ }
    Require ($sequence -lt $installFinalize) 'No free InstallExecuteSequence slot between InstallFiles and InstallFinalize.'

    Invoke-Sql "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('SPARKQUALFAILEXE', '$cmd')"
    Invoke-Sql "INSERT INTO ``CustomAction`` (``Action``, ``Type``, ``Source``, ``Target``) VALUES ('SparkQualFail', $customActionType, 'SPARKQUALFAILEXE', '/c exit 1')"
    Invoke-Sql "INSERT INTO ``InstallExecuteSequence`` (``Action``, ``Condition``, ``Sequence``) VALUES ('SparkQualFail', 'NOT Installed', $sequence)"
    [void]$modified.Commit()

    Require ([bool]$modified.GenerateTransform($original, $output)) 'Windows Installer produced an empty transform.'
    [void]$modified.CreateTransformSummaryInfo($original, $output, 0, 0)
    Require ([IO.File]::Exists($output) -and (Get-Item -LiteralPath $output).Length -gt 0) 'Transform was not written.'
    Write-Output ("SparkQualFail transform: sequence={0} after InstallFiles={1} -> {2}" -f $sequence, $installFiles, $output)
} finally {
    foreach ($comObject in @($modified, $original, $installer)) {
        if ($null -ne $comObject) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($comObject) }
    }
    $modified = $null
    $original = $null
    $installer = $null
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    Remove-Item -LiteralPath $work -Force -ErrorAction SilentlyContinue
}
