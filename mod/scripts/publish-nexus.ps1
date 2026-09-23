<#
.SYNOPSIS
    Publish the current release to both file entries on the mod's Nexus page.

.DESCRIPTION
    Synced from release-kit. Do not edit it here; the next sync overwrites it.

    A wrapper around Publish-NexusModUpdate.ps1. The ids come from the nexus
    block in release.json, and the archives and changelog follow from the
    version, so a release does not depend on remembering an id. Check the ids
    against the live page with nexus-ids.py.

    One run updates both entries:
      1. the DMM entry (dmmFileId) with <fileBase>-<version>-DMM.zip, as the
         primary mod-manager download, carrying the changelog and setting the
         page version
      2. the manual entry (manualFileId) with <fileBase>-<version>.zip, with no
         changelog, since the first upload already posted it

    The changelog endpoint appends rather than replaces, so publishing one
    version twice posts the text twice. Before sending, this asks the API what
    each entry already lists and skips an entry that already has the version.
    That also makes a second run safe after the first one failed half way.

    The previous version is left listed, never archived. Nexus demotes it to
    old_version on its own once the new one goes up, and an old_version file
    keeps its download button. Nothing needs moving by hand. -ArchivePrevious
    pulls a build outright, which is the thing Seth does not want.

    Report only unless you pass -Apply.

.EXAMPLE
    .\publish-nexus.ps1
    .\publish-nexus.ps1 -Apply
#>
[CmdletBinding()]
param(
    # Nothing is sent to Nexus without this.
    [switch] $Apply,

    # Override the version read from the header.
    [string] $Version,

    # Archive the previous version. Off by default and meant to stay off.
    [switch] $ArchivePrevious
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseConfig.ps1')
$cfg = Get-ReleaseConfig -Version $Version
$Version = $cfg.Version
$nx = $cfg.Data.nexus

foreach ($k in @('modId', 'dmmFileId')) {
    if (-not $nx -or [string]::IsNullOrWhiteSpace([string] $nx.$k)) {
        throw ("nexus.{0} is not set in release.json. Run nexus-ids.py and copy it in." -f $k)
    }
}

# A mod that has never had a manual entry gets one by hand, because the v3 API
# cannot create a file entry. That is done during a release, after GitHub and
# before this runs with -Apply. The manual entry never carries the changelog,
# so the hand upload loses nothing.
$handStep = [string]::IsNullOrWhiteSpace([string] $nx.manualFileId)
if ($handStep) {
    $how = ("nexus.manualFileId is empty: the page has no manual file entry yet. On the page's Files tab, " +
            "add a file in the Main files category, upload {0}, and type the version as {1} exactly, since " +
            "that string is what marks it as already listed and cannot be corrected by API. Leave the " +
            "mod-manager download option off. Then run nexus-ids.py, put the new id in release.json as " +
            "nexus.manualFileId, and run this with -Apply. It skips that entry and publishes the DMM one.") -f
           (Split-Path $cfg.ZipManual -Leaf), $cfg.Version
    if ($Apply) { throw $how }
}
foreach ($f in @($cfg.ZipDmm, $cfg.ZipManual)) {
    if (-not (Test-Path -LiteralPath $f)) { throw "No archive at $f. Run package.ps1 first." }
}
if (-not (Test-Path -LiteralPath $cfg.Changelog)) { throw "No changelog at $($cfg.Changelog). Write it before publishing." }
if ((Get-Content -LiteralPath $cfg.Changelog -Raw) -match '\{\{[A-Z_]+\}\}') {
    throw "$($cfg.Changelog) still has a {{PLACEHOLDER}}. Run release-docs.py stamp."
}

Write-Host ("{0} {1}, page {2}" -f $cfg.Name, $Version, $nx.page) -ForegroundColor Cyan

$entries = @(
    [pscustomobject] @{
        Label = 'DMM'; FileId = [string] $nx.dmmFileId; Archive = $cfg.ZipDmm
        DisplayName = ('{0} {1} DMM' -f $cfg.FileBase, $Version); Changelog = $true
    },
    [pscustomobject] @{
        Label = 'manual'; FileId = [string] $nx.manualFileId; Archive = $cfg.ZipManual
        DisplayName = ('{0} {1} manual' -f $cfg.FileBase, $Version); Changelog = $false
    }
)
if ($handStep) { $entries = @($entries[0]) }

# What is already up there, per entry.
$key = Read-ReleaseKey -Name 'NEXUS_API_KEY'
foreach ($e in $entries) {
    $e | Add-Member -NotePropertyName Already -NotePropertyValue $null
    if ([string]::IsNullOrWhiteSpace($key)) { continue }
    try {
        $existing = Invoke-RestMethod -Uri "https://api.nexusmods.com/v3/mod-files/$($e.FileId)/versions" `
                                      -Headers @{ 'apikey' = $key } -Method Get
        $hit = $existing.data.versions | Where-Object { $_.version -eq $Version } | Select-Object -First 1
        if ($hit) { $e.Already = $hit.uploaded_at }
    } catch {
        if ($Apply) {
            throw (("Could not read what file {0} already lists ({1}). Nothing was sent, because " +
                    "sending blind can post the changelog twice.") -f $e.FileId, $_.Exception.Message)
        }
        Write-Warning ("Could not read what file {0} already lists: {1}" -f $e.FileId, $_.Exception.Message)
    }
}
if ($Apply -and [string]::IsNullOrWhiteSpace($key)) {
    throw "No NEXUS_API_KEY in the environment or keys.local.env beside this script."
}

# The changelog rides on the DMM upload. If the DMM entry already has this
# version, its changelog is already posted, so the manual one must not post it.
foreach ($e in $entries) {
    Write-Host ""
    if ($e.Already) {
        Write-Host ("{0} entry {1} already lists {2} (uploaded {3}). Skipped." -f $e.Label, $e.FileId, $Version, $e.Already) -ForegroundColor Yellow
        continue
    }
    Write-Host ("{0} entry {1}: {2}" -f $e.Label, $e.FileId, (Split-Path $e.Archive -Leaf)) -ForegroundColor Cyan
    $call = @{
        FilePath    = $e.Archive
        FileId      = $e.FileId
        ModId       = [string] $nx.modId
        Version     = $Version
        DisplayName = $e.DisplayName
        Category    = 'main'
    }
    if ($e.Changelog) {
        $call['ChangelogPath']             = $cfg.Changelog
        $call['UpdateModVersion']          = $true
        $call['PrimaryModManagerDownload'] = $true
    }
    if ($ArchivePrevious) { $call['ArchiveExistingFile'] = $true }
    if ($Apply)           { $call['Apply']               = $true }
    & (Join-Path $PSScriptRoot 'Publish-NexusModUpdate.ps1') @call
}

Write-Host ""
if ($Apply) {
    Write-Host "Still by hand, because the v3 API has no endpoint for either:" -ForegroundColor Yellow
} else {
    if ($handStep) {
        Write-Host "HAND STEP before -Apply:" -ForegroundColor Yellow
        Write-Host "  $how"
        Write-Host ""
    }
    Write-Host "REPORT ONLY. Nothing was sent. After -Apply, these stay by hand:" -ForegroundColor Yellow
}
Write-Host ("  paste the page description   private\nexus\nexus-description.bbcode")
Write-Host ("  post the update              private\nexus\nexus-post-{0}.bbcode" -f $Version)
