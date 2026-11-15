# Shared by scripts\make_pc.ps1, build.ps1, env.ps1, launcher\build-game.ps1 and
# launcher\build-launcher.ps1 (dot-sourced): the record of how this checkout was last built,
# versions that sort as versions, the compiler job limit, and the git settings.

# The options of the last build, kept in launcher\build-options.json by
# make_pc.ps1 when it finishes: what an update needs to build the same way
# (-Toolchain, -ToolsDir, -SdkBundle with its SHA-256, and the SDK folder that was
# used). None when there is no record, or a record that cannot be read. The file is
# UTF-8 (with a mark, as Save-BuildOptions writes it; one without reads the same, and the
# ASCII file of an older build too): an install folder's name may have any character.
function Get-BuildOptions([string]$root) {
    $file = Join-Path $root "launcher\build-options.json"
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    try {
        $text = [System.IO.File]::ReadAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($file), (New-Object System.Text.UTF8Encoding $false))
        if (-not $text -or -not $text.Trim()) { return $null }
        $o = $text | ConvertFrom-Json
    } catch { return $null }
    if (-not $o -or ($o.toolchain -ne "msvc" -and $o.toolchain -ne "mingw")) { return $null }
    [pscustomobject]@{
        Toolchain = [string]$o.toolchain; ToolsDir = [string]$o.toolsDir; SdkBundle = [string]$o.sdkBundle
        SdkBundleSha256 = [string]$o.sdkBundleSha256; SdkDir = [string]$o.sdkDir
    }
}

# Keeps the options of a finished build, but only a choice that was made: a
# checkout that never asked for anything but the defaults stays without a record
# (and so on msvc), and one that has a record has it brought up to date.
function Save-BuildOptions([string]$root, [string]$toolchain, [string]$toolsDir, [string]$sdkBundle, [string]$sdkBundleSha256, [string]$sdkDir) {
    $file = Join-Path $root "launcher\build-options.json"
    $chosen = $toolchain -ne "msvc" -or $toolsDir -or $sdkBundle -or $sdkBundleSha256
    if (-not $chosen -and -not (Test-Path -LiteralPath $file)) { return }
    $record = [ordered]@{
        toolchain = $toolchain; toolsDir = $toolsDir; sdkBundle = $sdkBundle
        sdkBundleSha256 = $sdkBundleSha256; sdkDir = $(if ($sdkBundle) { $sdkDir } else { "" })
    }
    New-Item -ItemType Directory -Force (Split-Path $file -Parent) | Out-Null
    [System.IO.File]::WriteAllText($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($file), (($record | ConvertTo-Json) + "`r`n"), (New-Object System.Text.UTF8Encoding $true))
}

# The version a tool is pinned to in scripts\ci\tools.json ("clang-msvc",
# "llvm-mingw"), which scripts\ci\check_pins.py keeps the same as the one
# make_pc.ps1 downloads. The folder of tools\ that make_pc.ps1 unpacks it into has
# that version in its name, and is the project's own compiler, whatever other
# release is in tools\. "" when the file cannot be read.
function Get-PinnedToolVersion([string]$root, [string]$tool) {
    try {
        $pins = Get-Content -LiteralPath (Join-Path $root "scripts\ci\tools.json") -Raw | ConvertFrom-Json
        [string]$pins.$tool.version
    } catch { "" }
}

# The version in a tool folder's name (cmake-4.4.4-windows-x86_64,
# llvm-mingw-20250709-ucrt-x86_64, clang+llvm-20.1.8-...), to sort 20 after 9.
function Get-VersionKey([string]$name) {
    if ($name -match '(\d+(?:\.\d+){0,3})') {
        $v = $Matches[1]
        if ($v -notmatch '\.') { $v += ".0" }
        try { return [version]$v } catch { }
    }
    [version]"0.0"
}
function Sort-ByVersion($items) {
    @($items) | Sort-Object -Descending -Property @{ Expression = { Get-VersionKey $_.Name } }, @{ Expression = { $_.Name } }
}

# Memory available to start programs with (free and what Windows gives back at
# once), in MB: GlobalMemoryStatusEx's figure through Visual Basic's
# ComputerInfo, which is already loaded in Windows PowerShell (17 ms; a CIM
# query for the same took half a second to five). The free figure of
# Win32_OperatingSystem is the fallback; 0 when nothing says.
# MCLA_AVAILABLE_MEMORY_MB pretends another figure (a test hook).
function Get-AvailableMemoryMb {
    if ($env:MCLA_AVAILABLE_MEMORY_MB -match '^\d+$') { return [double]$env:MCLA_AVAILABLE_MEMORY_MB }
    try {
        Add-Type -AssemblyName Microsoft.VisualBasic -ErrorAction Stop
        $bytes = (New-Object Microsoft.VisualBasic.Devices.ComputerInfo).AvailablePhysicalMemory
        if ($bytes -gt 0) { return [double]$bytes / 1MB }
    } catch { }
    try { return [double](Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).FreePhysicalMemory / 1024 } catch { }
    0
}

# The number of compiler jobs memory allows: one per 0.5 GB (a compiler takes up
# to 0.4 GB, the codegen 0.7 GB), but only when that is fewer than Ninja would
# start by itself (one per CPU thread, and two more); 0 when Ninja's own number
# is fine, so that a PC with enough memory builds exactly as before.
function Get-JobLimit([double]$availableMb, [int]$cpus) {
    if ($availableMb -le 0) { return 0 }
    $ninjaJobs = $cpus + 2
    $memoryJobs = [Math]::Max(1, [int][Math]::Floor($availableMb / 512))
    if ($memoryJobs -lt $ninjaJobs) { return $memoryJobs }
    0
}

# The git settings of every git this process starts, and of the programs those start (the SDK's clone and
# submodules, the patches, CMake's): git reads no setting of the PC. Not the system config (for MinGit, its own
# etc\gitconfig, which includes an installed Git for Windows' etc\gitconfig) and not the user's ~\.gitconfig:
# a certificate file that http.sslcainfo named there and that was gone (a half-removed Git for Windows) stopped
# the setup at "Getting the source" with "error adding trust anchors from file: C:/Program Files/Git/mingw64/etc/
# ssl/certs/ca-bundle.crt" (issues #32, #71). The environment is the only place changed, so nobody's git
# configuration is. What is set instead (the command-line scope, which no file overrides):
#   http.sslBackend=schannel   HTTPS is checked against Windows' own certificate store (MinGit's default); said out
#                              loud, git does not hand a certificate file (GIT_SSL_CAINFO, a repository's
#                              http.sslCAInfo) to it
#   safe.directory=*           the checkouts here are made by the launcher itself, and git refuses a repository
#                              whose folder it cannot show to be the user's (exFAT, another account, a network
#                              path: "detected dubious ownership", issue #64)
#   core.longpaths=true        the SDK has files deep in its submodules
# launcher\src\Setup\ChildProcess.cs (GitEnvironment) does the same for the git the launcher starts itself, with
# the first two: keep them alike (scripts\ci\check-git-env.ps1 compares them). GIT_CONFIG_GLOBAL needs git 2.32.
function Set-GitEnvironment {
    $settings = [ordered]@{ "http.sslBackend" = "schannel"; "safe.directory" = "*"; "core.longpaths" = "true" }
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_NOSYSTEM", "1", "Process")
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_GLOBAL", "NUL", "Process")   # NUL: no file (git maps it like /dev/null)
    $n = 0
    foreach ($key in $settings.Keys) {
        [Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$n", $key, "Process")
        [Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$n", [string]$settings[$key], "Process")
        $n++
    }
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_COUNT", [string]$n, "Process")
}
