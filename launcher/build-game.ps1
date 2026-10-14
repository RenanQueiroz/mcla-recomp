# Updates and builds the Midnight Club: Los Angeles recompilation from this
# fork (the repository this folder is in). The launcher runs this, and it works
# the same by hand from a terminal:
#
#   .\build-game.ps1                build (or finish building) what is checked out
#   .\build-game.ps1 -Update        fast-forward to the fork's latest commit, then build
#   .\build-game.ps1 -ResetSdk      re-patch the ReXGlue SDK and rebuild it, then build
#   .\build-game.ps1 -InstallTools  let make_pc.ps1 install missing tools without asking
#   .\build-game.ps1 -Iso PATH      disc image, only needed until game\ is extracted;
#                                   by default the first *.iso next to the repository
#
# The real work is done by scripts\make_pc.ps1; this wrapper only covers what
# that script leaves to the user.
param([switch]$Update, [switch]$ResetSdk, [switch]$InstallTools, [string]$Iso = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $repo "third_party\rexglue-sdk"
$builtCommitFile = Join-Path $PSScriptRoot "built-commit.txt"

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" }
# Git writes progress to stderr, which Windows PowerShell 5.1 turns into errors
# when redirected; leave it unredirected and judge by the exit code.
# (Not named "git": make_pc.ps1 runs in a child scope and would get it too.)
function Invoke-Git {
    & git.exe @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed (exit code $LASTEXITCODE)" }
}

# The fork follows holdmysocks/mcla-recomp only when asked to (FORK.md); this
# just says how far behind it is. Information only: a failure here is not one.
function Show-UpstreamLag {
    $ErrorActionPreference = "Continue"
    $remotes = @(& git.exe -C $repo remote)
    if ($remotes -notcontains "upstream") { return }
    & git.exe -C $repo fetch --quiet upstream 2>$null
    if ($LASTEXITCODE -ne 0) { "could not fetch upstream (holdmysocks/mcla-recomp)"; return }
    $count = & git.exe -C $repo rev-list --count "@{u}..upstream/main" 2>$null
    if ($LASTEXITCODE -eq 0 -and [int]$count -gt 0) {
        "upstream (holdmysocks/mcla-recomp) has $count commit(s) that are not in this fork yet"
    }
}

try {
    if (-not (Test-Path "$repo\.git")) { throw "no mcla-recomp checkout at $repo" }

    # -----------------------------------------------------------------------
    if ($Update) {
        Step "update from the fork"
        $old = Invoke-Git -C $repo rev-parse HEAD
        Invoke-Git -C $repo fetch --quiet origin
        $upstream = Invoke-Git -C $repo rev-parse "@{u}"
        if ($old -eq $upstream) {
            "already up to date at $($old.Substring(0, 7))"
        } else {
            # --autostash keeps local edits across the update: codegen rewrites
            # mcla_manifest.toml and generated\rexglue.cmake with LF line
            # endings, so they always look modified.
            Invoke-Git -C $repo merge --ff-only --autostash "@{u}"
            Invoke-Git -C $repo log --oneline "$old..HEAD"
            # make_pc.ps1 patches the SDK once and adds later patches by name,
            # so a patch that changed would otherwise never be applied again.
            $changed = @(Invoke-Git -C $repo diff --name-only $old HEAD "--" "patches/*.patch")
            if ($changed) {
                "the SDK patches changed: $($changed -join ', ')"
                $ResetSdk = $true
            }
        }
        Show-UpstreamLag
    }

    # -----------------------------------------------------------------------
    if ($ResetSdk -and (Test-Path "$sdk\.git")) {
        Step "reset the ReXGlue SDK so that make_pc.ps1 patches and builds it again"
        $expected = Select-String -Path "$repo\scripts\make_pc.ps1" -Pattern '^\s*\$sdkCommit\s*=\s*"([0-9a-f]{40})"' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
        $current = Invoke-Git -C $sdk rev-parse HEAD
        if ($expected -and $current -ne $expected) {
            # A different SDK version: make_pc.ps1 refuses an existing checkout
            # of another commit, so it has to clone it afresh.
            "the SDK version changed ($($current.Substring(0, 7)) -> $($expected.Substring(0, 7))): removing it for a new clone"
            Remove-Item -LiteralPath $sdk -Recurse -Force
        } else {
            # Undo the patches and the symlink copies in the SDK and its
            # submodules. Ignored files (the SDK's out\ build folder) stay, so
            # the SDK build that follows is incremental.
            Invoke-Git -C $sdk checkout "--" .
            Invoke-Git -C $sdk clean -fdq
            Invoke-Git -C $sdk submodule foreach --quiet --recursive "git checkout -- . && git clean -fdq"
            Remove-Item -LiteralPath "$sdk\.mcla-patched" -Force -ErrorAction SilentlyContinue
            "SDK reset to $($current.Substring(0, 7))"
        }
    }

    # -----------------------------------------------------------------------
    # make_pc.ps1 takes any python.exe under WindowsApps for the Microsoft
    # Store placeholder, but the Python install manager (Python 3.14 and later)
    # puts its real python.exe there too. If that one works, hand it to
    # make_pc.ps1 as a function, which Get-Command finds before any program.
    $python = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($python -and $python.Source -like "*WindowsApps*") {
        $ErrorActionPreference = "Continue"
        & $python.Source -c "import sys" 2>$null
        $ErrorActionPreference = "Stop"
        if ($LASTEXITCODE -eq 0) {
            $global:MclaPython = $python.Source
            function global:python { & $global:MclaPython @args }
        }
    }

    $params = @{}
    if ($InstallTools) { $params.InstallTools = $true }
    if (-not (Test-Path "$repo\game\default.xex")) {
        if (-not $Iso) {
            $Iso = Get-ChildItem -LiteralPath (Split-Path $repo -Parent) -Filter *.iso -File | Select-Object -First 1 -ExpandProperty FullName
        }
        if ($Iso) { $params.Iso = $Iso; "disc image: $Iso" }
    }

    $global:LASTEXITCODE = 0
    & "$repo\scripts\make_pc.ps1" @params
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # The launcher compares this with HEAD to tell a finished build from one
    # that failed or was interrupted after an update.
    Invoke-Git -C $repo rev-parse HEAD | Set-Content -LiteralPath $builtCommitFile -Encoding ASCII
    exit 0
} catch {
    Write-Host ""
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
