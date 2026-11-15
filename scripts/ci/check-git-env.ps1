# The git settings of this project, tried on a PC whose own git settings are broken (fork issues #32 and #71: the setup's
# own MinGit still read the system config of an installed, half-removed Git for Windows, whose http.sslcainfo named a
# certificate file that was gone, and "Getting the source" failed with "error adding trust anchors from file").
#
#   powershell -File scripts\ci\check-git-env.ps1 [-ToolsDir <folder with mingit-*>] [-Origin <repository>] [-NoNetwork]
#
# -ToolsDir   the launcher's tools folder: its newest mingit-*\cmd\git.exe is tried (what the setup runs); without it, the
#             first git.exe on the PATH
# -Origin     the repository the read-only network call asks (git ls-remote --heads, nothing is fetched or written);
#             default: the fork on GitHub
# -NoNetwork  leave that call out
#
# A scratch folder (under %TEMP%, removed at the end) holds the broken PC: a system config that includes another one that names
# a certificate file which does not exist (the way MinGit's etc\gitconfig includes Git for Windows' own), a user config and an XDG
# config that name it too, and a GIT_SSL_CAINFO. Then:
#   1  without Set-GitEnvironment (scripts\mcla-lib.ps1) git reads all four (the test PC is broken as meant), and ls-remote
#      of the repository fails (a note, not a verdict: a git that ignores the broken file proves less)
#   2  with it, `git config --show-scope --show-origin --list` has no system or global entry, only the command line's (and the
#      repository's own), with the three settings in it, and ls-remote of the repository works: HTTPS is still checked, by
#      Windows' certificate store
#   3  launcher\src\Setup\ChildProcess.cs (GitEnvironment) names the same two variables and the same settings
#   4  every script of the setup that runs git (scripts\*.ps1, launcher\*.ps1, this folder's check-update.ps1) calls
#      Set-GitEnvironment; the release-only scripts of this folder (build-fidelityfx.ps1, publish-launcher.ps1, run on a
#      runner of their own) and launcher\dev are not looked at
#
# Output: one "PASS <step> - ..." / "FAIL <step> - ..." / "NOTE <step> - ..." line each. Exit code 0 only when nothing failed.
param(
    [string]$ToolsDir = "",
    [string]$Origin = "https://github.com/RenanQueiroz/mcla-recomp.git",
    [switch]$NoNetwork
)

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$script:Failed = 0
function Result([string]$Verdict, [string]$Step, [string]$Text) {
    Write-Host "$Verdict $Step - $Text"
    if ($Verdict -eq "FAIL") { $script:Failed++ }
}
function Check([string]$Step, [bool]$Ok, [string]$Text) { Result $(if ($Ok) { "PASS" } else { "FAIL" }) $Step $Text }

# git and its output (stdout and stderr) as lines; the exit code is left in $script:GitExit.
$script:GitExit = 0
function Git-Run([string[]]$GitArgs) {
    $lines = @(& $script:Git @GitArgs 2>&1 | ForEach-Object { "$_" })
    $script:GitExit = $LASTEXITCODE
    return $lines
}

# Which git: the launcher's MinGit when -ToolsDir is given, else the one on the PATH.
$script:Git = $null
if ($ToolsDir) {
    $mingit = Get-ChildItem -LiteralPath $ToolsDir -Directory -Filter "mingit*" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "cmd\git.exe") } | Sort-Object Name -Descending | Select-Object -First 1
    if ($mingit) { $script:Git = Join-Path $mingit.FullName "cmd\git.exe" }
    else { Result "FAIL" "git" "no mingit*\cmd\git.exe in $ToolsDir"; exit 1 }
} else {
    $found = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($found) { $script:Git = $found.Source } else { Result "FAIL" "git" "no git.exe on the PATH (give -ToolsDir)"; exit 1 }
}
$version = (Git-Run @("--version")) -join " "
Write-Host "git: $script:Git ($version)"
if ($version -match 'version (\d+)\.(\d+)') {
    Check "git-version" (([int]$Matches[1] -gt 2) -or ([int]$Matches[1] -eq 2 -and [int]$Matches[2] -ge 32)) "$version (GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM need 2.32)"
    if ($script:Failed) { exit 1 }
} else { Result "FAIL" "git-version" "could not read '$version'"; exit 1 }

# What this script changes in the process's environment, put back at the end.
$touched = @("GIT_CONFIG_SYSTEM", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_COUNT", "GIT_SSL_CAINFO", "GIT_TERMINAL_PROMPT", "HOME", "XDG_CONFIG_HOME") +
    @(0..9 | ForEach-Object { "GIT_CONFIG_KEY_$_"; "GIT_CONFIG_VALUE_$_" })
$saved = @{}
foreach ($name in $touched) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, "Process") }

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("mcla-git-env-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
try {
    New-Item -ItemType Directory -Force -Path "$scratch\work", "$scratch\home", "$scratch\xdg\git" | Out-Null
    $gone = ("$scratch\gone\ca-bundle.crt") -replace '\\', '/'
    $asciiLines = { param($path, $lines) [IO.File]::WriteAllText($path, ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding $false)) }
    # The installed Git for Windows' own config, and the system config of the git under test that includes it.
    & $asciiLines "$scratch\fake-gfw-gitconfig" @("[http]", "`tsslCAInfo = $gone", "[user]", "`tname = fake-system")
    & $asciiLines "$scratch\fake-system-gitconfig" @("[include]", "`tpath = $(($scratch + '\fake-gfw-gitconfig') -replace '\\', '/')")
    & $asciiLines "$scratch\home\.gitconfig" @("[http]", "`tsslCAInfo = $gone", "[user]", "`temail = fake-home")
    & $asciiLines "$scratch\xdg\git\config" @("[http]", "`tsslCAInfo = $gone", "[user]", "`tsigningkey = fake-xdg")

    [Environment]::SetEnvironmentVariable("GIT_CONFIG_SYSTEM", "$scratch\fake-system-gitconfig", "Process")
    [Environment]::SetEnvironmentVariable("HOME", "$scratch\home", "Process")
    [Environment]::SetEnvironmentVariable("XDG_CONFIG_HOME", "$scratch\xdg", "Process")
    [Environment]::SetEnvironmentVariable("GIT_SSL_CAINFO", $gone, "Process")
    [Environment]::SetEnvironmentVariable("GIT_TERMINAL_PROMPT", "0", "Process")
    foreach ($name in "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_COUNT") { [Environment]::SetEnvironmentVariable($name, $null, "Process") }
    Push-Location "$scratch\work"
    [void](Git-Run @("init", "-q"))

    # 1. The broken PC, without the settings.
    $before = Git-Run @("config", "--show-scope", "--show-origin", "--list")
    $beforeText = $before -join "`n"
    Check "broken-pc" ($beforeText -match 'fake-system' -and $beforeText -match 'fake-home' -and $beforeText -match 'fake-xdg' -and $beforeText -match 'sslcainfo') `
        "without Set-GitEnvironment git reads the fake system config (through an include), the user config and the XDG config, all naming a missing certificate file"
    if (-not $NoNetwork) {
        $control = Git-Run @("ls-remote", "--heads", $Origin, "main")
        if ($script:GitExit -ne 0) { Result "PASS" "broken-pc-network" "without Set-GitEnvironment git ls-remote fails, as it did for the players: $($control | Select-Object -Last 1)" }
        else { Result "NOTE" "broken-pc-network" "git ls-remote worked on the broken PC too (this git ignores the missing certificate file): the call below proves less" }
    }

    # 2. With the settings.
    . "$PSScriptRoot\..\mcla-lib.ps1"
    if (-not (Get-Command Set-GitEnvironment -ErrorAction SilentlyContinue)) { throw "scripts\mcla-lib.ps1 has no Set-GitEnvironment" }
    Set-GitEnvironment
    $after = Git-Run @("config", "--show-scope", "--show-origin", "--list")
    $foreign = @($after | Where-Object { $_ -match '^(system|global)\s' -or $_ -match 'fake-(system|home|xdg)' })
    Check "no-system-no-global" ($script:GitExit -eq 0 -and $foreign.Count -eq 0) $(if ($foreign.Count -gt 0) { "still read: " + ($foreign -join " | ") } else { "git config --show-scope --show-origin --list lists $($after.Count) settings, none of the system or global scope (the fake ones are not read)" })
    $commandLine = @($after | Where-Object { $_ -match '^command\s' })
    Check "settings-given" ($commandLine.Count -eq 3 -and ($commandLine -join "`n") -match 'http\.sslbackend=schannel' -and ($commandLine -join "`n") -match 'safe\.directory=\*' -and ($commandLine -join "`n") -match 'core\.longpaths=true') `
        "the command line scope has: $(($commandLine | ForEach-Object { ($_ -split "`t")[-1] }) -join ', ')"
    $backend = (Git-Run @("config", "--get", "http.sslBackend")) -join ""
    Check "https-by-windows" ($backend -eq "schannel") "http.sslBackend is '$backend': HTTPS is checked against Windows' certificate store, which a certificate file named by GIT_SSL_CAINFO or a config does not replace"
    if (-not $NoNetwork) {
        $remote = Git-Run @("ls-remote", "--heads", $Origin, "main")
        Check "ls-remote" ($script:GitExit -eq 0 -and ($remote -join "`n") -match '(?m)^[0-9a-f]{40}\s+refs/heads/main$') `
            "git ls-remote --heads of $Origin on the broken PC: $(($remote | Select-Object -Last 1))"
    } else { Result "NOTE" "ls-remote" "left out (-NoNetwork)" }
    Pop-Location

    # 3. The launcher's own list.
    $cs = Join-Path $repoRoot "launcher\src\Setup\ChildProcess.cs"
    $csText = if (Test-Path -LiteralPath $cs) { [IO.File]::ReadAllText($cs) } else { "" }
    $csSettings = @()
    if ($csText -match '(?s)Settings\s*=\s*\[(.*?)\];') { $csSettings = @([regex]::Matches($Matches[1], '\("([^"]+)",\s*"([^"]*)"\)') | ForEach-Object { "$($_.Groups[1].Value)=$($_.Groups[2].Value)" }) }
    $psSettings = @(0..9 | Where-Object { [Environment]::GetEnvironmentVariable("GIT_CONFIG_KEY_$_", "Process") } |
        ForEach-Object { "$([Environment]::GetEnvironmentVariable("GIT_CONFIG_KEY_$_", 'Process'))=$([Environment]::GetEnvironmentVariable("GIT_CONFIG_VALUE_$_", 'Process'))" })
    $missing = @($csSettings | Where-Object { $psSettings -notcontains $_ })
    $noFile = if ($csText -match 'const string NoFile = "([^"]+)"') { $Matches[1] } else { "" }
    Check "launcher-alike" ($csSettings.Count -ge 2 -and $missing.Count -eq 0 -and $noFile -eq [Environment]::GetEnvironmentVariable("GIT_CONFIG_GLOBAL", "Process") -and
                            $csText -match '"GIT_CONFIG_NOSYSTEM"\]\s*=\s*"1"' -and $csText -match '"GIT_CONFIG_GLOBAL"\]\s*=\s*NoFile') `
        "the launcher's GitEnvironment gives $($csSettings -join ', ') and GIT_CONFIG_GLOBAL=$noFile, Set-GitEnvironment gives $($psSettings -join ', ')$(if ($missing) { '; the script lacks ' + ($missing -join ', ') })"

    # 4. Every script of the setup that runs git calls the helper.
    $files = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot "scripts") -Filter *.ps1 -File) + @(Get-ChildItem -LiteralPath (Join-Path $repoRoot "launcher") -Filter *.ps1 -File) +
             @(Get-Item -LiteralPath (Join-Path $repoRoot "scripts\ci\check-update.ps1"))
    $call = '(?i)(&\s*git(\.exe)?\s|(^|[\s;{(|])git(\.exe)?\s+(-C|-c|clone|fetch|apply|submodule|checkout|merge|rev-parse|ls-files|log|diff|remote|config|status|pull|reset|clean)\b|\bInvoke-Git\b)'
    $callers = @(); $without = @()
    foreach ($f in $files) {
        $code = @([IO.File]::ReadAllLines($f.FullName) | Where-Object { $_.TrimStart() -notmatch '^#' })
        if (@($code | Where-Object { $_ -match $call }).Count -eq 0) { continue }
        $callers += $f.Name
        if (@($code | Where-Object { $_ -match '\bSet-GitEnvironment\b' }).Count -eq 0) { $without += $f.Name }
    }
    $expected = @("make_pc.ps1", "build-game.ps1", "build-launcher.ps1", "check-update.ps1")
    $notSeen = @($expected | Where-Object { $callers -notcontains $_ })
    Check "scripts-use-the-helper" ($without.Count -eq 0 -and $notSeen.Count -eq 0) $(
        if ($without.Count -gt 0) { "these run git without Set-GitEnvironment: $($without -join ', ')" }
        elseif ($notSeen.Count -gt 0) { "the scan did not find the git calls of $($notSeen -join ', ') (is the pattern out of date?)" }
        else { "$($callers -join ', ') run git, and each calls Set-GitEnvironment" })
}
catch {
    Result "FAIL" "self-test" "stopped: $($_.Exception.Message)"
}
finally {
    if ((Get-Location).Path -like "$scratch*") { Set-Location -LiteralPath $repoRoot }
    foreach ($name in $touched) { [Environment]::SetEnvironmentVariable($name, $saved[$name], "Process") }
    if ((Test-Path -LiteralPath $scratch) -and $scratch -like "*mcla-git-env-*") { Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($script:Failed -gt 0) { Write-Host "$script:Failed check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
