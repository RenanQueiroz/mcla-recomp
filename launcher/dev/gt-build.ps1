# Builds the GPU-timestamp instrumented GPU plugin (rexgpu-xenos.dll) from this checkout's patched SDK (fork-only
# development tool; the instrumentation is launcher\dev\gpu-timestamps*.sdk.patch, texload-split-..., resolve-split-...
# and frame-log.sdk.patch, and gt-phases.py reads what it writes).
#
#   .\launcher\dev\gt-build.ps1 [-OutDir <folder>] [-Toolchain mingw|msvc] [-Layer name,name,...] [-Reverse] [-NoBuild]
#
# Needs the SDK built once by scripts\make_pc.ps1 -Until sdk (or a normal build) with the same toolchain: the layer
# is applied to third_party\rexglue-sdk's sources (git apply, in order, recorded in third_party\rexglue-sdk\.gt-layer),
# only the GPU plugin target is rebuilt (incremental: a few minutes), and the DLL is copied to -OutDir
# (default out\gt) as rexgpu-xenos.dll, to put next to a normal mcla.exe and its DLLs of the same build.
# The layer only touches the GPU plugin (src\graphics\...), so the game and rexruntime.dll are not rebuilt.
#   -Reverse   takes the layer out again (git apply -R, last patch first) and rebuilds nothing; run scripts\make_pc.ps1
#              or build the target again for a clean plugin
#   -NoBuild   applies (or reverses) the patches only
#
# (The SDK folder is $sdkDir, not $sdk: scripts\env.ps1, dot-sourced below, has a -Sdk parameter, and PowerShell variable names are
# not case-sensitive, so a $sdk of this script was emptied by it.)
# Run under the shared lock (lockrun.py --lock): it is a compile. The instrumentation (all cvars and environment
# variables are read at run time; nothing changes without them):
#   MCLA_GT_EVERY=<s>, MCLA_GT_DIR=<folder>   one timed frame every <s> seconds as gt_<n>.csv (D3D12 and Vulkan)
#   --tmp_frame_log_dir=<folder> --tmp_frame_log_frames=<n> --tmp_frame_log_quiet=true|false (D3D12), and the pad's
#   `set tmp_frame_log <label>` starts <n> frames logged as <label>_<k>_gt.csv (and <label>_<k>.txt unless quiet)
#   --mcla_render_markers_all_phases=true     the game's phase markers in the log
#   --d3d12_threaded_submission_split_draws=0 one submission per frame at 1x and 2x (no mid-frame gaps)
param([string]$OutDir = "", [ValidateSet("mingw", "msvc")][string]$Toolchain = "mingw", [string[]]$Layer = @(),
      [switch]$Reverse, [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$sdkDir = "$root\third_party\rexglue-sdk"
if (-not (Test-Path -LiteralPath "$sdkDir\CMakeLists.txt")) { throw "no SDK at ${sdkDir}: run scripts\make_pc.ps1 -Until sdk first" }
if (-not $OutDir) { $OutDir = "$root\out\gt" }
if ($Layer.Count -eq 0) {
    $Layer = @("gpu-timestamps", "gpu-timestamps-vulkan", "texload-split-timestamps", "resolve-split-timestamps", "frame-log")
}
$env:GIT_CONFIG_COUNT = "1"; $env:GIT_CONFIG_KEY_0 = "core.longpaths"; $env:GIT_CONFIG_VALUE_0 = "true"
$recordFile = "$sdkDir\.gt-layer"
$applied = @(if (Test-Path -LiteralPath $recordFile) { Get-Content -LiteralPath $recordFile } else { })

function GitApply([string[]]$gitArgs, [string]$patch) {
    # git says why a check failed on stderr, which "Stop" would turn into an error: the exit code is what counts.
    $ErrorActionPreference = "Continue"
    & git -C $sdkDir apply --whitespace=nowarn @gitArgs $patch 2>&1 | Out-Host
    return ($LASTEXITCODE -eq 0)
}

if ($Reverse) {
    foreach ($name in @($applied)[($applied.Count - 1)..0]) {
        if (-not $name) { continue }
        $patch = "$root\launcher\dev\$name.sdk.patch"
        if (-not (GitApply @("-R") $patch)) { throw "could not take $name out (the sources changed since it was applied?)" }
        "reversed $name"
    }
    Remove-Item -LiteralPath $recordFile -ErrorAction SilentlyContinue
    exit 0
}

if ($applied.Count -gt 0) {
    # Recorded as applied: make sure the layer is still in (the sources may have been reset). The layers overlap (a later one's
    # context holds the lines of an earlier one), so only the last one applied reverses cleanly on its own.
    $lastName = $applied[$applied.Count - 1]
    if (-not (GitApply @("--reverse", "--check") "$root\launcher\dev\$lastName.sdk.patch")) {
        throw "$lastName is recorded as applied but does not reverse cleanly: reset the SDK sources (make_pc.ps1 re-patches a clean clone) and run this again"
    }
}
foreach ($name in $Layer) {
    $patch = "$root\launcher\dev\$name.sdk.patch"
    if (-not (Test-Path -LiteralPath $patch)) { throw "no patch $patch" }
    if ($applied -contains $name) { "already applied: $name"; continue }
    if (-not (GitApply @("--check") $patch)) {
        throw "$name does not apply to the SDK's sources (rebase it: launcher\dev\gt-phases.py's header lists what each patch needs)"
    }
    if (-not (GitApply @() $patch)) { throw "git apply $name failed" }
    $applied += $name
    Set-Content -LiteralPath $recordFile -Value $applied
    "applied $name"
}
if ($NoBuild) { exit 0 }

# The toolchain on PATH, as the build scripts do it.
. "$root\scripts\env.ps1" -Toolchain $Toolchain
if (-not $env:VULKAN_SDK) {
    $v = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
        Sort-Object { [version]$_.Name } | Select-Object -Last 1
    if ($v) { $env:VULKAN_SDK = $v.FullName }
}
$buildName = if ($Toolchain -eq "mingw") { "win-amd64-mingw" } else { "win-amd64" }
if (-not (Test-Path -LiteralPath "$sdkDir\out\build\$buildName\build.ninja")) { throw "the SDK is not built with $Toolchain ($sdkDir\out\build\$buildName)" }
$start = Get-Date
Push-Location $sdkDir
try {
    cmake --build "out\build\$buildName" --config Release --target rexgpu-xenos
    if ($LASTEXITCODE -ne 0) { throw "building rexgpu-xenos failed" }
} finally { Pop-Location }
# Where the plugin lands depends on the toolchain's output folder: the newest one this build wrote.
$dll = Get-ChildItem -LiteralPath "$sdkDir\out" -Recurse -Filter rexgpu-xenos.dll -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $start.AddSeconds(-5) } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $dll) { throw "the build wrote no rexgpu-xenos.dll (newer than $start) under $sdkDir\out" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item -LiteralPath $dll.FullName -Destination "$OutDir\rexgpu-xenos.dll" -Force
"instrumented plugin: $OutDir\rexgpu-xenos.dll ($($dll.Length) bytes, from $($dll.FullName); layer: $($applied -join ', '))"
