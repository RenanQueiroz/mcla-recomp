# Regenerate code from the user's XEX and build.  Usage: .\scripts\build.ps1 [-Preset win-amd64-release]
# [-Toolchain msvc|mingw] [-ToolsDir DIR] [-Sdk DIR]: as in make_pc.ps1, which passes them on; the
# toolchain has to be the one the SDK was built with. Without any of them, the options of the
# last build (launcher\build-options.json) are used, msvc when there is no record.
param([string]$Preset = "win-amd64-release", [switch]$NoCodegen,
      [ValidateSet("", "msvc", "mingw")][string]$Toolchain = "", [string]$ToolsDir = "", [string]$Sdk = "")
$envArgs = @{}
foreach ($name in "Toolchain", "ToolsDir", "Sdk") { if ($PSBoundParameters.ContainsKey($name)) { $envArgs[$name] = $PSBoundParameters[$name] } }
. "$PSScriptRoot\env.ps1" @envArgs
# (env.ps1 set $Toolchain, to what was asked for, or the last build's, or msvc)
$root = Split-Path $PSScriptRoot -Parent
if ($Toolchain -ne "msvc") { "toolchain: $Toolchain" }
# Windows PowerShell 5.1 turns a program's redirected stderr into errors, which
# a caller's "Stop" would end the script on; the exit codes are what count.
$ErrorActionPreference = "Continue"
Push-Location $root
try {
    New-Item -ItemType Directory -Force "$root\out" | Out-Null
    if (-not (Test-Path -LiteralPath "$env:REXSDK\bin\rexglue.exe")) {
        throw "no ReXGlue SDK for the $Toolchain toolchain at $env:REXSDK: run scripts\make_pc.ps1 -Toolchain $Toolchain first"
    }
    if (-not $NoCodegen) {
        & "$env:REXSDK\bin\rexglue.exe" codegen mcla_manifest.toml 2>&1 | ForEach-Object { "$_" } > "$root\out\codegen.log"
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath "$root\out\codegen.log" | Select-Object -Last 20; throw "codegen failed" }
        Select-String -Path "$root\out\codegen.log" -Pattern "Codegen summary" | ForEach-Object { $_.Line }
    }
    # Configure when there is no build yet, or when it was configured with
    # another compiler than the project's Clang (CMake then starts afresh).
    $configured = Test-Path -LiteralPath "$root\out\build\$Preset\build.ninja"
    if ($configured -and $env:MCLA_CXX) {
        $found = Get-ChildItem "$root\out\build\$Preset\CMakeFiles" -Recurse -Filter CMakeCXXCompiler.cmake -ErrorAction SilentlyContinue |
            Select-String -Quiet -SimpleMatch "set(CMAKE_CXX_COMPILER `"$($env:MCLA_CXX.Replace('\', '/'))`")"
        $configured = [bool]$found
    }
    # Configured against another SDK than this one (a prebuilt bundle instead of the
    # SDK built here, or the other toolchain's): configured again from scratch, as
    # the cache keeps where find_package found the first one (rexglue_DIR and the
    # libraries' own) whatever CMAKE_PREFIX_PATH says later.
    if ($configured) {
        $cached = Select-String -LiteralPath "$root\out\build\$Preset\CMakeCache.txt" -Pattern '^CMAKE_PREFIX_PATH:[A-Za-z]+=(.*)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
        if ($cached -and $cached.Replace('\', '/').TrimEnd('/') -ne $env:REXSDK.Replace('\', '/').TrimEnd('/')) {
            Remove-Item -LiteralPath "$root\out\build\$Preset\CMakeCache.txt" -Force
            $configured = $false
        }
    }
    if (-not $configured) {
        $compilers = @()
        if ($env:MCLA_CC) { $compilers = @("-DCMAKE_C_COMPILER=$env:MCLA_CC", "-DCMAKE_CXX_COMPILER=$env:MCLA_CXX") }
        cmake --preset $Preset "-DCMAKE_PREFIX_PATH=$env:REXSDK" @compilers 2>&1 | ForEach-Object { "$_" } > "$root\out\configure.log"
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath "$root\out\configure.log" | Select-Object -Last 20; throw "configure failed" }
    }
    cmake --build --preset $Preset 2>&1 | ForEach-Object { "$_" } > "$root\out\build.log"
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Select-String -Path "$root\out\build.log" -Pattern "error:|FAILED:|lld-link" | Select-Object -First 15 | ForEach-Object { $_.Line }
        throw "build failed"
    }
    Get-Content -LiteralPath "$root\out\build.log" | Select-Object -Last 1
} finally { Pop-Location }
