# Dot-source to put the project's toolchain on PATH:  . .\scripts\env.ps1 [-Toolchain mingw]
#
#   -Toolchain   msvc: the project's Clang 20.1.8 in tools\ with Visual Studio
#                Build Tools' headers and libraries; mingw: llvm-mingw (Clang,
#                lld, libc++ and mingw-w64, no Microsoft compiler needed).
#   -ToolsDir    a folder with portable tools to use first: llvm-mingw-*\ (or
#                clang+llvm-*\), cmake*\, ninja*\ (or ninja.exe) and mingit*\
#                (git) in it
#   -Sdk         the SDK install to build against (a prebuilt bundle's folder);
#                by default the one scripts\make_pc.ps1 builds for the toolchain
# With none of the three, the options of the last build are used
# (launcher\build-options.json, written by make_pc.ps1), msvc when there is no
# record: so a checkout built with mingw goes on with mingw.
param([ValidateSet("", "msvc", "mingw")][string]$Toolchain = "", [string]$ToolsDir = "", [string]$Sdk = "")
$root = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\mcla-lib.ps1"

if (-not ($PSBoundParameters.ContainsKey("Toolchain") -or $PSBoundParameters.ContainsKey("ToolsDir") -or $PSBoundParameters.ContainsKey("Sdk"))) {
    $recorded = Get-BuildOptions $root
    if ($recorded) {
        $Toolchain = $recorded.Toolchain
        $ToolsDir = $recorded.ToolsDir
        if ($recorded.SdkBundle -and $recorded.SdkDir) { $Sdk = $recorded.SdkDir }
    }
}
if (-not $Toolchain) { $Toolchain = "msvc" }
if ($ToolsDir) {
    if (-not (Test-Path -LiteralPath $ToolsDir -PathType Container)) { throw "-ToolsDir: no such folder: $ToolsDir" }
    $ToolsDir = (Resolve-Path -LiteralPath $ToolsDir).ProviderPath
}

# The folders of $base whose names match, newest version first.
function ToolFolders([string]$base, [string]$filter) {
    if (-not $base -or -not (Test-Path -LiteralPath $base -PathType Container)) { return @() }
    Sort-ByVersion (Get-ChildItem -LiteralPath $base -Directory -Filter $filter -ErrorAction SilentlyContinue) | ForEach-Object { $_.FullName }
}
# The first folder of the list that holds $file ("" if none).
function FirstWith([string[]]$folders, [string]$file) {
    foreach ($folder in $folders) { if (Test-Path -LiteralPath (Join-Path $folder $file)) { return $folder } }
    ""
}
# A tool in -ToolsDir: a folder named like it, then the folder itself, then its bin.
function FindTool([string]$filter, [string]$file, [string]$sub) {
    $candidates = @(ToolFolders $ToolsDir $filter | ForEach-Object { if ($sub) { Join-Path $_ $sub } else { $_ } })
    if ($ToolsDir) { $candidates += $ToolsDir; $candidates += (Join-Path $ToolsDir "bin") }
    FirstWith $candidates $file
}
# llvm-mingw's bin folder: the first of the list that has clang.exe, and the
# x86_64 runtime (libc++.dll) beside it.
function FindMingwBin([string[]]$candidates) {
    foreach ($c in $candidates) {
        if ((Test-Path -LiteralPath "$c\clang.exe") -and (Test-Path -LiteralPath "$(Split-Path $c -Parent)\x86_64-w64-mingw32\bin\libc++.dll")) { return $c }
    }
    ""
}

$paths = @()
if ($Toolchain -eq "mingw") {
    # llvm-mingw: in -ToolsDir (a folder named llvm-mingw-* first, then the folder
    # itself), else the pinned release unpacked into tools\ by make_pc.ps1 (the one
    # scripts\ci\tools.json names, whatever else is in tools\), else any other
    # llvm-mingw-* there.
    $candidates = @(ToolFolders $ToolsDir "llvm-mingw-*" | ForEach-Object { "$_\bin" })
    if ($ToolsDir) { $candidates += "$ToolsDir\bin"; $candidates += $ToolsDir }
    $bin = FindMingwBin $candidates
    if (-not $bin) {
        $pinned = Get-PinnedToolVersion $root "llvm-mingw"
        $own = @()
        if ($pinned) { $own = @(ToolFolders "$root\tools" "llvm-mingw-$pinned-*") }
        $bin = FindMingwBin @(($own + @(ToolFolders "$root\tools" "llvm-mingw-*")) | ForEach-Object { "$_\bin" })
    }
    if ($bin) {
        $mingwRoot = Split-Path $bin -Parent
        # The target's runtime DLLs (libc++, libunwind), for the programs the
        # build itself runs (rexglue.exe).
        $paths += $bin, "$mingwRoot\x86_64-w64-mingw32\bin"
        $env:MCLA_CC = "$bin\clang.exe"
        $env:MCLA_CXX = "$bin\clang++.exe"
        $env:MCLA_MINGW_ROOT = $mingwRoot
    } else {
        $env:MCLA_CC = ""; $env:MCLA_CXX = ""; $env:MCLA_MINGW_ROOT = ""
    }
} else {
    $bin = ""
    if ($ToolsDir) {
        $bin = FirstWith (@(ToolFolders $ToolsDir "clang+llvm-*" | ForEach-Object { "$_\bin" }) + @("$ToolsDir\bin", $ToolsDir)) "clang.exe"
    }
    # The project's own Clang is the release make_pc.ps1 installs (the one
    # scripts\ci\tools.json names), whatever other LLVM release is in tools\;
    # another one only when that is not there.
    $llvm = $null
    $pinned = Get-PinnedToolVersion $root "clang-msvc"
    if ($pinned) { $llvm = Get-ChildItem -LiteralPath "$root\tools" -Directory -Filter "clang+llvm-$pinned-*" -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if (-not $llvm) { $llvm = Get-ChildItem -LiteralPath "$root\tools" -Directory -Filter "clang+llvm-*" -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if ($bin) { $paths += $bin }
    elseif ($llvm) { $paths += "$($llvm.FullName)\bin" }
    # The compilers CMake is given by full path, so that another Clang on the path
    # (from some other toolchain) is never picked up instead.
    $env:MCLA_CC = if ($bin) { "$bin\clang.exe" } elseif ($llvm) { "$($llvm.FullName)\bin\clang.exe" } else { "" }
    $env:MCLA_CXX = if ($bin) { "$bin\clang++.exe" } elseif ($llvm) { "$($llvm.FullName)\bin\clang++.exe" } else { "" }
    # The Microsoft C++ libraries and headers Clang builds against. Left to itself
    # Clang takes the newest Visual Studio, even one without the C++ tools (a
    # Visual Studio 2026 installed for something else), and then cannot link
    # (issue #21). Point it at an install that has them: Visual Studio 2022 17.1
    # or newer first, the version this is tested with, else a newer one. A value
    # already set (a Developer PowerShell) is kept. Not with mingw, which does not
    # use them.
    if (-not ($env:VCToolsInstallDir -and (Test-Path -LiteralPath $env:VCToolsInstallDir))) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $vcTools = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
        if (Test-Path -LiteralPath $vswhere) {
            $vs = & $vswhere -products * -requires $vcTools -version "[17.1,18.0)" -latest -property installationPath
            if (-not $vs) { $vs = & $vswhere -products * -requires $vcTools -version "[17.1," -latest -property installationPath }
            $versionFile = "$vs\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
            if ($vs -and (Test-Path -LiteralPath $versionFile)) {
                $tools = "$vs\VC\Tools\MSVC\$((Get-Content -LiteralPath $versionFile -TotalCount 1).Trim())\"
                if (Test-Path -LiteralPath $tools) { $env:VCToolsInstallDir = $tools }
            }
        }
    }
    $env:MCLA_MINGW_ROOT = ""
}
$cmakeBin = FindTool "cmake*" "cmake.exe" "bin"
if ($cmakeBin) { $paths += $cmakeBin }
$ninjaBin = FindTool "ninja*" "ninja.exe" ""
if (-not $ninjaBin) {
    $ninja = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($ninja) { $ninjaBin = $ninja.DirectoryName }
}
if ($ninjaBin) { $paths += $ninjaBin }
# MinGit (a setup-wizard install has no git on its PATH): only from -ToolsDir, like the others.
$gitBin = if ($ToolsDir) { FindTool "mingit*" "git.exe" "cmd" } else { "" }
if ($gitBin) { $paths += $gitBin }
$paths += "C:\Program Files\CMake\bin"
$env:PATH = ($paths -join ";") + ";" + $env:PATH
$env:REXSDK = if ($Sdk) { $Sdk }
    elseif ($Toolchain -eq "mingw") { "$root\third_party\rexglue-sdk\out\install\win-amd64-mingw" }
    else { "$root\third_party\rexglue-sdk\out\install\win-amd64" }
