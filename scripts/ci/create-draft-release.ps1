# Creates a DRAFT GitHub release with the assembled files, and nothing else: no tag is pushed (a
# draft names the tag it would create when published; it does not create it), and the release is
# not published. Run by the release workflow's last job, whose GH_TOKEN can write contents.
#
#   powershell -File scripts\ci\create-draft-release.ps1 -Tag <tag> -Commit <sha> -Folder <assets and files>
#
# Refuses to touch a release that is published: if one with this tag exists, it stops. An earlier
# draft of the same tag (a rerun) is replaced.
param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][string]$Commit, [Parameter(Mandatory)][string]$Folder)

$ErrorActionPreference = "Stop"
if ($Tag -notmatch '^[A-Za-z0-9._-]+$') { throw "the tag has characters other than letters, digits, . _ -" }
if ($Commit -notmatch '^[0-9a-f]{40}$') { throw "the commit is not a full hash" }
$repository = $env:GITHUB_REPOSITORY
if (-not $repository) { throw "GITHUB_REPOSITORY is not set (this runs in the release workflow)" }
if (-not $env:GH_TOKEN) { throw "GH_TOKEN is not set" }

function Gh {
    $output = & gh @args
    if ($LASTEXITCODE -ne 0) { throw "gh $($args -join ' ') failed ($LASTEXITCODE)" }
    $output
}

# The releases with this tag name, published ones and drafts, one JSON object per line.
function ReleasesOfTag {
    $lines = Gh api "repos/$repository/releases?per_page=100" --paginate --jq '.[] | {id, tag_name, draft, html_url, published_at, assets: [.assets[] | {name, size, state}]} | tojson'
    @($lines | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json }) | Where-Object { $_.tag_name -eq $Tag }
}

$existing = @(ReleasesOfTag)
foreach ($r in $existing) {
    if (-not $r.draft) { throw "release $($r.html_url) with the tag $Tag is published: this workflow only makes drafts and will not touch it" }
}
foreach ($r in $existing) {
    "replacing the earlier draft of $Tag (id $($r.id))"
    Gh api -X DELETE "repos/$repository/releases/$($r.id)" | Out-Null
}

$names = @("MCLA-Launcher-$Tag-win-x64.zip") +
         @(Get-ChildItem -LiteralPath $Folder -File -Filter "rexglue-sdk-*.zip" | ForEach-Object { $_.Name }) +
         @("release.json", "NOTICES.txt", "SHA256SUMS")
$files = @($names | ForEach-Object { Join-Path $Folder $_ })
foreach ($f in $files) { if (-not (Test-Path -LiteralPath $f)) { throw "missing asset: $f" } }

# --draft is what makes it a draft. --target is the commit the tag would be created on at publishing.
Gh release create $Tag --repo $repository --draft --target $Commit --title "Draft $Tag" --notes-file (Join-Path $Folder "release-notes.md") @files | Out-Null

# Look at what was made.
$made = @(ReleasesOfTag)
if ($made.Count -ne 1) { throw "expected one release for $Tag, found $($made.Count)" }
$release = $made[0]
if (-not $release.draft) { throw "the release $($release.html_url) is not a draft" }
"draft release: $($release.html_url)"
"draft: $($release.draft); published at: $($release.published_at)"
foreach ($asset in $release.assets) { "  {0,-62} {1,12:N0} bytes  {2}" -f $asset.name, $asset.size, $asset.state }
if ($release.assets.Count -ne $files.Count) { throw "$($release.assets.Count) assets were uploaded, not $($files.Count)" }
$ErrorActionPreference = "Continue"
& gh api "repos/$repository/git/ref/tags/$Tag" 2>$null | Out-Null
$tagExists = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = "Stop"
if ($tagExists) { "note: a tag $Tag exists in the repository already (this workflow did not make it)" } else { "no tag $Tag exists (a draft does not create one)" }
$global:LASTEXITCODE = 0
