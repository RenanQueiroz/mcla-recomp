# Dot-source to put the project's toolchain on PATH:  . .\scripts\env.ps1
$root = Split-Path $PSScriptRoot -Parent
# The Clang make_pc.ps1 installs (keep the version in step with it), else any
# other unpacked LLVM release in tools\.
$llvm = Get-Item "$root\tools\clang+llvm-20.1.8-x86_64-pc-windows-msvc" -ErrorAction SilentlyContinue
if (-not $llvm) { $llvm = Get-ChildItem "$root\tools" -Directory -Filter "clang+llvm-*" -ErrorAction SilentlyContinue | Select-Object -First 1 }
$ninja = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$paths = @()
if ($llvm) { $paths += "$($llvm.FullName)\bin" }
# The compilers CMake is given by full path, so that another Clang on the path
# (from some other toolchain) is never picked up instead.
$env:MCLA_CC = if ($llvm) { "$($llvm.FullName)\bin\clang.exe" } else { "" }
$env:MCLA_CXX = if ($llvm) { "$($llvm.FullName)\bin\clang++.exe" } else { "" }
# The Microsoft C++ libraries and headers Clang builds against. Left to itself
# Clang takes the newest Visual Studio, even one without the C++ tools (a
# Visual Studio 2026 installed for something else), and then cannot link
# (issue #21). Point it at an install that has them: Visual Studio 2022 17.1
# or newer first, the version this is tested with, else a newer one. A value
# already set (a Developer PowerShell) is kept.
if (-not ($env:VCToolsInstallDir -and (Test-Path $env:VCToolsInstallDir))) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vcTools = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -products * -requires $vcTools -version "[17.1,18.0)" -latest -property installationPath
        if (-not $vs) { $vs = & $vswhere -products * -requires $vcTools -version "[17.1," -latest -property installationPath }
        $versionFile = "$vs\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
        if ($vs -and (Test-Path $versionFile)) {
            $tools = "$vs\VC\Tools\MSVC\$((Get-Content $versionFile -TotalCount 1).Trim())\"
            if (Test-Path $tools) { $env:VCToolsInstallDir = $tools }
        }
    }
}
if ($ninja) { $paths += $ninja.DirectoryName }
$paths += "C:\Program Files\CMake\bin"
$env:PATH = ($paths -join ";") + ";" + $env:PATH
$env:REXSDK = "$root\third_party\rexglue-sdk\out\install\win-amd64"
