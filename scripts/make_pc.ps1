# One command from your own disc image to the game running on Windows.
#
#   .\scripts\make_pc.ps1 -Iso C:\path\to\your.iso
#
# Needs Git, CMake, Ninja, Python 3, Visual Studio Build Tools 2022 version
# 17.1 or newer (C++ workload), a Windows SDK newer than 10.0.19041 and
# Clang 20. Whatever is missing, the script offers to install
# (winget, and Clang from the LLVM project's releases); README.md
# lists them for doing it by hand. With -Toolchain mingw it needs no Microsoft
# compiler: Clang, lld, libc++ and the mingw-w64 headers come in one download
# (llvm-mingw), which needs no administrator rights.
# Everything is built on your machine from your own copy of the game: nothing
# of the game is in this repository, and what this builds is not for sharing.
#
# Each step is skipped when its result is already there, so after a failure,
# or after `git pull`, run the same command again and it continues.
#
#   -Iso PATH    your disc image (Midnight Club: Los Angeles Complete Edition,
#                USA/Europe, Xbox 360). Not needed once game\ is extracted.
#   -Play        start the game when the build is done
#   -InstallTools  install missing tools without asking first
#   -Preset      CMake preset of the game build (default win-amd64-release)
#   -Toolchain   msvc (default): the project's Clang 20.1.8 with Visual Studio
#                Build Tools' headers and libraries. mingw: llvm-mingw 20250709
#                (Clang 20.1.8, lld, libc++, mingw-w64 and Windows' own UCRT),
#                downloaded into tools\ and checked against a pinned SHA-256.
#                Each toolchain keeps its own SDK build (the first build with
#                the other one builds a second SDK; after that, switching
#                rebuilds only the game). A game built with mingw needs
#                libc++.dll and libunwind.dll next to mcla.exe (copied), not the
#                Visual C++ runtime (FSR's DLLs, built with MSVC, have the C
#                runtime linked in and need none either); its debug
#                information is split into mcla.exe.debug.
#   -ToolsDir    a folder with portable tools to use instead of installing or
#                downloading them: llvm-mingw-*\ (mingw) or clang+llvm-*\
#                (msvc), cmake*\, ninja*\ (or ninja.exe) and mingit*\ (git) in
#                it; it has to exist. A tool not found there is looked for as usual. Nothing
#                is added to the PATH outside this process.
#   -SdkBundle   a prebuilt ReXGlue SDK instead of cloning, patching and building
#                the SDK: a folder (used where it is, and trusted as it is), or a
#                .zip, a path or an https URL; a URL needs -SdkBundleSha256, and
#                http is refused. A .zip is checked against the SHA-256 when one
#                is given and unpacked into third_party\rexglue-sdk-bin. Either
#                way its layout, the toolchain it was built with, and the SDK
#                commit and patches it was built from (mcla-sdk-bundle.json,
#                which this script writes into the SDK it builds) have to be
#                this checkout's.
#
#   -Until       stop after a step, for CI, which has no game: tools (the tools
#                found or installed), sdk-source (and the SDK cloned and
#                patched) or sdk (and built and installed, or the prebuilt one
#                unpacked and checked). The game's steps (extracting the disc
#                image, codegen, the build) are not run.
#
# What a finished build was asked for (-Toolchain, -ToolsDir, -SdkBundle, its
# SHA-256) is kept in launcher\build-options.json when it is not just the
# defaults, and launcher\build-game.ps1 (so an update, also the launcher's)
# builds with the same options when it is given none, and so does this script when
# it is given none of -Toolchain, -ToolsDir, -SdkBundle and -SdkBundleSha256: a
# record written by hand ({"toolchain": "mingw"}) switches a checkout at its next
# build, by whichever build-game.ps1 starts it.
#
# On a PC with little free memory the number of compiler jobs is capped (about
# one per 0.5 GB), so that it does not page; set CMAKE_BUILD_PARALLEL_LEVEL to
# choose it yourself.
param([string]$Iso = "", [switch]$Play, [switch]$InstallTools, [string]$Preset = "win-amd64-release",
      [ValidateSet("msvc", "mingw")][string]$Toolchain = "msvc", [string]$ToolsDir = "",
      [string]$SdkBundle = "", [string]$SdkBundleSha256 = "",
      [ValidateSet("", "tools", "sdk-source", "sdk")][string]$Until = "")

$ErrorActionPreference = "Stop"
# The SDK has files deep in its submodules: git on Windows needs core.longpaths
# for them. Set for every git this script starts, and the ones those start,
# without touching anyone's git configuration.
$env:GIT_CONFIG_COUNT = "1"; $env:GIT_CONFIG_KEY_0 = "core.longpaths"; $env:GIT_CONFIG_VALUE_0 = "true"
$root = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\mcla-lib.ps1"
# Given none of the options that say how to build (-Toolchain, -ToolsDir, -SdkBundle,
# -SdkBundleSha256), the last build's are used (launcher\build-options.json), as
# build-game.ps1 and env.ps1 do: a checkout built with mingw goes on with mingw however
# this is started, by a build-game.ps1 that passes none too (an older one, as the
# launcher of an install not updated yet runs). So a record written by hand
# ({"toolchain": "mingw"}) is all an update needs to build with mingw.
if (-not ($PSBoundParameters.ContainsKey("Toolchain") -or $PSBoundParameters.ContainsKey("ToolsDir") -or
          $PSBoundParameters.ContainsKey("SdkBundle") -or $PSBoundParameters.ContainsKey("SdkBundleSha256"))) {
    $recorded = Get-BuildOptions $root
    if ($recorded) {
        $Toolchain = $recorded.Toolchain; $ToolsDir = $recorded.ToolsDir
        $SdkBundle = $recorded.SdkBundle; $SdkBundleSha256 = $recorded.SdkBundleSha256
        if ($Toolchain -ne "msvc" -or $ToolsDir -or $SdkBundle) {
            "build options of the last build: -Toolchain $Toolchain$(if ($ToolsDir) { ' -ToolsDir ' + $ToolsDir })$(if ($SdkBundle) { ' -SdkBundle ' + $SdkBundle })"
        }
    }
}
$sdk = "$root\third_party\rexglue-sdk"
$sdkTag = "v0.10.0"
$sdkCommit = "f5337cdc947ff6d4c4196737e2c807a48f2a1fc2"
$mingw = $Toolchain -eq "mingw"
# Each toolchain has its own SDK build and install, so that the other one's
# stay as they are (msvc: the folders earlier versions of this script used).
$sdkBuildName = if ($mingw) { "win-amd64-mingw" } else { "win-amd64" }
$install = "$sdk\out\install\$sdkBuildName"
$bundleDir = "$root\third_party\rexglue-sdk-bin"
$logs = "$root\out\logs"
New-Item -ItemType Directory -Force $logs | Out-Null

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" }
function Fail([string]$text) { Write-Host ""; Write-Host "FAILED: $text" -ForegroundColor Red; exit 1 }

# Paths given on the command line are taken from where the command was run, as
# absolute paths (the script works in the repository's folder), and have to be
# there: a -ToolsDir that is not there is not "no tools found". A path with [ ]
# in it is a path: -LiteralPath for every one.
if ($ToolsDir) {
    if (-not (Test-Path -LiteralPath $ToolsDir -PathType Container)) { Fail "-ToolsDir: no such folder: $ToolsDir" }
    $ToolsDir = (Resolve-Path -LiteralPath $ToolsDir).ProviderPath
}
$sdkBundleIsUrl = $SdkBundle -match '^[A-Za-z][A-Za-z0-9+.-]*://'
if ($SdkBundle -and -not $sdkBundleIsUrl) {
    if (-not (Test-Path -LiteralPath $SdkBundle)) { Fail "no such SDK bundle: $SdkBundle (a folder, a .zip, or an https URL of one)" }
    $SdkBundle = (Resolve-Path -LiteralPath $SdkBundle).ProviderPath
}
if ($Iso -and (Test-Path -LiteralPath $Iso)) { $Iso = (Resolve-Path -LiteralPath $Iso).ProviderPath }
if ($SdkBundleSha256 -and -not $SdkBundle) { Fail "-SdkBundleSha256 is for -SdkBundle" }
if ($SdkBundleSha256 -and $SdkBundleSha256.Trim() -notmatch '^[0-9a-fA-F]{64}$') { Fail "-SdkBundleSha256 is not a SHA-256 (64 hexadecimal digits)" }
# Run a command with its output in a log; on failure show the end of the log.
function Logged([string]$name, [scriptblock]$command) {
    $log = "$logs\$name.log"
    # Windows PowerShell 5.1 turns a program's redirected stderr into errors,
    # which "Stop" would end the script on; the exit code is what counts.
    $ErrorActionPreference = "Continue"
    & $command 2>&1 | ForEach-Object { "$_" } > $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $log -Tail 25 | ForEach-Object { Write-Host $_ }
        Fail "$name (full log: $log)"
    }
}

# Apply a patch unless it is already in: a run that stopped after this patch
# and before the end of step 2 leaves it applied, and it does not apply twice.
function Patch([string]$name, [string]$repo, [string]$patch) {
    $ErrorActionPreference = "Continue"
    git -C $repo apply --reverse --check $patch 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { "already applied: $(Split-Path $patch -Leaf)" }
    else { Logged $name { git -C $repo apply $patch } }
    RecordPatch $patch
}

# A patch revised later still adds the name the SDK is checked for, so the
# SHA-256 of every patch applied goes into the SDK's .mcla-patches, and a patch
# that is in without its current hash there stops the build: the SDK has to
# be patched afresh, as reverting an older version can fail where later
# patches change the same lines.
function PatchHash([string]$patch) { (Get-FileHash -Algorithm SHA256 -LiteralPath $patch).Hash.ToLowerInvariant() }
function RecordPatch([string]$patch) {
    $leaf = Split-Path $patch -Leaf
    $lines = @()
    if (Test-Path -LiteralPath "$sdk\.mcla-patches") {
        $lines = @(Get-Content -LiteralPath "$sdk\.mcla-patches" | Where-Object { -not $_.EndsWith(" $leaf") })
    }
    $lines += "$(PatchHash $patch) $leaf"
    Set-Content -Force -Encoding Ascii "$sdk\.mcla-patches" $lines
}
function CheckPatch([string]$patch) {
    $leaf = Split-Path $patch -Leaf
    $record = $null
    if (Test-Path -LiteralPath "$sdk\.mcla-patches") {
        $record = Get-Content -LiteralPath "$sdk\.mcla-patches" | Where-Object { $_.EndsWith(" $leaf") } | Select-Object -First 1
    }
    $again = "Patch the SDK afresh: launcher\build-game.ps1 -ResetSdk, or delete third_party\rexglue-sdk, and run this again."
    if (-not $record) {
        Fail "$leaf is in the SDK without a record in third_party\rexglue-sdk\.mcla-patches: it was patched by an older make_pc.ps1 and may hold an older version of it. $again"
    }
    if ($record.Split(" ")[0] -ne (PatchHash $patch)) {
        Fail "patches\$leaf has changed since it was applied to the SDK. $again"
    }
}

# Remove a folder and stop if it is still there (a program has a file in it
# open, or a path in it is too long for PowerShell 5.1): a new folder moved over
# a leftover one would end up inside it, and the script would go on with the old.
function RemoveFolder([string]$path) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $path) {
            Fail "could not remove $path (a program may have a file in it open, or a path in it is too long for PowerShell): close it, delete the folder by hand, and run this again. Nothing else was changed."
        }
    }
}

# Unpack a .zip into a new, empty folder (an old one is removed). tar.exe
# (Windows 10 and later) is quick; .NET if it cannot. The folders it is given are
# short ones, as the archives hold paths of over 100 characters.
function UnzipTo([string]$name, [string]$archive, [string]$dest) {
    RemoveFolder $dest
    New-Item -ItemType Directory -Force $dest | Out-Null
    Logged $name {
        tar.exe -xf $archive -C $dest
        if ($LASTEXITCODE -ne 0) {
            "tar.exe could not unpack the archive; unpacking it with .NET instead"
            Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction SilentlyContinue
            try {
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                [IO.Compression.ZipFile]::ExtractToDirectory($archive, $dest)
                $global:LASTEXITCODE = 0
            } catch { "$_"; $global:LASTEXITCODE = 1 }
        }
    }
}

# The runtime a program built with llvm-mingw loads: libc++ and libunwind, in
# place of the Visual C++ runtime (Apache-2.0 with the LLVM exception; no
# notice is needed with a binary), from the toolchain or from $from. Beside
# rexglue.exe and the SDK's DLLs, and beside mcla.exe. -OnlyIfMissing leaves one
# that is there (a bundle's own: they belong to the SDK it was built with).
function CopyMingwRuntime([string]$dest, [string]$from = "", [switch]$OnlyIfMissing) {
    if (-not $from) { $from = "$env:MCLA_MINGW_ROOT\x86_64-w64-mingw32\bin" }
    foreach ($dll in "libc++.dll", "libunwind.dll") {
        if (-not (Test-Path -LiteralPath "$from\$dll")) { Fail "$from\$dll is missing: there is no x86_64 UCRT runtime there" }
        $target = Get-Item -LiteralPath "$dest\$dll" -ErrorAction SilentlyContinue
        $same = $target -and $target.Length -eq (Get-Item -LiteralPath "$from\$dll").Length
        if (-not $target -or (-not $OnlyIfMissing -and -not $same)) { Copy-Item -LiteralPath "$from\$dll" -Destination $dest -Force }
    }
}

# What is wrong with a prebuilt SDK folder (empty when nothing): the files the
# game's build and the launcher need, and that it was built with the toolchain
# asked for (libraries of the one cannot be linked by the other: libc++ against
# the Microsoft STL).
function SdkLayoutProblem([string]$dir) {
    $need = "bin\rexglue.exe", "bin\rexruntime.dll", "bin\rexgpu-xenos.dll", "lib\cmake\rexglue\rexglueConfig.cmake", "include\rex"
    $missing = @($need | Where-Object { -not (Test-Path -LiteralPath "$dir\$_") })
    if ($missing) { return "it lacks $($missing -join ', ')" }
    $bundleMingw = Test-Path -LiteralPath "$dir\lib\rexruntime.dll.a"
    $bundleMsvc = Test-Path -LiteralPath "$dir\lib\rexruntime.lib"
    if (-not $bundleMingw -and -not $bundleMsvc) { return "it has no import library for rexruntime.dll (lib\rexruntime.dll.a or lib\rexruntime.lib)" }
    if ($mingw -and -not $bundleMingw) { return "it was built with the msvc toolchain, and this is -Toolchain mingw" }
    if (-not $mingw -and $bundleMingw) { return "it was built with the mingw toolchain: use -Toolchain mingw" }
    ""
}

# What an SDK was built from, written into its install folder (and so into a
# bundle made from it): the SDK commit, the toolchain, and the SHA-256 of every
# patch applied (as .mcla-patches records them). A prebuilt SDK is used only when
# it says the same as this checkout, so that a bundle made before a pull is not
# built against.
$bundleManifestName = "mcla-sdk-bundle.json"
function ExpectedPatchNames {
    @("rexglue-v0.10.0-mcla.patch", "rexglue-ffmpeg-ps5-config.patch", "rexglue-minmax-blend-factor.patch") +
        @($laterPatches | ForEach-Object { $_.Patch })
}
function WriteBundleManifest([string]$dir) {
    $patches = @()
    foreach ($line in (Get-Content -LiteralPath "$sdk\.mcla-patches")) {
        if ($line -match '^([0-9a-f]{64}) (\S+)$') { $patches += [ordered]@{ name = $Matches[2]; sha256 = $Matches[1] } }
    }
    # Sorted by name, ordinally (Sort-Object follows the culture, which ignores hyphens): a
    # consumer that derives a hash of the patches from this list (scripts\ci\make-sdk-bundle.py
    # does, the same way) gets the same order on any PC.
    $names = New-Object 'System.Collections.Generic.List[string]'
    foreach ($p in $patches) { $names.Add([string]$p["name"]) }
    $names.Sort([System.StringComparer]::Ordinal)
    $byName = @{}
    foreach ($p in $patches) { $byName[[string]$p["name"]] = $p }
    $manifest = [ordered]@{
        format = 1; sdkCommit = $sdkCommit; toolchain = $Toolchain
        patches = @($names | ForEach-Object { $byName[$_] })
    }
    Set-Content -LiteralPath "$dir\$bundleManifestName" -Encoding Ascii (($manifest | ConvertTo-Json -Depth 4) + "`r`n")
}
function BundleManifestProblem([string]$dir) {
    $file = "$dir\$bundleManifestName"
    if (-not (Test-Path -LiteralPath $file)) {
        return "it has no $bundleManifestName (the SDK that make_pc.ps1 builds has one, and so has a release's bundle)"
    }
    try { $m = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json } catch { return "its $bundleManifestName cannot be read" }
    if ($m.format -ne 1) { return "its $bundleManifestName is of another format ($($m.format))" }
    if ($m.sdkCommit -ne $sdkCommit) { return "it was built from SDK commit $($m.sdkCommit), and this checkout's is $sdkCommit" }
    if ($m.toolchain -ne $Toolchain) { return "its manifest says the $($m.toolchain) toolchain, and this is -Toolchain $Toolchain" }
    $theirs = @{}
    foreach ($p in @($m.patches)) { $theirs[[string]$p.name] = [string]$p.sha256 }
    $expectedNames = @(ExpectedPatchNames)
    $differ = @()
    foreach ($name in $expectedNames) {
        $patchFile = "$root\patches\$name"
        if (-not $theirs.ContainsKey($name)) { $differ += "$name (not in it)" }
        elseif (-not (Test-Path -LiteralPath $patchFile) -or $theirs[$name] -ne (PatchHash $patchFile)) { $differ += "$name (another version)" }
    }
    foreach ($name in $theirs.Keys) { if ($expectedNames -notcontains $name) { $differ += "$name (this checkout does not apply it)" } }
    if ($differ) {
        $more = if ($differ.Count -gt 4) { ", and $($differ.Count - 4) more" } else { "" }
        return "it was built from other patches than this checkout's: $((@($differ) | Select-Object -First 4) -join ', ')$more"
    }
    ""
}

# -SdkBundle: a prebuilt SDK instead of cloning, patching and building one. A
# folder is used where it is (nothing is written into it). A .zip (a path, or an
# https URL, which needs its SHA-256 and is downloaded into out\downloads and
# removed after) is unpacked into third_party\rexglue-sdk-bin, and again only when
# it is another one: the SHA-256 of what is unpacked is kept in .mcla-bundle, so a
# run with -SdkBundleSha256 does not even download it twice. The zip holds the
# install tree (bin, lib, include, share), directly or in a single folder. What is
# in place is looked at at every run (layout, manifest) against this checkout, so
# that "unpacked already" never means "still the right one". Sets $install.
function UseSdkBundle {
    $expected = $SdkBundleSha256.Trim().ToLowerInvariant()
    $record = "$bundleDir\.mcla-bundle"
    $unpacked = ""
    if (Test-Path -LiteralPath $record) {
        $first = Get-Content -LiteralPath $record -TotalCount 1
        if ($first) { $unpacked = $first.Trim() }
    }
    if (-not $sdkBundleIsUrl -and (Test-Path -LiteralPath $SdkBundle -PathType Container)) {
        $dir = $SdkBundle
        "SDK bundle: the folder $dir, used where it is (nothing is written into it; -SdkBundleSha256 is for a .zip)"
    } else {
        if ($sdkBundleIsUrl) {
            if ($SdkBundle -notmatch '^https://') { Fail "-SdkBundle takes https URLs only, not $SdkBundle" }
            if (-not $expected) {
                Fail "a -SdkBundle URL needs -SdkBundleSha256: what is downloaded is run (rexglue.exe) and put beside the game, so it is checked against the hash the release names."
            }
        }
        $zip = $SdkBundle
        $downloaded = $false
        if ($sdkBundleIsUrl -and $unpacked -eq $expected -and (Test-Path -LiteralPath "$bundleDir\bin\rexglue.exe")) {
            "SDK bundle $expected is unpacked already"
        } else {
            if ($sdkBundleIsUrl) {
                New-Item -ItemType Directory -Force "$root\out\downloads" | Out-Null
                # A file name made of the URL's last part, with nothing in it that is a path.
                $leaf = (($SdkBundle -split '[?#]')[0].Split('/')[-1]) -replace '[^A-Za-z0-9._-]', '_'
                if (-not $leaf -or $leaf -match '^\.+$') { $leaf = "sdk-bundle.zip" }
                $zip = "$root\out\downloads\$leaf"
                $downloaded = $true
                "downloading the SDK bundle"
                Logged "sdk-bundle-download" { curl.exe -L --fail --proto "=https" --proto-redir "=https" -o $zip $SdkBundle }
            }
            $dropZip = { if ($downloaded) { Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue } }
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
            if ($expected -and $actual -ne $expected) {
                & $dropZip
                Fail "the SDK bundle's SHA-256 is $actual, not the $expected given with -SdkBundleSha256; nothing was unpacked."
            }
            if ($actual -eq $unpacked -and (Test-Path -LiteralPath "$bundleDir\bin\rexglue.exe")) {
                "SDK bundle $actual is unpacked already"
            } else {
                "unpacking the SDK bundle into $bundleDir"
                $tmp = "$root\third_party\.u"
                New-Item -ItemType Directory -Force "$root\third_party" | Out-Null
                UnzipTo "sdk-bundle-unpack" $zip $tmp
                $top = if (Test-Path -LiteralPath "$tmp\bin\rexglue.exe") { $tmp } else {
                    $inner = @(Get-ChildItem -LiteralPath $tmp -Directory | Where-Object { Test-Path -LiteralPath "$($_.FullName)\bin\rexglue.exe" })
                    if ($inner.Count -eq 1) { $inner[0].FullName } else { $null }
                }
                if (-not $top) {
                    & $dropZip; Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
                    Fail "the SDK bundle holds no bin\rexglue.exe (the zip is the install tree: bin, lib, include, share)"
                }
                # Looked at before it replaces the one in place.
                $problem = SdkLayoutProblem $top
                if (-not $problem) { $problem = BundleManifestProblem $top }
                if ($problem) {
                    & $dropZip; Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
                    Fail "the SDK bundle cannot be used: $problem (nothing was changed)."
                }
                RemoveFolder $bundleDir
                Move-Item -LiteralPath $top -Destination $bundleDir
                Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
                Set-Content -LiteralPath $record -Encoding Ascii $actual
            }
            & $dropZip
        }
        $dir = $bundleDir
    }
    # Whatever is in place, now: the layout, and that it is this checkout's SDK.
    $problem = SdkLayoutProblem $dir
    if ($problem) { Fail "the SDK bundle at $dir cannot be used: $problem." }
    $problem = BundleManifestProblem $dir
    if ($problem) {
        Fail "the SDK bundle at $dir is not this checkout's SDK: $problem. Use the bundle of the release this checkout is at, or build the SDK here (run without -SdkBundle)."
    }
    # The unpacked one is ours; a folder given is not written into.
    if ($mingw -and $dir -eq $bundleDir) { CopyMingwRuntime "$dir\bin" -OnlyIfMissing }
    $script:install = $dir
}

# Later patches to the SDK (patches\README.md), applied in the order they were
# made, as several change neighbouring lines: each goes onto an SDK patched before
# it existed, found by a name it adds. Above the steps, as a prebuilt SDK is
# checked against the same list.
$laterPatches = @(
    @{ Patch = "rexglue-gpu-regwrite-bulk.patch"; File = "src\graphics\command_processor.cpp"; Adds = "gpu_regwrite_bulk" }
    @{ Patch = "rexglue-imgui-dialog-repaint-on-demand.patch"; File = "include\rex\ui\imgui_dialog.h"; Adds = "RepaintsContinuously" }
    @{ Patch = "rexglue-inline-bit-helpers.patch"; File = "include\rex\math.h"; Adds = "Out of line in rexruntime" }
    @{ Patch = "rexglue-perf-counters-batched.patch"; File = "src\graphics\command_processor.cpp"; Adds = "gpu_batch_perf_counters" }
    @{ Patch = "rexglue-shared-memory-request-fast-path.patch"; File = "src\graphics\shared_memory.cpp"; Adds = "shared_memory_request_fast_path" }
    @{ Patch = "rexglue-threaded-submission-d3d12.patch"; File = "src\graphics\d3d12\command_processor.cpp"; Adds = "d3d12_threaded_submission" }
    @{ Patch = "rexglue-timer-queue-blocking-wait.patch"; File = "src\core\timer_queue.cpp"; Adds = "timer_queue_blocking_wait" }
    @{ Patch = "rexglue-shared-memory-fast-path-hot-pages.patch"; File = "src\graphics\shared_memory.cpp"; Adds = "shared_memory_request_fast_path_with_hot_pages" }
    @{ Patch = "rexglue-hot-pages-watched-ranges.patch"; File = "src\graphics\shared_memory.cpp"; Adds = "Watching wins" }
    @{ Patch = "rexglue-cursor-auto-hide.patch"; File = "src\ui\window.cpp"; Adds = "RequestAutoHiddenCursorHide" }
    @{ Patch = "rexglue-vulkan-memexport-check-order.patch"; File = "src\graphics\vulkan\command_processor.cpp"; Adds = "most draws export nothing" }
    @{ Patch = "rexglue-vulkan-hoist-shared-memory-uploads.patch"; File = "src\graphics\vulkan\command_processor.cpp"; Adds = "vulkan_hoist_shared_memory_uploads" }
    @{ Patch = "rexglue-threaded-submission-vulkan.patch"; File = "src\graphics\vulkan\command_processor.cpp"; Adds = "vulkan_threaded_submission" }
    @{ Patch = "rexglue-d3d12-placed-texture-heaps.patch"; File = "src\graphics\d3d12\texture_cache.cpp"; Adds = "d3d12_texture_heaps" }
    @{ Patch = "rexglue-vulkan-texture-load-storage-copy.patch"; File = "src\graphics\vulkan\texture_cache.cpp"; Adds = "vulkan_texture_load_storage_copy" }
    @{ Patch = "rexglue-clear-rectangle-draws.patch"; File = "src\graphics\pipeline\render_target\cache.cpp"; Adds = "clear_rectangle_draws" }
    @{ Patch = "rexglue-texture-load-direct.patch"; File = "src\graphics\pipeline\texture\cache.cpp"; Adds = "texture_load_direct" }
    @{ Patch = "rexglue-async-pipelines-no-frame-wait.patch"; File = "src\graphics\command_processor.cpp"; Adds = "async_pipeline_wait_budget_ms" }
    @{ Patch = "rexglue-vulkan-pipeline-storage-parallel-translation.patch"; File = "src\graphics\vulkan\pipeline_cache.cpp"; Adds = "vulkan_pipeline_storage_translation_threads" }
    @{ Patch = "rexglue-vulkan-pipeline-cache-file.patch"; File = "src\graphics\vulkan\pipeline_cache.cpp"; Adds = "vulkan_pipeline_cache_file" }
    @{ Patch = "rexglue-resolve-4xmsaa-depth-samples.patch"; File = "src\graphics\shaders\bytecode\d3d12_5_1\resolve_fast_32bpp_4xmsaa_cs.h"; Adds = "Approximately 200 instruction slots used" }
    @{ Patch = "rexglue-direct-resolve.patch"; File = "src\graphics\pipeline\render_target\cache.cpp"; Adds = "GetDirectResolveSource" }
    @{ Patch = "rexglue-cvar-toml-strings.patch"; File = "src\core\cvar.cpp"; Adds = "LoadConfigByLine" }
    @{ Patch = "rexglue-edram-size.patch"; File = "src\graphics\pipeline\render_target\cache.cpp"; Adds = "edram_tiles" }
    @{ Patch = "rexglue-guest-markers.patch"; File = "include\rex\graphics\command_processor.h"; Adds = "OnGuestMarker" }
    @{ Patch = "rexglue-hdr-output.patch"; File = "src\ui\presenter.cpp"; Adds = "IsHdrOutputWanted" }
    @{ Patch = "rexglue-hdr-ui-layer.patch"; File = "src\graphics\command_processor.cpp"; Adds = "HdrBeforeResolve" }
    @{ Patch = "rexglue-hdr-scene-mcla.patch"; File = "src\graphics\command_processor.cpp"; Adds = "HdrPrepareScene" }
    @{ Patch = "rexglue-temporal-aa.patch"; File = "include\rex\graphics\command_processor.h"; Adds = "TaaJitterNdcOffset" }
    @{ Patch = "rexglue-temporal-aa-shaders-vk.patch"; File = "thirdparty\fidelityfx-shaders-vk\rexglue_fsr_shaders_vk.h"; Adds = "REXGLUE_FSR_SHADERS_VK_FLAGS" }
    @{ Patch = "rexglue-fullscreen-setting-ui-thread.patch"; File = "src\ui\rex_app.cpp"; Adds = "after the lock is released" }
    @{ Patch = "rexglue-presenter-ui-overlay.patch"; File = "src\ui\vulkan\vulkan_presenter_ui_overlay.cpp"; Adds = "CreateUIOverlayFromUIThread" }
    @{ Patch = "rexglue-frame-generation.patch"; File = "src\ui\vulkan\vulkan_presenter_frame_generation.cpp"; Adds = "FramePacerThread" }
    @{ Patch = "rexglue-vulkan-paint-pipeline-format.patch"; File = "src\ui\vulkan\vulkan_presenter.cpp"; Adds = "swapchain_effect_pipeline.swapchain_format =" }
    @{ Patch = "rexglue-vulkan-present-semaphore-per-image.patch"; File = "src\ui\vulkan\vulkan_presenter.cpp"; Adds = "swapchain_present_semaphores.push_back" }
    @{ Patch = "rexglue-d3d12-storage-preload-threads.patch"; File = "src\graphics\d3d12\pipeline_cache.cpp"; Adds = "creation_threads_.size() < creation_thread_needed_count" }
    @{ Patch = "rexglue-vulkan-pipeline-storage-keep-unsupported.patch"; File = "src\graphics\vulkan\pipeline_cache.cpp"; Adds = "pipeline_storage_valid_count" }
    @{ Patch = "rexglue-shader-storage-prepare.patch"; File = "src\graphics\shader_storage_progress.h"; Adds = "ShaderStorageProgress" }
    @{ Patch = "rexglue-content-header-padding.patch"; File = "src\system\xam\content_manager.cpp"; Adds = "Written as zeros." }
    @{ Patch = "rexglue-user-name.patch"; File = "src\system\xam\user_profile.cpp"; Adds = "ProfileNameFromCvar" }
    @{ Patch = "rexglue-low-latency.patch"; File = "src\ui\low_latency.cpp"; Adds = "LowLatency::OnFrameStart" }
    @{ Patch = "rexglue-frame-generation-nonwindows.patch"; File = "src\ui\vulkan\vulkan_presenter_frame_generation.cpp"; Adds = "frame_generation_pacing" }
    @{ Patch = "rexglue-threaded-submission-d3d12-idle-wait.patch"; File = "src\graphics\d3d12\command_processor.cpp"; Adds = "d3d12_threaded_submission_idle_wait" }
    @{ Patch = "rexglue-fsr-upscaling.patch"; File = "include\rex\graphics\command_processor.h"; Adds = "TaaResolveStretch" }
    @{ Patch = "rexglue-guest-atomics-lock-free.patch"; File = "src\core\mutex.cpp"; Adds = "guest_atomics_lock_free" }
    @{ Patch = "rexglue-resolve-direct-batched-loads.patch"; File = "src\graphics\pipeline\render_target\cache.cpp"; Adds = "resolve_direct_batched_loads" }
    @{ Patch = "rexglue-xma-decoder-fixes.patch"; File = "src\audio\xma_context.cpp"; Adds = "xma_race_fixes" }
    @{ Patch = "rexglue-alpha-to-mask-tolerance.patch"; File = "src\graphics\flags.cpp"; Adds = "alpha_to_mask_tolerance" }
    @{ Patch = "rexglue-resolve-downscale-unit-layout.patch"; File = "src\graphics\command_processor.cpp"; Adds = "resolve_downscale_unit_layout" }
    @{ Patch = "rexglue-resolve-readback-rules.patch"; File = "src\graphics\command_processor.cpp"; Adds = "readback_resolve_rules" }
    @{ Patch = "rexglue-resolve-unscaled-copy.patch"; File = "src\graphics\command_processor.cpp"; Adds = "resolve_unscaled_copy_rules" }
    @{ Patch = "rexglue-vulkan-odepth-scale.patch"; File = "src\graphics\flags.cpp"; Adds = "vulkan_odepth_float24_scale" }
    @{ Patch = "rexglue-render-target-max-height.patch"; File = "src\graphics\pipeline\render_target\cache.cpp"; Adds = "render_target_max_height" }
    @{ Patch = "rexglue-render-target-trim.patch"; File = "src\graphics\pipeline\render_target\cache.cpp"; Adds = "render_target_trim_seconds" }
    @{ Patch = "rexglue-gpu-memory.patch"; File = "src\graphics\command_processor.cpp"; Adds = "gpu_memory_log_ms" }
    # Small fixes from open pull requests to upstream ReXGlue (patches\README.md credits each).
    @{ Patch = "rexglue-upstream-storage-flush.patch"; File = "src\graphics\vulkan\pipeline_cache.cpp"; Adds = "waiting first would" }
    @{ Patch = "rexglue-upstream-host-fp-state.patch"; File = "include\rex\ppc\context.h"; Adds = "Keep host FP exceptions masked on every write" }
    @{ Patch = "rexglue-upstream-linux-wine.patch"; File = "src\core\dynlib_win.cpp"; Adds = "GetModuleHandleExW" }
    @{ Patch = "rexglue-upstream-small-fixes.patch"; File = "include\rex\system\xam\user_profile.h"; Adds = "Settings are owned, and replaced, through" }
    # llvm-mingw (-Toolchain mingw). Applied with either toolchain: guarded
    # by the compiler or a no-op for the other, so that one patched SDK
    # serves both, and switching toolchains needs no new patching.
    @{ Patch = "rexglue-mingw.patch"; File = "cmake\mingw_imp_exports.cmake"; Adds = "__imp_" }
    @{ Patch = "rexglue-ffmpeg-mingw-config.patch"; Repo = "thirdparty\FFmpeg"; File = "config.h"; Adds = "HAVE_LIBC_MSVCRT 0" }
)

# Git refuses a repository folder whose path is over 220 characters, with
# core.longpaths or without; the longest below this folder is 72 (the
# submodule vulkan-memory-allocator's), so this folder's path may have 148.
if ($root.Length -gt 148) {
    Fail "this folder's path is $($root.Length) characters long, and git cannot clone the SDK into one over 148. Move the whole folder somewhere with a shorter path (for example C:\Games\mcla-recomp) and run this again."
}

# A first clone that was cut short leaves submodules missing, or with files
# that were never checked out, and the clone is skipped on the next run once
# the SDK's .git exists (issue #21). Fetch what is missing and put back files
# missing from a submodule; changed files (an applied patch) are left alone.
function RepairSubmodules {
    Logged "sdk-submodules" { git -C $sdk submodule update --init --recursive }
    $ErrorActionPreference = "Continue"
    foreach ($line in @(git -C $sdk submodule status --recursive)) {
        $path = Join-Path $sdk ($line.Substring(1).Split(' ')[1])
        $missing = @(git -C $path ls-files --deleted)
        if ($missing.Count -eq 0) { continue }
        "restoring $($missing.Count) missing files in $path"
        # A list file without a byte order mark: Windows PowerShell 5.1 puts
        # one in front of text piped to a program, and git takes it as part
        # of the first path.
        $list = "$logs\sdk-missing-files.txt"
        [IO.File]::WriteAllLines($list, [string[]]$missing)
        git -C $path checkout --pathspec-from-file=$list 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail "could not restore the files missing in $path; delete third_party\rexglue-sdk and run this again" }
    }
}

Push-Location $root
try {
    $lastOptions = Get-BuildOptions $root
    if ($lastOptions -and ($lastOptions.Toolchain -ne $Toolchain -or $lastOptions.ToolsDir -ne $ToolsDir -or
                           $lastOptions.SdkBundle -ne $SdkBundle -or $lastOptions.SdkBundleSha256 -ne $SdkBundleSha256.Trim())) {
        "note: the last build used -Toolchain $($lastOptions.Toolchain)$(if ($lastOptions.ToolsDir) { ' -ToolsDir ' + $lastOptions.ToolsDir })$(if ($lastOptions.SdkBundle) { ' -SdkBundle ' + $lastOptions.SdkBundle }); this one is $Toolchain, and launcher\build-options.json will say so (launcher\build-game.ps1 without options builds as the last build did)"
    }
    # -----------------------------------------------------------------------
    Step "1/6 tools"
    # What the build needs, how to tell that it is there, and how to get it.
    # Programs come from winget (part of Windows 10 and 11); Clang is the
    # portable build from the LLVM project, unpacked into tools\.
    $llvmVersion = "20.1.8"
    $llvmName = "clang+llvm-$llvmVersion-x86_64-pc-windows-msvc"
    # The SHA-256 of the release's .tar.xz (the digest GitHub shows for the asset).
    $llvmSha256 = "f229769f11d6a6edc8ada599c0cda964b7dee6ab1a08c6cf9dd7f513e85b107f"
    # -Toolchain mingw: llvm-mingw, Clang $llvmVersion again with lld, libc++ and
    # mingw-w64, in one zip from its release page. Pinned, with the SHA-256 of
    # the zip (the one GitHub shows for the release asset); a download that does
    # not match is deleted. Programs it builds use Windows' own UCRT.
    $mingwVersion = "20250709"
    $mingwName = "llvm-mingw-$mingwVersion-ucrt-x86_64"
    $mingwUrl = "https://github.com/mstorsjo/llvm-mingw/releases/download/$mingwVersion/$mingwName.zip"
    $mingwSha256 = "82babcd6aae4dc3606e8e0471d816989c384b4bf86a139f184a8b4a1b2c2758d"
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    # Visual Studio 2022 17.1 or newer: the SDK uses std::byteswap, which
    # Microsoft's C++ library has from that version on. With an older one
    # (VS 2019, or an early 2022) the SDK build fails with "no member named
    # 'byteswap' in namespace 'std'".
    $vcTools = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
    function HasBuildTools {
        if (-not (Test-Path -LiteralPath $vswhere)) { return $false }
        [bool](& $vswhere -products * -requires $vcTools -version "[17.1," -property installationPath)
    }
    # The newest Visual Studio with the C++ tools of any version, to name an
    # install that is there but too old.
    function OldBuildTools {
        if (-not (Test-Path -LiteralPath $vswhere)) { return "" }
        $found = @(& $vswhere -products * -requires $vcTools -sort -format json | ConvertFrom-Json) | Select-Object -First 1
        if ($found) { "$($found.displayName) ($($found.installationVersion))" } else { "" }
    }
    # A Windows SDK new enough for the SDK's Direct3D 12 code: 10.0.19041 and
    # older lack D3D12_FEATURE_DATA_D3D12_OPTIONS8, and the SDK build fails on
    # it (issue #21). Clang takes the newest one installed, so that is the one
    # checked; the version is returned to name it when it is too old.
    $windowsSdk = "10.0.26100"
    function NewestWindowsSdk {
        # Both registry views can name a Windows Kits folder, and one may hold
        # no headers (a 64-bit entry next to the SDK in Program Files (x86)),
        # so every one is searched.
        $kits = @(Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots", "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" -ErrorAction SilentlyContinue |
            ForEach-Object { $_.KitsRoot10 }) + "${env:ProgramFiles(x86)}\Windows Kits\10\" | Where-Object { $_ } | Select-Object -Unique
        $versions = @($kits | ForEach-Object { Get-ChildItem "$_\Include" -Directory -ErrorAction SilentlyContinue } |
            Where-Object { $_.Name -match '^10\.[0-9.]+$' -and (Test-Path -LiteralPath "$($_.FullName)\um\d3d12.h") } |
            Sort-Object { [version]$_.Name })
        if ($versions) { $versions[-1] } else { $null }
    }
    function HasWindowsSdk {
        $newest = NewestWindowsSdk
        $newest -and (Select-String -Quiet -SimpleMatch "D3D12_FEATURE_DATA_D3D12_OPTIONS8" -LiteralPath "$($newest.FullName)\um\d3d12.h")
    }
    # Whether a path is the folder or inside it (-ToolsDir, which may be empty).
    function InFolder([string]$path, [string]$folder) {
        if (-not $path -or -not $folder -or -not (Test-Path -LiteralPath $path)) { return $false }
        $folder = $folder.TrimEnd('\')
        ($path.TrimEnd('\') -eq $folder) -or $path.StartsWith($folder + '\', [StringComparison]::OrdinalIgnoreCase)
    }
    function HasPython {
        if (-not (Get-Command python -ErrorAction SilentlyContinue)) { return $false }
        $ErrorActionPreference = "Continue"
        $version = try { python --version 2>&1 | Out-String } catch { "" }
        ($LASTEXITCODE -eq 0) -and ($version -match "^Python 3")
    }
    function RefreshPath {
        $env:PATH = [Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" +
                    [Environment]::GetEnvironmentVariable("PATH", "User")
        . "$PSScriptRoot\env.ps1" -Toolchain $Toolchain -ToolsDir $ToolsDir
    }
    # llvm-mingw into tools\: downloaded, checked against the pinned SHA-256
    # (a download that does not match is deleted and nothing is unpacked),
    # unpacked next to its final place and moved there only when complete, so
    # that an interrupted run leaves no half-unpacked compiler that looks
    # installed. No administrator rights, no registry, no PATH.
    function FetchMingw {
        New-Item -ItemType Directory -Force "$root\tools" | Out-Null
        $archive = "$root\tools\$mingwName.zip"
        $partial = "$archive.part"
        function Sha256Of([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }
        # Its longest path is 117 characters (a Python extension): from a folder of
        # more than 133 it passes 260, which tar.exe copes with and PowerShell 5.1
        # does not (nor the .NET fallback).
        if ($root.Length -gt 133) {
            "note: this folder's path is $($root.Length) characters long, and llvm-mingw has files whose paths pass 260 characters from here: if unpacking or removing it fails, move the folder to a shorter path"
        }
        # A zip that is there already and right (an earlier run's, or one put there)
        # is used, and is not removed after.
        $downloaded = $false
        if (-not ((Test-Path -LiteralPath $archive) -and (Sha256Of $archive) -eq $mingwSha256)) {
            "downloading llvm-mingw $mingwVersion (about 170 MB) into tools\"
            # Named install-clang for the launcher, which shows "Downloading Clang" for that log.
            Logged "install-clang" { curl.exe -L --fail --proto "=https" --proto-redir "=https" -o $partial $mingwUrl }
            $actual = Sha256Of $partial
            if ($actual -ne $mingwSha256) {
                Remove-Item -LiteralPath $partial -Force
                Fail "the llvm-mingw download does not match its pinned SHA-256 ($mingwSha256; got $actual), so it was deleted and nothing was unpacked. Try again, or download $mingwUrl yourself, unpack it and give the folder with -ToolsDir."
            }
            Move-Item -LiteralPath $partial -Destination $archive -Force
            $downloaded = $true
        }
        "unpacking llvm-mingw (700 MB; about a minute)"
        # A short folder of its own: the archive's paths are long.
        $unpack = "$root\tools\.u"
        UnzipTo "unpack-llvm-mingw" $archive $unpack
        if (-not (Test-Path -LiteralPath "$unpack\$mingwName\bin\clang.exe")) { Fail "the llvm-mingw archive does not hold $mingwName\bin\clang.exe" }
        RemoveFolder "$root\tools\$mingwName"
        Move-Item -LiteralPath "$unpack\$mingwName" -Destination "$root\tools\$mingwName"
        Remove-Item -LiteralPath $unpack -Recurse -Force -ErrorAction SilentlyContinue
        if ($downloaded) { Remove-Item -LiteralPath $archive -Force }
    }
    function MissingTools {
        RefreshPath
        $list = @()
        # A prebuilt SDK needs no Git (it is for cloning the SDK), and no Python
        # (for building it) once the game is extracted.
        if (-not $SdkBundle -and -not (Get-Command git -ErrorAction SilentlyContinue)) { $list += "Git" }
        if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $list += "CMake" }
        if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { $list += "Ninja" }
        # Python counts if it runs: Windows' placeholder python.exe (it only
        # points at the Microsoft Store) fails, while Python installed from the
        # Store works from the same folder (issue #13).
        # (Python builds the SDK, where glslang's build_info.py runs, and extracts the
        # game; -Until stops before the game and, at sdk-source, before the build.)
        $buildsSdk = -not $SdkBundle -and $Until -ne "tools" -and $Until -ne "sdk-source"
        $extractsGame = -not $Until -and -not (Test-Path -LiteralPath "$root\game\default.xex")
        if (($buildsSdk -or $extractsGame) -and -not (HasPython)) { $list += "Python" }
        if ($mingw) {
            # The pinned release unpacked into tools\ counts, and so does one in
            # -ToolsDir (the user's choice). Another llvm-mingw in tools\ does not: after
            # an update that pins a newer release, the old one is not used instead.
            if ($env:MCLA_MINGW_ROOT -ne "$root\tools\$mingwName" -and -not (InFolder $env:MCLA_MINGW_ROOT $ToolsDir)) { $list += "llvm-mingw" }
        } else {
            if (-not (HasBuildTools)) { $list += "Visual Studio Build Tools" }
            elseif (-not (HasWindowsSdk)) { $list += "Windows SDK" }
            # Only the project's own Clang counts: another one on the path may not
            # find the Microsoft headers (issue #9). One in -ToolsDir is the user's choice.
            if (-not (Test-Path -LiteralPath "$root\tools\$llvmName\bin\clang.exe") -and
                -not (InFolder $env:MCLA_CC $ToolsDir)) { $list += "Clang" }
        }
        # Optional: FSR 3.1's Vulkan runtime is built only with it (and only
        # when the SDK is built here with msvc: llvm-mingw cannot build FSR).
        if (-not $SdkBundle -and -not $mingw -and $Until -ne "tools" -and $Until -ne "sdk-source" -and -not (HasVulkanSdk)) { $list += "Vulkan SDK" }
        $list
    }
    # The Vulkan SDK (its headers and loader library): VULKAN_SDK, as its
    # installer sets it for the machine, or its usual folder.
    function HasVulkanSdk {
        $machine = [Environment]::GetEnvironmentVariable("VULKAN_SDK", "Machine")
        if (-not $env:VULKAN_SDK -and $machine) { $env:VULKAN_SDK = $machine }
        if ($env:VULKAN_SDK -and (Test-Path -LiteralPath "$env:VULKAN_SDK\Include\vulkan\vulkan.h")) { return $true }
        [bool](Get-ChildItem "C:\VulkanSDK\*\Include\vulkan\vulkan.h" -ErrorAction SilentlyContinue)
    }
    $missing = @(MissingTools)
    $required = @($missing | Where-Object { $_ -ne "Vulkan SDK" })
    # Nobody can be asked when the launcher runs this (-NonInteractive).
    $nonInteractive = [bool]([Environment]::GetCommandLineArgs() | Where-Object { $_ -like "-NonI*" })
    if ($missing -and -not $required -and -not $InstallTools) {
        # Only the Vulkan SDK: the game builds without it, FSR then for
        # Direct3D 12 only. Asked only when someone can answer.
        $answer = if ($nonInteractive) { "n" } else {
            Read-Host "The Vulkan SDK is missing: without it, the FSR anti-aliasing option is built for Direct3D 12 only. Install it now (from winget; it asks for administrator rights)? [y/N]"
        }
        if ($answer -notmatch '^[yY]') {
            "going on without the Vulkan SDK: FSR on Vulkan will be unavailable"
            $missing = @()
        }
    }
    if ($missing) {
        "missing: $($missing -join ', ')"
        $old = OldBuildTools
        if ($missing -contains "Visual Studio Build Tools" -and $old) {
            "$old is installed, but the build needs Visual Studio 2022 version 17.1 or newer. Update it in the Visual Studio Installer, or let this install Build Tools 2022 next to it."
        }
        $sdkFound = NewestWindowsSdk
        if ($missing -contains "Windows SDK" -and $sdkFound) {
            "Windows SDK $($sdkFound.Name) is installed, but the build needs a newer one; this installs Windows SDK $windowsSdk next to it."
        }
        if ($required -and -not $InstallTools -and $nonInteractive) {
            # llvm-mingw alone is fetched without asking: a download into tools\ that is
            # checked against its pinned SHA-256, needing no administrator rights and
            # changing nothing outside the folder, and the player chose mingw already (the
            # option, or the record of the last build: an install that switches with an
            # update). Anything else would be installed on the PC, so it is not, unasked.
            $others = @($required | Where-Object { $_ -ne "llvm-mingw" })
            if ($others) {
                Fail "missing: $($others -join ', '). Nobody can be asked here: run .\launcher\build-game.ps1 -InstallTools in a terminal, or install them yourself (README.md, 'Windows')"
            }
            "fetching llvm-mingw without asking (about 170 MB into tools\, checked against its pinned SHA-256; no administrator rights)"
        } elseif ($required -and -not $InstallTools) {
            $answer = if ($mingw) {
                Read-Host "Install them now? Programs come from winget (CMake's installer may ask for administrator rights), the compiler (llvm-mingw, about 170 MB, no administrator rights) from its release page, checked against a pinned SHA-256; the Vulkan SDK asks for administrator rights. [y/N]"
            } else {
                Read-Host "Install them now? Programs come from winget, Clang from the LLVM project's releases; the Build Tools are several GB and ask for administrator rights, and so do the Windows SDK and the Vulkan SDK. [y/N]"
            }
            if ($answer -notmatch '^[yY]') { Fail "install them (see 'Windows' in README.md) and run this again, or run with -InstallTools" }
        }
        $ids = @{ "Git" = "Git.Git"; "CMake" = "Kitware.CMake"; "Ninja" = "Ninja-build.Ninja"; "Python" = "Python.Python.3.12" }
        # winget is for what comes from winget: not for llvm-mingw (a download) or Clang.
        $fromWinget = @($missing | Where-Object { $ids.ContainsKey($_) -or $_ -eq "Visual Studio Build Tools" -or $_ -eq "Windows SDK" -or $_ -eq "Vulkan SDK" })
        if ($fromWinget -and -not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is not available; install $($fromWinget -join ', ') by hand (README.md, 'Windows')" }
        foreach ($name in $missing) {
            if ($ids.ContainsKey($name)) {
                "installing $name"
                Logged "install-$name" { winget install --id $ids[$name] -e --accept-source-agreements --accept-package-agreements }
            } elseif ($name -eq "Visual Studio Build Tools") {
                "installing Visual Studio Build Tools with the C++ workload (this takes a while)"
                Logged "install-build-tools" {
                    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements `
                        --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
                }
            } elseif ($name -eq "Windows SDK") {
                "installing Windows SDK $windowsSdk (this takes a while)"
                Logged "install-windows-sdk" { winget install --id "Microsoft.WindowsSDK.$windowsSdk" -e --accept-source-agreements --accept-package-agreements }
            } elseif ($name -eq "Clang") {
                "downloading Clang $llvmVersion (about 900 MB) into tools\"
                New-Item -ItemType Directory -Force "$root\tools" | Out-Null
                $archive = "$root\tools\$llvmName.tar.xz"
                # Python, installed just before if it was missing, unpacks it
                # when tar.exe cannot: the one in Windows 10 has no .xz support
                # ("unable to run program xz", issue #10).
                RefreshPath
                Logged "install-clang" {
                    curl.exe -L --fail --proto "=https" --proto-redir "=https" -o $archive "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvmVersion/$llvmName.tar.xz"
                    if ($LASTEXITCODE -ne 0) { return }
                    # Checked before it is unpacked; one that does not match is deleted.
                    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
                    if ($actualHash -ne $llvmSha256) {
                        "the download's SHA-256 is $actualHash, not the pinned ${llvmSha256}: deleted, nothing unpacked"
                        Remove-Item -LiteralPath $archive -Force
                        $global:LASTEXITCODE = 1
                        return
                    }
                    # Unpacked next to its final place and moved there only when complete, as
                    # llvm-mingw is: an interrupted run leaves no half-unpacked Clang that
                    # looks installed.
                    RemoveFolder "$root\tools\.u"
                    New-Item -ItemType Directory -Force "$root\tools\.u" | Out-Null
                    tar.exe -xf $archive -C "$root\tools\.u"
                    if ($LASTEXITCODE -ne 0) {
                        "tar.exe could not unpack the archive; unpacking it with Python instead (about a minute)"
                        python -c "import sys, tarfile; kw = {'filter': 'data'} if hasattr(tarfile, 'data_filter') else {}; tarfile.open(sys.argv[1], 'r:xz').extractall(sys.argv[2], **kw)" $archive "$root\tools\.u"
                    }
                    if ($LASTEXITCODE -ne 0) { return }
                    if (-not (Test-Path -LiteralPath "$root\tools\.u\$llvmName\bin\clang.exe")) {
                        "the archive does not hold $llvmName\bin\clang.exe"
                        $global:LASTEXITCODE = 1
                        return
                    }
                    RemoveFolder "$root\tools\$llvmName"
                    Move-Item -LiteralPath "$root\tools\.u\$llvmName" -Destination "$root\tools\$llvmName"
                    RemoveFolder "$root\tools\.u"
                }
                Remove-Item -LiteralPath $archive -ErrorAction SilentlyContinue
            } elseif ($name -eq "llvm-mingw") {
                FetchMingw
            } elseif ($name -eq "Vulkan SDK") {
                # Not through Logged: the build goes on without it.
                "installing the Vulkan SDK (for FSR on Vulkan; it asks for administrator rights)"
                & {
                    $ErrorActionPreference = "Continue"
                    winget install --id KhronosGroup.VulkanSDK -e --accept-source-agreements --accept-package-agreements 2>&1 |
                        ForEach-Object { "$_" } > "$logs\install-vulkan-sdk.log"
                }
                if ($LASTEXITCODE -ne 0) { "the Vulkan SDK was not installed (log: $logs\install-vulkan-sdk.log)" }
            }
        }
        $missing = @(MissingTools)
        if ($missing -contains "Vulkan SDK") {
            "the Vulkan SDK is missing: FSR anti-aliasing will be built for Direct3D 12 only (on Vulkan the game uses MSAA instead)"
        }
        $missing = @($missing | Where-Object { $_ -ne "Vulkan SDK" })
        if ($missing) { Fail "still missing after installing: $($missing -join ', '). A new PowerShell window may be needed; then run this again" }
    }
    # The compilers CMake is given are by full path (env.ps1 sets them): from the
    # project's own Clang folder whatever else is in tools\ (or the one -ToolsDir
    # names), and the build stops here if that folder lacks one of them.
    foreach ($compiler in $env:MCLA_CC, $env:MCLA_CXX) {
        if (-not $compiler -or -not (Test-Path -LiteralPath $compiler)) {
            Fail "the compiler is incomplete: $(if ($compiler) { $compiler } else { 'clang or clang++' }) is missing. Delete its folder in tools\ (or fix the one -ToolsDir names) and run this again"
        }
    }
    "clang: $env:MCLA_CC ($((& $env:MCLA_CC --version | Select-Object -First 1)))"
    if ($mingw) {
        "toolchain: mingw (llvm-mingw, libc++ and Windows' UCRT; the game needs no Visual C++ runtime, nor do FSR's DLLs)"
        if ($env:MCLA_MINGW_ROOT -notlike "*$mingwName") {
            "WARNING: the build is made and tested with llvm-mingw $mingwVersion (Clang $llvmVersion, libc++ 20); this one is $env:MCLA_MINGW_ROOT, and the SDK may not build with it (a newer libc++ can bring declarations the SDK makes itself, and the toolchain's own fixes)"
        }
    }

    # Compiler jobs: Ninja starts one per CPU thread (and two more), and a
    # compiler takes up to 0.4 GB (the codegen 0.7 GB), so a PC with 8 GB
    # would page. Capped at one job per 0.5 GB of memory available (free and
    # reclaimable), only when that is fewer than Ninja's own number, so that a
    # PC with enough memory builds exactly as before. Every cmake --build
    # below (the SDK's, FidelityFX's, the game's) takes it from the environment.
    # (The figure is one look at the memory available now, in 17 ms; see mcla-lib.ps1.)
    if ($env:CMAKE_BUILD_PARALLEL_LEVEL) {
        "compiler jobs: $env:CMAKE_BUILD_PARALLEL_LEVEL (CMAKE_BUILD_PARALLEL_LEVEL)"
    } else {
        $availableMb = Get-AvailableMemoryMb
        $jobLimit = Get-JobLimit $availableMb ([Environment]::ProcessorCount)
        if ($jobLimit) {
            $env:CMAKE_BUILD_PARALLEL_LEVEL = "$jobLimit"
            "memory: $([Math]::Round($availableMb / 1024, 1)) GB available; building with $jobLimit compiler jobs instead of $([Environment]::ProcessorCount + 2)"
        }
    }

    if ($Until -eq "tools") {
        "stopping after the tools (-Until tools)"
        exit 0
    }

    # -----------------------------------------------------------------------
    if ($SdkBundle) {
        Step "2/6 ReXGlue SDK: the prebuilt bundle"
        UseSdkBundle
        Step "3/6 build and install the SDK"
        "not needed: the bundle is prebuilt ($install)"
    } else {
        Step "2/6 ReXGlue SDK $sdkTag with this project's patches"
        if (-not (Test-Path -LiteralPath "$sdk\.mcla-patched")) {
            if (-not (Test-Path -LiteralPath "$sdk\.git")) {
                Logged "sdk-clone" { git clone --recursive --branch $sdkTag https://github.com/rexglue/rexglue-sdk.git $sdk }
            }
            if ((git -C $sdk rev-parse HEAD) -ne $sdkCommit) { Fail "the SDK checkout is not $sdkTag ($sdkCommit)" }
            RepairSubmodules
            if (-not (Test-Path -LiteralPath "$sdk\thirdparty\FFmpeg\config.h")) { Fail "the SDK download is incomplete (thirdparty\FFmpeg); delete third_party\rexglue-sdk and run this again" }
            Patch "sdk-patch" $sdk "$root\patches\rexglue-v0.10.0-mcla.patch"
            Patch "sdk-patch-ffmpeg" "$sdk\thirdparty\FFmpeg" "$root\patches\rexglue-ffmpeg-ps5-config.patch"
            # Git on Windows checks out symbolic links as small text files holding
            # the link's target; one of the SDK's libraries has some. Replace them
            # with copies of what they point to. With Developer Mode on (or as an
            # administrator) Git makes real links instead, which need nothing:
            # they are skipped, as reading one gives the target's contents.
            $links = "$sdk\thirdparty\libmspack\cabextract\mspack"
            Get-ChildItem -LiteralPath $links -File | Where-Object { $_.Length -lt 200 -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | ForEach-Object {
                $target = Join-Path $links (Get-Content -LiteralPath $_.FullName -Raw).Trim()
                if (Test-Path -LiteralPath $target) { Copy-Item $target $_.FullName -Force }
            }
            New-Item -ItemType File "$sdk\.mcla-patched" | Out-Null
        } else {
            "already patched: $sdk"
            CheckPatch "$root\patches\rexglue-v0.10.0-mcla.patch"
            CheckPatch "$root\patches\rexglue-ffmpeg-ps5-config.patch"
        }
        # The car reflection fix came later than the patches
        # above: an SDK patched before it gets it here, and the marker's new time
        # has step 3 build the SDK again.
        if (-not (Select-String -Quiet -SimpleMatch "kSysFlag_MinMaxColor0TimesSrcAlpha" -LiteralPath "$sdk\include\rex\graphics\pipeline\shader\spirv_translator.h")) {
            Patch "sdk-patch-reflection" $sdk "$root\patches\rexglue-minmax-blend-factor.patch"
            (Get-Item -LiteralPath "$sdk\.mcla-patched" -Force).LastWriteTime = Get-Date
        } else {
            CheckPatch "$root\patches\rexglue-minmax-blend-factor.patch"
        }
        # Later patches to the SDK (patches\README.md), applied in the order they
        # were made, as several change neighbouring lines. Each goes onto an SDK
        # patched before it existed, found by a name it adds, and the marker's new
        # time has step 3 build the SDK again. One that is in already must be the
        # version recorded (CheckPatch).
        foreach ($later in $laterPatches) {
            # A patch to a submodule (FFmpeg) is applied from its folder.
            $laterRepo = if ($later.Repo) { "$sdk\$($later.Repo)" } else { $sdk }
            # A patch that adds its file is not in while the file isn't there.
            if (-not (Test-Path -LiteralPath "$laterRepo\$($later.File)") -or
                -not (Select-String -Quiet -SimpleMatch $later.Adds -LiteralPath "$laterRepo\$($later.File)")) {
                Patch "sdk-patch-$($later.Patch -replace '^rexglue-|\.patch$')" $laterRepo "$root\patches\$($later.Patch)"
                (Get-Item -LiteralPath "$sdk\.mcla-patched" -Force).LastWriteTime = Get-Date
            } else {
                CheckPatch "$root\patches\$($later.Patch)"
            }
        }

        if ($Until -eq "sdk-source") {
            "stopping after the SDK's sources, patched (-Until sdk-source)"
            exit 0
        }

        # -----------------------------------------------------------------------
        Step "3/6 build and install the SDK (the long step, once)"
        # -Force on both: a hidden file (the marker often is one) is invisible to
        # Get-Item otherwise, which would end the script under "Stop".
        $marker = Get-Item -LiteralPath "$sdk\.mcla-patched" -Force
        $recompiler = Get-Item -LiteralPath "$install\bin\rexglue.exe" -Force -ErrorAction SilentlyContinue
        # FSR's runtime missing (an earlier build without the network, or the
        # Vulkan SDK installed since): configured and built again, which fetches
        # and builds it if it can (the rest of the SDK is up to date).
        # Not with mingw: FidelityFX's source needs MSVC's headers (comdef.h, strsafe.h),
        # so that SDK is built without its runtime (step 5 takes the DLLs from elsewhere).
        $fsrMissing = (-not $mingw) -and (-not (Test-Path -LiteralPath "$install\bin\amd_fidelityfx_dx12.dll") -or
            ((HasVulkanSdk) -and -not (Test-Path -LiteralPath "$install\bin\amd_fidelityfx_vk.dll")))
        if ($fsrMissing -and $recompiler) { "FSR's runtime is missing from the SDK's build: building it again" }
        if (-not $recompiler -or $recompiler.LastWriteTime -lt $marker.LastWriteTime -or $fsrMissing) {
            # FSR 3.1 (temporal anti-aliasing) is built from the FidelityFX SDK's
            # source, fetched at a pinned commit (cmake\rexglue_fsr.cmake); its
            # Vulkan library needs the Vulkan SDK, found through VULKAN_SDK
            # (without it, FSR is built for Direct3D 12 only).
            if (-not $env:VULKAN_SDK) {
                $vulkanSdk = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
                    Sort-Object { [version]$_.Name } | Select-Object -Last 1
                if ($vulkanSdk) { $env:VULKAN_SDK = $vulkanSdk.FullName }
            }
            # msvc: the SDK's precompiled headers keep the Windows SDK and Visual Studio
            # headers they were made with, and a new Windows SDK does not make them
            # stale to the build (the old headers are still there), so files then mix
            # two Windows SDKs and fail (issue #21). The build folder records what it
            # was made with and starts again when that changes; one with no record
            # starts again only if the SDK was never installed (so an SDK built before
            # this stays as it is). Not with mingw, which uses neither.
            $toolchainFile = "$sdk\out\build\$sdkBuildName\mcla-toolchain.txt"
            $toolchainNow = "Windows SDK $((NewestWindowsSdk).Name), $env:VCToolsInstallDir"
            if (-not $mingw -and (Test-Path -LiteralPath "$sdk\out\build\$sdkBuildName")) {
                $toolchainThen = if (Test-Path -LiteralPath $toolchainFile) { (Get-Content -LiteralPath $toolchainFile -Raw).Trim() } else { $null }
                if (($toolchainThen -and $toolchainThen -ne $toolchainNow) -or (-not $toolchainThen -and -not $recompiler)) {
                    "the Windows SDK or Visual Studio changed since the SDK build folder was made; starting it again"
                    RemoveFolder "$sdk\out\build\$sdkBuildName"
                }
            }
            Push-Location $sdk
            try {
                # The compilers by full path; a build folder configured earlier
                # with another Clang is then configured again from scratch.
                # mingw: a build folder and an install folder of their own, beside the msvc ones.
                $sdkConfigure = if ($mingw) { @("-B", "out\build\$sdkBuildName", "-DCMAKE_INSTALL_PREFIX=$($install.Replace('\', '/'))") } else { @() }
                # mingw: FSR stays ON, as it is what compiles FSR support into the GPU
                # plugin, but FidelityFX's runtime is not built (its source needs
                # MSVC's headers): a source folder that is not there makes the SDK's
                # CMake skip it with a warning, and not fetch it. The plugin loads the
                # DLLs at run time (step 5 says where they come from).
                if ($mingw) { $sdkConfigure += "-DREXGLUE_FSR_SOURCE_DIR=$($root.Replace('\', '/'))/third_party/fidelityfx-not-built-with-mingw" }
                Logged "sdk-configure" { cmake --preset win-amd64 @sdkConfigure -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FSR=ON "-DCMAKE_C_COMPILER=$env:MCLA_CC" "-DCMAKE_CXX_COMPILER=$env:MCLA_CXX" }
                if (-not $mingw) { Set-Content -LiteralPath $toolchainFile $toolchainNow }
                Logged "sdk-build" { cmake --build out\build\$sdkBuildName --config Release }
                Logged "sdk-install" { cmake --install out\build\$sdkBuildName --config Release }
            } finally { Pop-Location }
        } else {
            "already installed: $install"
        }
        if (-not (Test-Path -LiteralPath "$install\bin\rexglue.exe")) { Fail "the SDK was not installed" }
        # What it was built from (a bundle made of this install says so).
        WriteBundleManifest $install
        # The target's runtime DLLs (libc++, libunwind) beside rexglue.exe and the
        # SDK's DLLs, which need them to run.
        if ($mingw) { CopyMingwRuntime "$install\bin" }
        # FSR is optional: say plainly when it was left out, and what brings it back.
        if ($mingw) {
            # (nothing built here: step 5 says where the DLLs come from)
        } elseif (-not (Test-Path -LiteralPath "$install\bin\amd_fidelityfx_dx12.dll")) {
            "note: the FSR anti-aliasing option was not built this time: fetching the FidelityFX SDK needs the internet, or its build failed (out\logs\sdk-configure.log, sdk-build.log). The game offers MSAA and off; the next build (or Update) tries again."
            # FidelityFX's shader compiler stops at 260 characters, whatever Windows
            # is set to; its longest source file path is 181 below this folder.
            if ($root.Length -gt 75) {
                "      This folder's path is $($root.Length) characters long, and that may be the reason: FidelityFX's shader compiler cannot open paths over 260 characters. With the folder at a path of 75 characters or fewer (for example C:\Games\mcla-recomp), the next build includes FSR."
            }
        } elseif (-not (Test-Path -LiteralPath "$install\bin\amd_fidelityfx_vk.dll")) {
            if (HasVulkanSdk) {
                "note: FSR was built for Direct3D 12 only: its Vulkan runtime failed to build (out\logs\sdk-build.log). With Vulkan the game offers MSAA and off."
            } else {
                "note: FSR was built for Direct3D 12 only: its Vulkan runtime needs the Vulkan SDK (run with -InstallTools, or install it from vulkan.lunarg.com, then build again)."
            }
        }
    }

    if ($Until) {
        "stopping after the SDK (-Until $Until): $install"
        exit 0
    }

    # -----------------------------------------------------------------------
    Step "4/6 your game: extract from the disc image"
    if (-not (Test-Path -LiteralPath "$root\game\default.xex")) {
        if (-not $Iso) { Fail "game\ is empty: give your disc image with -Iso" }
        if (-not (Test-Path -LiteralPath $Iso)) { Fail "no such file: $Iso" }
        Logged "extract" { python "$PSScriptRoot\extract_game.py" $Iso "$root\game" }
    } else {
        "already extracted: $root\game"
    }
    if (-not (Test-Path -LiteralPath "$root\game\default.xex")) { Fail "game\default.xex is missing after extraction" }

    # -----------------------------------------------------------------------
    Step "5/6 recompile the game's code and build it (several minutes the first time)"
    & "$PSScriptRoot\build.ps1" -Preset $Preset -Toolchain $Toolchain -ToolsDir $ToolsDir -Sdk $install
    $build = "$root\out\build\$Preset"
    if (-not (Test-Path -LiteralPath "$build\mcla.exe")) { Fail "mcla.exe was not built (log: $root\out\build.log)" }
    Copy-Item -LiteralPath "$install\bin\rexgpu-xenos.dll" -Destination $build -Force
    # FSR 3.1's runtime (MIT, THIRD-PARTY-NOTICES.md), which the plugin loads.
    Get-ChildItem -LiteralPath "$install\bin" -Filter "amd_fidelityfx_*.dll" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $build -Force }
    if ($mingw) {
        # The SDK's own libc++ and libunwind when it brings them (a bundle's: the
        # ones its DLLs were built with), else the toolchain's.
        if ($SdkBundle -and (Test-Path -LiteralPath "$install\bin\libc++.dll") -and (Test-Path -LiteralPath "$install\bin\libunwind.dll")) {
            CopyMingwRuntime $build "$install\bin"
        } else {
            CopyMingwRuntime $build
        }
        # FidelityFX's runtime does not build with llvm-mingw, but it is plain C
        # to the plugin, so the ones a prebuilt bundle holds (built with MSVC's
        # toolchain in CI) work with it. Without a bundle, the ones this
        # checkout's SDK build with the msvc toolchain made, if the patches have
        # not changed since (the marker is touched whenever one is applied).
        # They have the C runtime linked in (the SDK builds them with the static
        # one), so they need no Visual C++ runtime on the PC either; a DLL that
        # does not load is reported unavailable and the game goes on with MSAA.
        if (-not (Test-Path -LiteralPath "$build\amd_fidelityfx_dx12.dll")) {
            $msvcBin = "$sdk\out\install\win-amd64\bin"
            $theirs = @(Get-ChildItem -LiteralPath $msvcBin -Filter "amd_fidelityfx_*.dll" -ErrorAction SilentlyContinue)
            $patched = Get-Item -LiteralPath "$sdk\.mcla-patched" -Force -ErrorAction SilentlyContinue
            if ($theirs -and $patched -and ($theirs | Where-Object { $_.LastWriteTime -lt $patched.LastWriteTime })) {
                "note: the FSR runtime in the msvc SDK build ($msvcBin) is older than the SDK's patches, so it was not used; run make_pc.ps1 -Toolchain msvc once to refresh it"
                $theirs = @()
            }
            # None, and this PC has the msvc toolchain (Visual Studio Build Tools and the
            # project's Clang, as an install that used it before has): its SDK is built
            # once for them, in a process of its own, as this one's compiler and PATH are
            # llvm-mingw's. It builds the SDK the patches in place now ask for (the
            # runtime is part of it), and stops there. The same as running
            # make_pc.ps1 -Toolchain msvc -Until sdk by hand. Not for a prebuilt SDK: its
            # bundle has the DLLs, or nothing here makes them.
            if (-not $theirs -and -not $SdkBundle -and (HasBuildTools) -and (HasWindowsSdk) -and
                (Test-Path -LiteralPath "$root\tools\$llvmName\bin\clang.exe")) {
                "FSR: FidelityFX's runtime is made by the msvc toolchain, which is here: building its SDK once for it (make_pc.ps1 -Toolchain msvc -Until sdk)"
                $ErrorActionPreference = "Continue"
                & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -Toolchain msvc -Until sdk 2>&1 | ForEach-Object { "$_" }
                $msvcStatus = $LASTEXITCODE
                $ErrorActionPreference = "Stop"
                if ($msvcStatus -ne 0) { "note: the msvc SDK build did not finish ($msvcStatus), so FSR's runtime is not there (out\logs has the log)" }
                $theirs = @(Get-ChildItem -LiteralPath $msvcBin -Filter "amd_fidelityfx_*.dll" -ErrorAction SilentlyContinue)
            }
            if ($theirs) {
                $theirs | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $build -Force }
                "FSR: the runtime ($($theirs.Name -join ', ')) comes from the msvc SDK build"
            } else {
                "note: the FSR anti-aliasing option and frame generation are not available: FidelityFX's runtime (amd_fidelityfx_dx12.dll) does not build with llvm-mingw. A prebuilt SDK bundle has it, and so does an SDK built once with -Toolchain msvc (this build does that itself where Visual Studio Build Tools and the project's Clang are installed). The game offers MSAA and off."
            }
        }
    }

    # What this build was asked for, for an update to ask for the same.
    Save-BuildOptions $root $Toolchain $ToolsDir $SdkBundle $SdkBundleSha256 $install

    # -----------------------------------------------------------------------
    Step "6/6 done"
    # A shortcut in the repository folder, so that playing is a double-click.
    # mcla.exe finds the game folder and its graphics plugin by itself.
    $shortcutPath = "$root\Midnight Club Los Angeles.lnk"
    $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
    $shortcut.TargetPath = "$build\mcla.exe"
    $shortcut.WorkingDirectory = $build
    $shortcut.Save()
    "game:     $build\mcla.exe"
    "shortcut: $shortcutPath"
    "To play, double-click either one."
    if ($Play) { Start-Process "$build\mcla.exe" -WorkingDirectory $build }
} finally { Pop-Location }
