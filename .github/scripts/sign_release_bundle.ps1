[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$InputRoot,
    [Parameter(Mandatory = $true)] [string]$ExpectedAssetsFile,
    [Parameter(Mandatory = $true)] [string]$OutputDirectory,
    [Parameter(Mandatory = $true)] [string]$SourceCommit,
    [Parameter(Mandatory = $true)] [string]$SignerFingerprint,
    [Parameter(Mandatory = $true)] [string]$SignerCertificateThumbprint
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PfxBase64 = [Environment]::GetEnvironmentVariable('SPARK_RELEASE_SIGNING_PFX_BASE64')
$PfxPassword = [Environment]::GetEnvironmentVariable('SPARK_RELEASE_SIGNING_PFX_PASSWORD')

function Fail([string]$Message) { throw $Message }
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { Fail $Message } }

Require ($SourceCommit -match '^[0-9a-f]{40}$') 'Source commit must be a lowercase 40-character SHA.'
Require ($SignerFingerprint -match '^[0-9a-f]{64}$') 'Detached signer fingerprint must be a lowercase SHA-256 digest.'
Require ($SignerCertificateThumbprint -match '^[0-9A-Fa-f]{40}$') 'Signer certificate thumbprint must be a 40-character hexadecimal value.'
Require (-not [string]::IsNullOrWhiteSpace($PfxBase64)) 'Detached signer PFX is missing.'
Require ($null -ne $PfxPassword) 'Detached signer PFX password is missing.'

$rootItem = Get-Item -LiteralPath $InputRoot -ErrorAction Stop
$expectedItem = Get-Item -LiteralPath $ExpectedAssetsFile -ErrorAction Stop
Require (-not ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Input root must not be a reparse point.'
Require (-not ($expectedItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Expected asset inventory must not be a reparse point.'
$root = (Resolve-Path -LiteralPath $InputRoot -ErrorAction Stop).Path
$expectedPath = (Resolve-Path -LiteralPath $ExpectedAssetsFile -ErrorAction Stop).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
Require ([IO.Directory]::Exists($root)) 'Input root is not a directory.'
Require ([IO.File]::Exists($expectedPath)) 'Expected asset inventory is missing.'
Require ($output -ne $root) 'Signature output must not overwrite the release root.'
Require (-not [IO.File]::Exists($output) -and -not [IO.Directory]::Exists($output)) 'Signature output already exists; refusing overwrite.'
[IO.Directory]::CreateDirectory($output) | Out-Null

$names = @(
    [IO.File]::ReadAllLines($expectedPath) |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne '' }
)
Require (@($names).Count -gt 0 -and @($names).Count -le 100) 'Expected asset inventory is empty or too large.'
$checksumNames = @($names | Where-Object { $_ -eq 'SHA256SUMS' })
Require (@($checksumNames).Count -eq 1) 'Expected asset inventory must contain SHA256SUMS.'
$names = @($names | Where-Object { $_ -ne 'SHA256SUMS' })
$folded = @{}
foreach ($name in $names) {
    Require ($name -match '^[A-Za-z0-9][A-Za-z0-9._-]*$') "Unsafe expected asset name: $name"
    $key = $name.ToLowerInvariant()
    Require (-not $folded.ContainsKey($key)) "Case-folded duplicate expected asset: $name"
    $folded[$key] = $true
    $asset = Join-Path $root $name
    Require ([IO.File]::Exists($asset)) "Expected asset is missing: $name"
    Require (-not ([IO.File]::GetAttributes($asset) -band [IO.FileAttributes]::ReparsePoint)) "Expected asset is a reparse point: $name"
}

try {
    $pfxBytes = [Convert]::FromBase64String($PfxBase64)
} catch {
    Remove-Item -LiteralPath $output -Recurse -Force -ErrorAction SilentlyContinue
    Fail 'Detached signer PFX is not valid base64.'
}
try {
    # EphemeralKeySet keeps the private key out of the certificate store and disk.
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $pfxBytes, $PfxPassword,
        [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
} catch {
    Remove-Item -LiteralPath $output -Recurse -Force -ErrorAction SilentlyContinue
    Fail 'Detached signer PFX could not be imported.'
}

$rsa = $null
try {
    Require ($certificate.HasPrivateKey) 'Detached signer PFX has no private key.'
    Require ($certificate.Thumbprint -eq $SignerCertificateThumbprint.ToUpperInvariant()) 'Protected PFX certificate thumbprint does not match the pinned Authenticode signer.'
    $rsa = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    Require ($null -ne $rsa) 'Detached signer PFX does not contain an RSA private key.'
    $publicDer = $rsa.ExportSubjectPublicKeyInfo()
    $derivedFingerprint = ([Security.Cryptography.SHA256]::HashData($publicDer) | ForEach-Object { $_.ToString('x2') }) -join ''
    Require ($derivedFingerprint -eq $SignerFingerprint) 'Pinned detached signer fingerprint does not match the protected PFX.'
    $publicBase64 = [Convert]::ToBase64String($publicDer)
    $pemLines = [Collections.Generic.List[string]]::new()
    for ($offset = 0; $offset -lt $publicBase64.Length; $offset += 64) {
        $length = [Math]::Min(64, $publicBase64.Length - $offset)
        $pemLines.Add($publicBase64.Substring($offset, $length))
    }
    $pem = "-----BEGIN PUBLIC KEY-----`n$([string]::Join("`n", $pemLines))`n-----END PUBLIC KEY-----`n"
    [IO.File]::WriteAllText((Join-Path $output 'spark-release-public-key.pem'), $pem, [Text.UTF8Encoding]::new($false))

    $entries = [Collections.Generic.List[object]]::new()
    foreach ($name in $names) {
        $asset = Join-Path $root $name
        $digest = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
        $signatureName = "$name.sig"
        $signaturePath = Join-Path $output $signatureName
        $stream = [IO.File]::OpenRead($asset)
        try {
            $signature = $rsa.SignData(
                $stream,
                [Security.Cryptography.HashAlgorithmName]::SHA256,
                [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        } finally {
            $stream.Dispose()
        }
        [IO.File]::WriteAllBytes($signaturePath, $signature)
        $entries.Add([ordered]@{
            name = $name
            signature = $signatureName
            artifactSha256 = $digest
            signerFingerprint = $SignerFingerprint
        })
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        algorithm = 'detached-sha256'
        sourceCommit = $SourceCommit
        signerFingerprint = $SignerFingerprint
        artifacts = @($entries)
    }
    $json = $manifest | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $output 'release-signatures.json'), "$json`n", [Text.UTF8Encoding]::new($false))
} catch {
    Remove-Item -LiteralPath $output -Recurse -Force -ErrorAction SilentlyContinue
    throw
} finally {
    if ($null -ne $rsa) { $rsa.Dispose() }
    if ($null -ne $certificate) { $certificate.Dispose() }
    $pfxBytes = $null
    $PfxBase64 = $null
    $PfxPassword = $null
}

Require ([IO.File]::Exists((Join-Path $output 'release-signatures.json')) -and
         [IO.File]::Exists((Join-Path $output 'spark-release-public-key.pem'))) 'Detached signature controls were not created.'
Write-Output 'Detached release signature bundle generated from exact frozen assets.'
