# Creates a DRAFT GitHub release with the assembled files, and nothing else: no tag is pushed (a
# draft names the tag it would create when published; it does not create it), and the release is
# not published. Run by the release workflow's last job, whose GH_TOKEN can write contents.
#
#   powershell -File scripts\ci\create-draft-release.ps1 -Tag <tag> -Commit <sha> -Folder <assets and files>
#
# Refuses to touch a release that is published: if one with this tag exists, it stops. An earlier
# draft of the same tag that this script made (its text starts with the marker release-notes.md
# begins with) is replaced; a draft someone else wrote is not touched, and then this stops too.
param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][string]$Commit, [Parameter(Mandatory)][string]$Folder)

$ErrorActionPreference = "Stop"
# A letter or digit first (a tag starting with - would be read by gh as an option), no more than 63
# characters after it, nothing git refuses in a ref name; case-sensitive, and \z (not $) ends it.
if ($Tag -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,62}\z' -or $Tag -match '\.\.|\.$|\.lock$') { throw "not a tag name this script makes drafts for: $Tag" }
if ($Commit -cnotmatch '^[0-9a-f]{40}\z') { throw "the commit is not a full hash" }
$repository = $env:GITHUB_REPOSITORY
if (-not $repository) { throw "GITHUB_REPOSITORY is not set (this runs in the release workflow)" }
if (-not $env:GH_TOKEN) { throw "GH_TOKEN is not set" }
$marker = "<!-- mcla-release-draft -->"

function Invoke-Gh {
    $output = & gh.exe @args
    if ($LASTEXITCODE -ne 0) { throw "gh $($args -join ' ') failed ($LASTEXITCODE)" }
    $output
}

# The releases with this tag name, published ones and drafts, one JSON object per line.
function ReleasesOfTag {
    $lines = Invoke-Gh api "repos/$repository/releases?per_page=100" --paginate --jq '.[] | {id, tag_name, draft, html_url, published_at, body, assets: [.assets[] | {name, size, state}]} | tojson'
    @($lines | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json }) | Where-Object { $_.tag_name -ceq $Tag }
}

# What is to be uploaded exists, before anything is deleted.
$names = @("MCLA-Launcher-$Tag-win-x64.zip") +
         @(Get-ChildItem -LiteralPath $Folder -File -Filter "rexglue-sdk-*.zip" | ForEach-Object { $_.Name }) +
         @(Get-ChildItem -LiteralPath $Folder -File -Filter "rexglue-sdk-source-*.tar.xz" | ForEach-Object { $_.Name }) +
         @("release.json", "NOTICES.txt", "SHA256SUMS")
$files = @($names | ForEach-Object { Join-Path $Folder $_ })
foreach ($f in $files) { if (-not (Test-Path -LiteralPath $f)) { throw "missing asset: $f" } }
$notes = Join-Path $Folder "release-notes.md"
if (-not (Test-Path -LiteralPath $notes) -or -not (Get-Content -LiteralPath $notes -TotalCount 1).StartsWith($marker)) { throw "release-notes.md is missing or lacks the marker $marker" }

$existing = @(ReleasesOfTag)
foreach ($r in $existing) {
    if (-not $r.draft) { throw "release $($r.html_url) with the tag $Tag is published: this workflow only makes drafts and will not touch it" }
    if (-not "$($r.body)".StartsWith($marker)) { throw "a draft of $Tag that this workflow did not make exists ($($r.html_url)): delete or rename it" }
}
foreach ($r in $existing) {
    "replacing the earlier draft of $Tag (id $($r.id))"
    Invoke-Gh api -X DELETE "repos/$repository/releases/$($r.id)" | Out-Null
}

# --draft is what makes it a draft. --target is the commit the tag would be created on at publishing.
Invoke-Gh release create $Tag --repo $repository --draft --target $Commit --title $Tag --notes-file $notes @files | Out-Null

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
& gh.exe api "repos/$repository/git/ref/tags/$Tag" 2>$null | Out-Null
$tagExists = ($LASTEXITCODE -eq 0)
$ErrorActionPreference = "Stop"
if ($tagExists) { "note: a tag $Tag exists in the repository already (this workflow did not make it)" } else { "no tag $Tag exists (a draft does not create one)" }
$global:LASTEXITCODE = 0
