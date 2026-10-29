# The update path of a release, on a scratch folder: an install of the PREVIOUS release, built the way a player's first
# build is, is updated to the CANDIDATE the way the launcher updates one, and has to build. Nothing here boots the game
# (that needs the dev tools: see the end of this file's output, and scripts\ci\README.md).
#
#   powershell -File scripts\ci\check-update.ps1 -PreviousTag v0.3.1 -PreviousBundle <zip> -Candidate <tag or commit> `
#       -CandidateBundle <zip> -Root <empty scratch folder> -GameData <extracted game> -ToolsDir <portable tools>
#
#   -PreviousTag, -PreviousBundle    the release the players have now, and its SDK bundle (a zip). -PreviousBundleSha256 is the
#                                    hash in that release's release.json; left out, it is read from a release.json beside the zip
#   -Candidate, -CandidateBundle     the release to be published: its tag, or its commit while the draft has no tag yet, and the
#                                    draft's SDK bundle (-CandidateBundleSha256 as above)
#   -EarlierTag, -EarlierBundle      optional: the release before the previous one, for an install that the previous release's
#                                    update may have left half-updated: it is installed, updated to -PreviousTag (whatever comes
#                                    of that), and then updated to the candidate, which has to build (-EarlierBundleSha256 as above)
#   -Root                            a scratch folder that is empty or not there yet; the installs are made in it (update\ and
#                                    stuck\), with logs\ and RESULTS.txt. Nothing outside it is written, except the build tools'
#                                    own caches. It is refused when it is not empty or is inside (or holds) the player's
#                                    %LOCALAPPDATA%\Programs\MCLA. Delete it yourself afterwards.
#   -GameData                        the extracted game (default.xex and the rest); each install gets it as a junction at game\
#                                    and it is only read. Removed again at the end (rmdir of the link, never a recursive delete).
#   -ToolsDir                        the folder with llvm-mingw-*, cmake-*, ninja* and mingit-* (as in the launcher's tools\)
#   -Origin                          the repository to clone and fetch (default: the fork on GitHub)
#
# The install is a clone of -Origin at the tag; the update is git (what codegen rewrites put back; a fast-forward merge with
# --autostash, else a forced detached checkout; Updates.cs MoveToAsync), launcher\build-options.json naming the new bundle
# (Updates.cs WriteOptions), and then launcher\build-game.ps1 -Toolchain mingw -ToolsDir <tools> -SdkBundle <zip>
# -SdkBundleSha256 <hash>, which is what the launcher runs. Both builds use the one -ToolsDir (the launcher would unpack the
# candidate's own tools; a change of tools.json is not tested here).
#
# -PreflightOnly checks the arguments and the folders and stops (nothing is written).
#
# Output: one "PASS <step> - ..." or "FAIL <step> - ..." line per check, also in <Root>\RESULTS.txt; the build logs are in
# <Root>\logs\. Exit code 0 only when every check passed. It takes two builds of about a minute each (more with a slow PC).
param(
    [Parameter(Mandatory)][string]$PreviousTag,
    [Parameter(Mandatory)][string]$PreviousBundle,
    [string]$PreviousBundleSha256 = "",
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$CandidateBundle,
    [string]$CandidateBundleSha256 = "",
    [string]$EarlierTag = "",
    [string]$EarlierBundle = "",
    [string]$EarlierBundleSha256 = "",
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$GameData,
    [Parameter(Mandatory)][string]$ToolsDir,
    [string]$Origin = "https://github.com/RenanQueiroz/mcla-recomp.git",
    [int]$BuildTimeoutMin = 25,
    [switch]$PreflightOnly
)

$ErrorActionPreference = "Continue"
$R = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$Game = [IO.Path]::GetFullPath($GameData).TrimEnd('\')
$Tools = [IO.Path]::GetFullPath($ToolsDir).TrimEnd('\')
$LogDir = "$R\logs"
$ResultsFile = "$R\RESULTS.txt"
$script:Failed = 0
$script:GitExit = 0
$env:GIT_CONFIG_COUNT = "1"; $env:GIT_CONFIG_KEY_0 = "core.longpaths"; $env:GIT_CONFIG_VALUE_0 = "true"

function Note([string]$Text) { Write-Host "$(Get-Date -Format s) $Text" }
function Result([string]$Verdict, [string]$Step, [string]$Text) {
    $line = "$Verdict $Step - $Text"
    Write-Host $line
    if (Test-Path -LiteralPath $LogDir) { Add-Content -LiteralPath $ResultsFile -Value $line -Encoding ascii }
    if ($Verdict -eq "FAIL") { $script:Failed++ }
}
function Same-Or-Inside([string]$Path, [string]$Folder) {
    return $Path.Equals($Folder, [StringComparison]::OrdinalIgnoreCase) -or $Path.StartsWith($Folder + "\", [StringComparison]::OrdinalIgnoreCase)
}

# git and its output (stdout and stderr) as lines; the exit code is left in $script:GitExit.
function Git-Run([string[]]$GitArgs) {
    $lines = @(& git.exe @GitArgs 2>&1 | ForEach-Object { "$_" })
    $script:GitExit = $LASTEXITCODE
    return $lines
}

# A program with its output in <Root>\logs\<Name>.out/.err and a time limit (its process tree is stopped at the limit).
# Returns the exit code (-1 at the limit, -2 when it could not be started).
function Invoke-Child([string]$Name, [string]$File, [string]$ArgText, [string]$Dir, [int]$TimeoutSec) {
    $out = "$LogDir\$Name.out"
    $err = "$LogDir\$Name.err"
    Note "start $Name (limit $TimeoutSec s)"
    try {
        $p = Start-Process -FilePath $File -ArgumentList $ArgText -WorkingDirectory $Dir -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
    } catch {
        Note "${Name}: could not start: $($_.Exception.Message)"
        return -2
    }
    $null = $p.Handle   # keeps the exit code readable after the process has gone (Windows PowerShell 5.1)
    $began = Get-Date
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Note "${Name}: still running after $TimeoutSec s; stopping its process tree (PID $($p.Id))"
        & taskkill.exe /PID $p.Id /T /F | Out-Null
        [void]$p.WaitForExit(15000)
        return -1
    }
    $p.WaitForExit()
    Note "$Name finished: exit $($p.ExitCode) after $([int]((Get-Date) - $began).TotalSeconds) s"
    return $p.ExitCode
}
function Read-Log([string]$Name) {
    $text = ""
    foreach ($ext in "out", "err") {
        $f = "$LogDir\$Name.$ext"
        if (Test-Path -LiteralPath $f) { $text += (Get-Content -LiteralPath $f -Raw -ErrorAction SilentlyContinue) + "`n" }
    }
    return $text
}

# A bundle zip: it exists, and its SHA-256 is the one given (or the one in a release.json beside it).
function Resolve-Bundle([string]$Label, [string]$Zip, [string]$Sha, [System.Collections.ArrayList]$Problems) {
    if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { [void]$Problems.Add("$Label bundle: no such file $Zip"); return $null }
    $zipFull = [IO.Path]::GetFullPath($Zip)
    $release = $null
    $json = Join-Path (Split-Path -Parent $zipFull) "release.json"
    if (Test-Path -LiteralPath $json) { try { $release = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json } catch { $release = $null } }
    if (-not $Sha) {
        if ($release -and $release.sdkBundle -and $release.sdkBundle.asset -eq (Split-Path -Leaf $zipFull)) { $Sha = "$($release.sdkBundle.sha256)" }
        else { [void]$Problems.Add("$Label bundle: give its SHA-256 (release.json's sdkBundle.sha256); no release.json beside the zip names it"); return $null }
    }
    $Sha = $Sha.Trim().ToLowerInvariant()
    $actual = (Get-FileHash -LiteralPath $zipFull -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha) { [void]$Problems.Add("$Label bundle: SHA-256 of $zipFull is $actual, not $Sha"); return $null }
    return [pscustomobject]@{ Zip = $zipFull; Sha = $Sha; Release = $release }
}

# What the build folder and the unpacked SDK look like (evidence for a log).
function Describe-State([string]$Repo, [string]$Label) {
    $head = (Git-Run @("-C", $Repo, "rev-parse", "--short", "HEAD")) -join ""
    $pch = Get-Item -LiteralPath "$Repo\out\build\win-amd64-release\CMakeFiles\mcla_recomp.dir\cmake_pch.hxx.pch" -ErrorAction SilentlyContinue
    $ctx = Get-Item -LiteralPath "$Repo\third_party\rexglue-sdk-bin\include\rex\ppc\context.h" -ErrorAction SilentlyContinue
    $rec = Get-Content -LiteralPath "$Repo\third_party\rexglue-sdk-bin\.mcla-bundle" -TotalCount 1 -ErrorAction SilentlyContinue
    $exe = Get-Item -LiteralPath "$Repo\out\build\win-amd64-release\mcla.exe" -ErrorAction SilentlyContinue
    Note ("state $Label : HEAD $head; PCH " + $(if ($pch) { $pch.LastWriteTime.ToString("s") } else { "absent" }) +
          "; SDK context.h " + $(if ($ctx) { $ctx.LastWriteTime.ToString("s") } else { "absent" }) +
          "; bundle record " + $(if ($rec) { "$rec".Substring(0, [Math]::Min(12, "$rec".Length)) } else { "none" }) +
          "; dated marker " + (Test-Path -LiteralPath "$Repo\third_party\.mcla-bundle-dated") +
          "; mcla.exe " + $(if ($exe) { $exe.LastWriteTime.ToString("s") } else { "absent" }))
}

# What a build's output says about the SDK bundle and the precompiled header.
function Get-BuildFacts([string]$Text) {
    return [pscustomobject]@{
        Already   = [bool]($Text -match "SDK bundle [0-9a-f]{64} is unpacked already")
        Unpacking = [bool]($Text -match "unpacking the SDK bundle")
        Dated     = [bool]($Text -match "SDK bundle files dated now")
        PchError  = [bool]($Text -match "has been modified since the precompiled header")
    }
}
function Describe-Facts($Facts) {
    $parts = @()
    if ($Facts.Unpacking) { $parts += "bundle unpacked" } elseif ($Facts.Already) { $parts += "bundle unpacked already" }
    $parts += $(if ($Facts.Dated) { "'SDK bundle files dated now' line seen" } else { "no 'SDK bundle files dated now' line" })
    return $parts -join "; "
}

function Make-Junction([string]$Link) {
    cmd /c "mklink /J `"$Link`" `"$Game`"" | Out-Null
    return ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath "$Link\default.xex"))
}
function Remove-Junction([string]$Link) {
    if (Test-Path -LiteralPath $Link) { cmd /c "rmdir `"$Link`"" | Out-Null }
}

# An install made like a player's first build: a clone of -Origin at the tag (the wizard's clone), the game junction, and
# build-game.ps1 with that release's bundle. Returns the repo path, or $null after a FAIL line.
function New-Install([string]$Step, [string]$Dir, [string]$Tag, $Bundle) {
    $repo = "$Dir\mcla-recomp"
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $t = Get-Date
    $o = Git-Run @("-c", "core.longpaths=true", "-c", "advice.detachedHead=false", "clone", "--quiet", "--branch", $Tag, $Origin, $repo)
    if ($script:GitExit -ne 0) { Result "FAIL" $Step "git clone of $Tag from $Origin failed: $(($o | Select-Object -Last 3) -join ' | ')"; return $null }
    $head = ((Git-Run @("-C", $repo, "rev-parse", "HEAD")) -join "").Trim()
    if ($Bundle.Release -and $Bundle.Release.commit -and $Bundle.Release.commit -ne $head) {
        Result "FAIL" $Step "the release.json beside $(Split-Path -Leaf $Bundle.Zip) names commit $($Bundle.Release.commit), but $Tag is $head"
        return $null
    }
    if (-not (Make-Junction "$repo\game")) { Result "FAIL" $Step "could not link $repo\game to $Game"; return $null }
    $code = Invoke-Child "$Step-build" "powershell.exe" "-NoProfile -NonInteractive -File `"$repo\launcher\build-game.ps1`" -Toolchain mingw -ToolsDir `"$Tools`" -SdkBundle `"$($Bundle.Zip)`" -SdkBundleSha256 $($Bundle.Sha)" "$repo\launcher" ($BuildTimeoutMin * 60)
    $built = "$(Get-Content -LiteralPath "$repo\launcher\built-commit.txt" -ErrorAction SilentlyContinue | Select-Object -First 1)".Trim()
    Describe-State $repo "$Step after the build"
    if ($code -ne 0 -or $built -ne $head -or -not (Test-Path -LiteralPath "$repo\out\build\win-amd64-release\mcla.exe")) {
        Result "FAIL" $Step "the $Tag install did not build: exit $code, built commit '$built' (want $head), see $LogDir\$Step-build.out"
        return $null
    }
    Result "PASS" $Step "the $Tag install ($($head.Substring(0, 7))) built with its own bundle in $([int]((Get-Date) - $t).TotalSeconds) s"
    return $repo
}

# The update, the way the launcher does it (Updates.cs MoveToAsync + WriteOptions, then the build): git moves the checkout
# (what codegen rewrites is put back first; a fast-forward merge, else a forced detached checkout), build-options.json names
# the new bundle, and build-game.ps1 builds. Returns $true when it built; the verdict line is written either way.
function Update-Install([string]$Step, [string]$Repo, [string]$Ref, $Bundle, [string]$Label, [bool]$MayFail) {
    $t = Get-Date
    [void](Git-Run @("-C", $Repo, "fetch", "--quiet", "--tags", "origin", "+refs/heads/*:refs/remotes/origin/*"))
    $target = ((Git-Run @("-C", $Repo, "rev-parse", "--verify", "$Ref^{commit}")) -join "").Trim()
    if ($script:GitExit -ne 0 -and $Ref -match "^[0-9a-fA-F]{7,40}$") {
        [void](Git-Run @("-C", $Repo, "fetch", "--quiet", "origin", $Ref))
        $target = ((Git-Run @("-C", $Repo, "rev-parse", "--verify", "$Ref^{commit}")) -join "").Trim()
    }
    if ($script:GitExit -ne 0 -or $target -notmatch "^[0-9a-f]{40}$") { Result "FAIL" $Step "$Ref is not a commit of ${Origin}: $target"; return $false }
    if ($Bundle.Release -and $Bundle.Release.commit -and $Bundle.Release.commit -ne $target) {
        Result "FAIL" $Step "the release.json beside $(Split-Path -Leaf $Bundle.Zip) names commit $($Bundle.Release.commit), but $Ref is $target"
        return $false
    }
    $head = ((Git-Run @("-C", $Repo, "rev-parse", "HEAD")) -join "").Trim()
    if ($head -ne $target) {
        foreach ($f in "mcla_manifest.toml", "generated/rexglue.cmake") {
            [void](Git-Run @("-C", $Repo, "ls-files", "--error-unmatch", "--", $f))
            if ($script:GitExit -eq 0) { [void](Git-Run @("-C", $Repo, "checkout", "--quiet", "--", $f)) }
        }
        [void](Git-Run @("-C", $Repo, "merge-base", "--is-ancestor", $head, $target))
        if ($script:GitExit -eq 0) {
            $m = Git-Run @("-c", "user.name=mcla", "-c", "user.email=mcla@localhost", "-C", $Repo, "merge", "--ff-only", "--autostash", $target)
        } else {
            $m = Git-Run @("-c", "advice.detachedHead=false", "-C", $Repo, "checkout", "--quiet", "--force", "--detach", $target)
        }
        if ($script:GitExit -ne 0) { Result "FAIL" $Step "git could not move the checkout to ${Ref}: $(($m | Select-Object -Last 3) -join ' | ')"; return $false }
        $unmerged = @(Git-Run @("-C", $Repo, "ls-files", "--unmerged"))
        if ($unmerged.Count -gt 0 -and "$($unmerged[0])".Trim()) { Result "FAIL" $Step "unmerged files after the move to $Ref"; return $false }
    }
    $options = [ordered]@{ toolchain = "mingw"; toolsDir = $Tools; sdkBundle = $Bundle.Zip; sdkBundleSha256 = $Bundle.Sha; sdkDir = "$Repo\third_party\rexglue-sdk-bin" }
    Set-Content -LiteralPath "$Repo\launcher\build-options.json" -Value (($options | ConvertTo-Json) + "`r`n") -Encoding UTF8
    Describe-State $Repo "$Step before the build"
    $code = Invoke-Child "$Step-build" "powershell.exe" "-NoProfile -NonInteractive -File `"$Repo\launcher\build-game.ps1`" -Toolchain mingw -ToolsDir `"$Tools`" -SdkBundle `"$($Bundle.Zip)`" -SdkBundleSha256 $($Bundle.Sha)" "$Repo\launcher" ($BuildTimeoutMin * 60)
    $facts = Get-BuildFacts (Read-Log "$Step-build")
    $built = "$(Get-Content -LiteralPath "$Repo\launcher\built-commit.txt" -ErrorAction SilentlyContinue | Select-Object -First 1)".Trim()
    $record = "$(Get-Content -LiteralPath "$Repo\third_party\rexglue-sdk-bin\.mcla-bundle" -ErrorAction SilentlyContinue | Select-Object -First 1)".Trim()
    $exe = Get-Item -LiteralPath "$Repo\out\build\win-amd64-release\mcla.exe" -ErrorAction SilentlyContinue
    Describe-State $Repo "$Step after the build"
    $secs = [int]((Get-Date) - $t).TotalSeconds
    $short = $target.Substring(0, 7)
    if ($code -ne 0 -or $facts.PchError) {
        $why = if ($facts.PchError) { "clang refused the old precompiled header ('has been modified since the precompiled header')" } else { "exit $code" }
        Result $(if ($MayFail) { "INFO" } else { "FAIL" }) $Step "$Label : the update to $Ref ($short) did not build: $why; $(Describe-Facts $facts); see $LogDir\$Step-build.out"
        return $false
    }
    if ($built -ne $target -or -not $exe -or ($Bundle.Sha -and $record -ne $Bundle.Sha)) {
        Result "FAIL" $Step "$Label : the build ended with exit 0, but built-commit '$built' (want $target), mcla.exe present $([bool]$exe), bundle record '$record' (want $($Bundle.Sha))"
        return $false
    }
    Result $(if ($MayFail) { "INFO" } else { "PASS" }) $Step "$Label : updated to $Ref ($short) and built in $secs s; $(Describe-Facts $facts); mcla.exe $($exe.LastWriteTime.ToString('s'))"
    return $true
}

# ---- checks before anything is written -------------------------------------------------------------------------------------------
$problems = New-Object System.Collections.ArrayList
$player = [IO.Path]::GetFullPath("$env:LOCALAPPDATA\Programs\MCLA").TrimEnd('\')
if ((Same-Or-Inside $R $player) -or (Same-Or-Inside $player $R)) { [void]$problems.Add("-Root $R is the player's install ($player) or holds it") }
if ((Same-Or-Inside $R $Game) -or (Same-Or-Inside $Game $R)) { [void]$problems.Add("-Root and -GameData must not hold each other ($R, $Game)") }
if ((Test-Path -LiteralPath $R) -and (Get-ChildItem -LiteralPath $R -Force | Select-Object -First 1)) { [void]$problems.Add("-Root $R is not empty: a clean run needs an empty or new folder") }
if (-not (Test-Path -LiteralPath "$Game\default.xex")) { [void]$problems.Add("-GameData $Game has no default.xex") }
foreach ($pattern in "llvm-mingw-*", "cmake-*", "ninja*", "mingit-*") {
    if (-not (Get-ChildItem -LiteralPath $Tools -Filter $pattern -ErrorAction SilentlyContinue)) { [void]$problems.Add("no $pattern in -ToolsDir $Tools") }
}
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { [void]$problems.Add("git.exe is not on the PATH") }
if ($EarlierTag -and (-not $EarlierBundle)) { [void]$problems.Add("-EarlierTag needs -EarlierBundle") }
$drive = Get-PSDrive -Name $R.Substring(0, 1) -ErrorAction SilentlyContinue
if ($drive -and $drive.Free -lt 10GB) { [void]$problems.Add("less than 10 GB free on $($drive.Name):") }
$prev = Resolve-Bundle "previous" $PreviousBundle $PreviousBundleSha256 $problems
$cand = Resolve-Bundle "candidate" $CandidateBundle $CandidateBundleSha256 $problems
$earlier = if ($EarlierTag -and $EarlierBundle) { Resolve-Bundle "earlier" $EarlierBundle $EarlierBundleSha256 $problems } else { $null }
if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host "FAIL preflight - $p" }
    exit 1
}

$preflight = "-Root is new or empty and not the player's install; the bundle zips match their hashes (previous $($prev.Sha.Substring(0, 12)), candidate $($cand.Sha.Substring(0, 12))); tools and game data are there"
if ($PreflightOnly) { Write-Host "PASS preflight - $preflight (-PreflightOnly: nothing was written)"; exit 0 }
New-Item -ItemType Directory -Force $LogDir | Out-Null
Result "PASS" "preflight" $preflight

# What -GameData holds before: it is linked into the installs and has to come out as it went in.
function Get-GameFingerprint {
    $files = @(Get-ChildItem -LiteralPath $Game -Recurse -File -Force)
    $sum = ($files | Measure-Object -Property Length -Sum).Sum
    $newest = ($files | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
    return "$($files.Count) files, $sum bytes, newest $($newest.ToString('s'))"
}
$gameBefore = Get-GameFingerprint
$updateRepo = $null
$stuckRepo = $null
try {
    # 1. the previous release's install, updated to the candidate
    $updateRepo = New-Install "update-base" "$R\update" $PreviousTag $prev
    if ($updateRepo) { [void](Update-Install "update" $updateRepo $Candidate $cand "$PreviousTag install" $false) }
    else { Note "update: skipped, the $PreviousTag install did not build" }

    # 2. an install the previous release's update may have left half-updated
    if ($earlier) {
        $stuckRepo = New-Install "stuck-base" "$R\stuck" $EarlierTag $earlier
        if ($stuckRepo) {
            $reached = Update-Install "stuck-previous" $stuckRepo $PreviousTag $prev "$EarlierTag install updated to $PreviousTag (may fail: that is the state under test)" $true
            if ($reached) { Note "stuck: the update to $PreviousTag built here, so this install is not stuck; the next step only tests the two updates in a row" }
            else { Note "stuck: the update to $PreviousTag did not build: the install is left as that update left it" }
            [void](Update-Install "stuck-update" $stuckRepo $Candidate $cand "$EarlierTag install after the update to $PreviousTag" $false)
        } else { Note "stuck: skipped, the $EarlierTag install did not build" }
    }
} finally {
    foreach ($d in "$R\update\mcla-recomp\game", "$R\stuck\mcla-recomp\game") { Remove-Junction $d }
}

$gameAfter = Get-GameFingerprint
if ($gameAfter -eq $gameBefore) { Result "PASS" "game data" "-GameData unchanged ($gameAfter)" }
else { Result "FAIL" "game data" "-GameData changed: before $gameBefore, after $gameAfter" }

Write-Host ""
if ($script:Failed -gt 0) { Write-Host "RESULT: FAIL ($script:Failed check(s)); logs in $LogDir" } else { Write-Host "RESULT: PASS; the installs are in $R, logs in $LogDir" }
if ($updateRepo -and $script:Failed -eq 0) {
    Write-Host "To boot the updated game (dev tools: Python, a GPU, the disc's game data; the real saves stay untouched):"
    Write-Host "  cd `"$updateRepo`""
    Write-Host "  `$env:MCLA_GAME_DATA = `"$Game`"; `$env:MCLA_DEV_ROOT = `"$R\dev`""
    Write-Host "  python -I launcher\dev\devrun.py run --script launcher\dev\scripts\boot-to-roam.pad --timeout 240 --cache-root `"$R\dev\cache`""
}
if ($script:Failed -gt 0) { exit 1 }
exit 0
