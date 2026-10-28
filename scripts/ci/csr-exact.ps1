# Compiles and runs csr-exact\csr_exact.cpp against a ReXGlue SDK built with llvm-mingw: the host-side exactness test of
# the codegen options csr_mode_free_ops and csr_cheap_switch (patches\rexglue-codegen-csr-lazy.patch). It needs no game and
# runs on any x86-64 CPU; it takes about 20 seconds with the default scale (4 threads), a few minutes with -Exhaustive.
#
#   powershell -File scripts\ci\csr-exact.ps1 -SdkDir <SDK install folder> [-Exhaustive] [-Scale N] [-Threads T] [-Quick]
#                                              [-Work <folder>] [-ToolsDir <folder with llvm-mingw>]
# Exit code 0: every check passed (see the program's header for what is checked).
param([Parameter(Mandatory)][string]$SdkDir, [switch]$Exhaustive, [int]$Scale = 1, [int]$Threads = 4, [switch]$Quick,
      [string]$Work = "", [string]$ToolsDir = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$envArgs = @{ Toolchain = "mingw" }
if ($ToolsDir) { $envArgs["ToolsDir"] = $ToolsDir }
. "$repo\scripts\env.ps1" @envArgs
if (-not $env:MCLA_CXX -or -not (Test-Path -LiteralPath $env:MCLA_CXX)) { throw "no llvm-mingw (scripts\make_pc.ps1 -Toolchain mingw -InstallTools installs it)" }
$include = Join-Path $SdkDir "include"
if (-not (Test-Path -LiteralPath "$include\rex\ppc\context.h")) { throw "$SdkDir is not an SDK install (include\rex\ppc\context.h is missing)" }
if (-not (Select-String -LiteralPath "$include\rex\ppc\context.h" -Pattern "enableFlushModeCheckedFast" -Quiet)) {
    throw "$include\rex\ppc\context.h has no enableFlushModeCheckedFast: this SDK was built without rexglue-codegen-csr-lazy.patch"
}
if (-not $Work) { $Work = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }) "csr-exact" }
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }  # pscheck: ignore PS-RM-RECURSE the work folder is our own, under TEMP, with no links in it
New-Item -ItemType Directory -Force $Work | Out-Null

$exe = "$Work\csr_exact.exe"
# (quoted: PowerShell 5.1 splits an unquoted -msse4.1 into -msse4 and .1)
& $env:MCLA_CXX "-std=c++23" "-O3" "-msse4.1" "-Wall" "-Wextra" "-Wno-unused-parameter" "-I$include" "$PSScriptRoot\csr-exact\csr_exact.cpp" "-o" $exe
if ($LASTEXITCODE -ne 0) { throw "csr_exact.cpp does not compile" }
foreach ($dll in "libc++.dll", "libunwind.dll") {
    Copy-Item -LiteralPath "$env:MCLA_MINGW_ROOT\x86_64-w64-mingw32\bin\$dll" -Destination $Work -Force
}
$testArgs = @("--scale", "$Scale", "--threads", "$Threads")
if ($Exhaustive) { $testArgs += "--exhaustive" }
if ($Quick) { $testArgs += "--quick" }
& $exe @testArgs
$code = $LASTEXITCODE
if ($code -ne 0) { throw "csr_exact failed (exit $code)" }
