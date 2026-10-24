# Compiles the fork's host sources (src\*.cpp) against a ReXGlue SDK built with llvm-mingw, with
# the project's own flags (CMakeLists.txt) and stand-ins for the headers `rexglue codegen` writes
# from the game (host_stub.py). Syntax and types only (-fsyntax-only): the recompiled game code,
# linking and running need the player's game and are not covered.
#
#   powershell -File scripts\ci\host-compile.ps1 -SdkDir <SDK install folder> [-Work <folder>]
param([Parameter(Mandatory)][string]$SdkDir, [string]$Work = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. "$repo\scripts\env.ps1" -Toolchain mingw
if (-not $env:MCLA_CC -or -not (Test-Path -LiteralPath $env:MCLA_CC)) { throw "no llvm-mingw (scripts\make_pc.ps1 -Toolchain mingw -InstallTools installs it)" }
if (-not (Test-Path -LiteralPath "$SdkDir\lib\cmake\rexglue\rexglueConfig.cmake")) { throw "$SdkDir is not an SDK install (lib\cmake\rexglue\rexglueConfig.cmake is missing)" }
$sdkPath = (Resolve-Path -LiteralPath $SdkDir).ProviderPath.Replace("\", "/")
if (-not $Work) { $Work = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }) "host-compile" }
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work | Out-Null
$stub = "$Work\stub".Replace("\", "/")
$build = "$Work\build"

python -I "$repo\scripts\ci\host_stub.py" "$repo\src" $stub
if ($LASTEXITCODE -ne 0) { throw "host_stub.py failed" }
cmake -S $repo -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$sdkPath" "-DCMAKE_C_COMPILER=$env:MCLA_CC" "-DCMAKE_CXX_COMPILER=$env:MCLA_CXX" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_CXX_FLAGS=-I$stub"
if ($LASTEXITCODE -ne 0) { throw "cmake could not configure the game's project against $SdkDir" }
python -I "$repo\scripts\ci\host_check.py" $build -j ([Environment]::ProcessorCount)
if ($LASTEXITCODE -ne 0) { throw "a host source does not compile" }
