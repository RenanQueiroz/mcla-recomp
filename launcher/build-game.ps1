# Updates and builds the Midnight Club: Los Angeles recompilation (mcla-recomp).
# The launcher runs this, and it works the same by hand from a terminal:
#
#   .\build-game.ps1                build (or finish building) what is checked out
#   .\build-game.ps1 -Update        fast-forward to the latest upstream commit, then build
#   .\build-game.ps1 -ResetSdk      re-patch the ReXGlue SDK and rebuild it, then build
#   .\build-game.ps1 -InstallTools  let make_pc.ps1 install missing tools without asking
#   .\build-game.ps1 -Iso PATH      disc image, only needed until game\ is extracted;
#                                   by default the first *.iso next to mcla-recomp\ is used
#
# The real work is done by mcla-recomp\scripts\make_pc.ps1; this wrapper only
# covers what that script leaves to the user.
param([switch]$Update, [switch]$ResetSdk, [switch]$InstallTools, [string]$Iso = "")

$ErrorActionPreference = "Stop"
$base = Split-Path $PSScriptRoot -Parent
$repo = Join-Path $base "mcla-recomp"
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

# Our own changes to mcla-recomp live as patches in launcher\patches\, so that
# updates stay plain fast-forwards: they come off before an update and go back
# on before every build. One that no longer applies is skipped with a warning.
$localPatches = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "patches") -Filter *.patch -File -ErrorAction SilentlyContinue)
function Test-GitApply([string[]]$extra, [string]$patch, [string]$target = $repo) {
    $ErrorActionPreference = "Continue"
    & git.exe -C $target apply --check @extra $patch 2>$null
    $LASTEXITCODE -eq 0
}
function Remove-LocalPatches {
    foreach ($p in $localPatches) {
        if (Test-GitApply @("--reverse") $p.FullName) {
            Invoke-Git -C $repo apply --reverse $p.FullName
            "removed local patch $($p.Name) for the update"
        }
    }
}
function Add-LocalPatches {
    foreach ($p in $localPatches) {
        if (Test-GitApply @("--reverse") $p.FullName) {
            "local patch already applied: $($p.Name)"
        } elseif (Test-GitApply @() $p.FullName) {
            Invoke-Git -C $repo apply $p.FullName
            "applied local patch: $($p.Name)"
        } else {
            Write-Host "WARNING: local patch $($p.Name) no longer applies to mcla-recomp; building without it" -ForegroundColor Yellow
        }
    }
}

# Our changes to the ReXGlue SDK (launcher\patches\sdk\) go on top of the ones
# make_pc.ps1 applies. make_pc.ps1 rebuilds the SDK when its .mcla-patched
# marker is newer than the installed SDK, so applying one moves the marker on.
# Returns whether any patch was newly applied; status goes to the host only.
# Several SDK patches edit neighbouring lines of the same files, so once a later
# one is applied an earlier one no longer reverse-applies on its own; which
# patches are in is recorded in a stamp file in the SDK checkout instead (git
# clean on an SDK reset removes it along with the patches).
$sdkPatches = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "patches\sdk") -Filter *.patch -File -ErrorAction SilentlyContinue | Sort-Object Name)
$sdkPatchStamp = Join-Path $sdk ".launcher-sdk-patches"
function Add-SdkPatches {
    $applied = $false
    $done = @(if (Test-Path -LiteralPath $sdkPatchStamp) { Get-Content -LiteralPath $sdkPatchStamp })
    foreach ($p in $sdkPatches) {
        $id = "$($p.Name) $((Get-FileHash -LiteralPath $p.FullName -Algorithm SHA256).Hash)"
        if ($done -contains $id) {
            Write-Host "SDK patch already applied: $($p.Name)"
        } elseif (Test-GitApply @("--reverse") $p.FullName $sdk) {
            Write-Host "SDK patch already applied: $($p.Name)"
            Add-Content -LiteralPath $sdkPatchStamp $id
        } elseif (Test-GitApply @() $p.FullName $sdk) {
            Invoke-Git -C $sdk apply $p.FullName | Out-Null
            Add-Content -LiteralPath $sdkPatchStamp $id
            Write-Host "applied SDK patch: $($p.Name)"
            $applied = $true
        } else {
            Write-Host "WARNING: SDK patch $($p.Name) no longer applies to the ReXGlue SDK; building without it" -ForegroundColor Yellow
        }
    }
    return $applied
}
function Request-SdkRebuild {
    (Get-Item -LiteralPath "$sdk\.mcla-patched" -Force).LastWriteTime = Get-Date
}

try {
    if (-not (Test-Path "$repo\.git")) { throw "no mcla-recomp checkout at $repo" }

    # -----------------------------------------------------------------------
    if ($Update) {
        Step "update mcla-recomp"
        $old = Invoke-Git -C $repo rev-parse HEAD
        Invoke-Git -C $repo fetch --quiet origin
        $upstream = Invoke-Git -C $repo rev-parse "@{u}"
        if ($old -eq $upstream) {
            "already up to date at $($old.Substring(0, 7))"
        } else {
            # --autostash keeps other local edits (scripts\play_loop.ps1 writes
            # to config\runtime_discovered.toml) across the update.
            Remove-LocalPatches
            Invoke-Git -C $repo merge --ff-only --autostash "@{u}"
            Invoke-Git -C $repo log --oneline "$old..HEAD"
            # make_pc.ps1 patches the SDK only once (the .mcla-patched marker),
            # so new versions of the patches would otherwise never be applied.
            $changed = @(Invoke-Git -C $repo diff --name-only $old HEAD "--" "patches/*.patch")
            if ($changed) {
                "the SDK patches changed: $($changed -join ', ')"
                $ResetSdk = $true
            }
        }
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

    if ($localPatches) {
        Step "our local patches to mcla-recomp"
        Add-LocalPatches
    }

    $params = @{}
    if ($InstallTools) { $params.InstallTools = $true }
    if (-not (Test-Path "$repo\game\default.xex")) {
        if (-not $Iso) {
            $Iso = Get-ChildItem -LiteralPath $base -Filter *.iso -File | Select-Object -First 1 -ExpandProperty FullName
        }
        if ($Iso) { $params.Iso = $Iso; "disc image: $Iso" }
    }

    # On an SDK make_pc.ps1 has already patched, ours go on now, so one
    # make_pc.ps1 run rebuilds the SDK with them and then the game.
    $sdkPatchedBefore = $sdkPatches -and (Test-Path -LiteralPath "$sdk\.mcla-patched")
    if ($sdkPatchedBefore) {
        Step "our patches to the ReXGlue SDK"
        if (Add-SdkPatches) { Request-SdkRebuild }
    }

    $global:LASTEXITCODE = 0
    & "$repo\scripts\make_pc.ps1" @params
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # A fresh or reset SDK only got make_pc.ps1's own patches in the run above;
    # ours go on now, and the SDK and the game are built once more.
    if ($sdkPatches -and -not $sdkPatchedBefore) {
        Step "our patches to the ReXGlue SDK"
        if (Add-SdkPatches) {
            Request-SdkRebuild
            $global:LASTEXITCODE = 0
            & "$repo\scripts\make_pc.ps1" @params
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }

    # The launcher compares this with HEAD to tell a finished build from one
    # that failed or was interrupted after an update.
    Invoke-Git -C $repo rev-parse HEAD | Set-Content -LiteralPath $builtCommitFile -Encoding ASCII
    exit 0
} catch {
    Write-Host ""
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
