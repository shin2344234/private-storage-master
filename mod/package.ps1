# Builds the two Nexus archives from mod\dist and prints their SHA-256.
#
#   powershell -ExecutionPolicy Bypass -File "<repo>\mod\package.ps1"
#
# PrivateStorageMaster-<version>.zip      manual install: plugin, readme, licences
# PrivateStorageMaster-<version>-DMM.zip  Definitive Mod Manager: the plugin alone,
#                                  which is all DMM registers.
#
# Neither carries an ini. The plugin writes a fully commented one beside itself
# the first time it runs, and it only brings bindings over from an old
# PrivateStorageAnywhere.ini when no PrivateStorageMaster.ini exists yet.
#
# Run build.bat first; this script packages what is in dist and refuses if the
# plugin there is older than the sources.
[CmdletBinding()]
param([switch] $Unsigned)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $here 'dist'
$root = Split-Path -Parent $here

$version = (Select-String -Path (Join-Path $here 'src\version.h') -Pattern '#define PSM_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
if (-not $version) { throw 'no PSM_VERSION in src\version.h' }

$asi = Join-Path $dist 'PrivateStorageMaster.asi'
if (-not (Test-Path $asi)) { throw "missing $asi; run build.bat" }

$newestSource = Get-ChildItem (Join-Path $here 'src') -Recurse -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSource.LastWriteTime -gt (Get-Item $asi).LastWriteTime) {
    throw "$($newestSource.Name) is newer than the built plugin; run build.bat first"
}

# Sign before packaging, never after: these archives carry the plugin and the
# checksums on the Nexus page are of the signed file.
if (-not $Unsigned) {
    $sig = Get-AuthenticodeSignature $asi
    if ($sig.Status -ne 'Valid') {
        throw "$asi is not signed (status $($sig.Status)). Run mod\scripts\sign.ps1, or pass -Unsigned to package anyway."
    }
    Write-Host ("Signed by {0}" -f $sig.SignerCertificate.Subject)
}

if (-not (Test-Path (Join-Path $root 'LICENSE'))) {
    throw "No LICENSE at the repo root. The archives carry it next to the plugin."
}

$staging = Join-Path $env:TEMP "psm-package-$version"
Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $staging | Out-Null
Copy-Item $asi $staging
Copy-Item (Join-Path $here 'README.md') $staging
Copy-Item (Join-Path $root 'LICENSE') $staging
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $staging

$plain = Join-Path $dist "PrivateStorageMaster-$version.zip"
$dmm = Join-Path $dist "PrivateStorageMaster-$version-DMM.zip"
Remove-Item $plain, $dmm -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $plain
Compress-Archive -Path $asi -DestinationPath $dmm
Remove-Item $staging -Recurse -Force

Write-Host "`nPrivate Storage Master $version"
foreach ($z in @($plain, $dmm)) {
    Write-Host ("  {0,-34} {1,9:N0} bytes" -f (Split-Path $z -Leaf), (Get-Item $z).Length)
    [System.IO.Compression.ZipFile]::OpenRead($z).Entries | ForEach-Object { Write-Host "      $($_.FullName)" }
}
Write-Host "`nSHA-256:"
Get-FileHash $dmm, $plain, $asi -Algorithm SHA256 |
    ForEach-Object { Write-Host ("{0}  {1}" -f $_.Hash.ToLower(), (Split-Path $_.Path -Leaf)) }
