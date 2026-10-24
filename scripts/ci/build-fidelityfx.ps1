# Builds FSR's two runtimes (amd_fidelityfx_dx12.dll, amd_fidelityfx_vk.dll) from the
# FidelityFX SDK's source, with the msvc toolchain and the C runtime linked in, so that a
# player's PC needs no Visual C++ Redistributable for them.
#
#   powershell -File scripts\ci\build-fidelityfx.ps1 -Out <folder> [-Work <short folder>] [-ToolsDir <folder>] [-DynamicCrt]
#
# What it needs: the patched ReXGlue SDK next to this script's repository
# (scripts\make_pc.ps1 -Toolchain msvc -Until sdk-source makes it, and finds or installs the
# project's Clang and the Visual C++ build tools), and the Vulkan SDK (VULKAN_SDK;
# scripts\ci\install-vulkan-sdk.ps1).
#
# The same sources, pins, wrapper and arguments as the SDK's own build of the runtimes
# (cmake\rexglue_fsr.cmake builds them as an ExternalProject, which gives them the dynamic
# C runtime and, being a part of the whole SDK build, the whole SDK's build time): the commit
# and the tree hashes it pins are read from that file, the source is fetched and checked as
# it does, and the wrapper project (cmake\fidelityfx) is configured with its arguments, plus
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded. The result is checked: with the static runtime,
# the DLLs may import no Visual C++ or UCRT DLL. -DynamicCrt builds the SDK's own flavour.
param([Parameter(Mandatory)][string]$Out, [string]$Work = "", [string]$ToolsDir = "", [switch]$DynamicCrt)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$sdkRoot = "$repoRoot\third_party\rexglue-sdk"
$env:GIT_CONFIG_COUNT = "1"; $env:GIT_CONFIG_KEY_0 = "core.longpaths"; $env:GIT_CONFIG_VALUE_0 = "true"
if (-not (Test-Path -LiteralPath "$sdkRoot\cmake\fidelityfx\CMakeLists.txt")) {
    throw "no patched SDK at $sdkRoot (run scripts\make_pc.ps1 -Toolchain msvc -Until sdk-source first)"
}
if (-not $env:VULKAN_SDK -or -not (Test-Path -LiteralPath "$env:VULKAN_SDK\Include\vulkan\vulkan.h")) {
    throw "VULKAN_SDK is not set to a Vulkan SDK (scripts\ci\install-vulkan-sdk.ps1)"
}
if ($ToolsDir) { . "$PSScriptRoot\..\env.ps1" -Toolchain msvc -ToolsDir $ToolsDir } else { . "$PSScriptRoot\..\env.ps1" -Toolchain msvc }
if (-not $env:MCLA_CC -or -not (Test-Path -LiteralPath $env:MCLA_CC)) { throw "no msvc-target Clang (scripts\make_pc.ps1 -Toolchain msvc installs it)" }
$cc = $env:MCLA_CC; $cxx = $env:MCLA_CXX
$readobj = Join-Path (Split-Path $cc -Parent) "llvm-readobj.exe"
"compiler: $cc ($((& $cc --version | Select-Object -First 1)))"

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).ProviderPath
if (-not $Work) { $Work = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }) "ffx" }
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).ProviderPath
# FidelityFX's shader compiler fails on paths over 260 characters; its longest source path is 181
# below its folder.
if ($Work.Length -gt 60) { throw "the work folder's path is $($Work.Length) characters long; FidelityFX's shader compiler needs 75 or fewer for its folder: give a shorter -Work" }

function Invoke-Git { & git.exe @args; if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed ($LASTEXITCODE)" } }

# --- the FidelityFX source, as the SDK fetches and checks it
$fsr = Get-Content -LiteralPath "$sdkRoot\cmake\rexglue_fsr.cmake" -Raw
function Pin([string]$pattern) {
    $m = [regex]::Match($fsr, $pattern)
    if (-not $m.Success) { throw "cmake\rexglue_fsr.cmake no longer has $pattern" }
    $m.Groups[1].Value
}
$commit = Pin 'set\(_rexglue_fsr_commit "([0-9a-f]{40})"\)'
$treeApi = Pin 'set\(_rexglue_fsr_tree_ffx_api "([0-9a-f]{40})"\)'
$treeSdk = Pin 'set\(_rexglue_fsr_tree_sdk "([0-9a-f]{40})"\)'
$repository = Pin 'set\(REXGLUE_FSR_REPOSITORY "(https://[^"]+)"'
"FidelityFX $commit from $repository"
$ffx = "$Work\ffx"
New-Item -ItemType Directory -Force $ffx | Out-Null
Invoke-Git -C $ffx init -q
Invoke-Git -C $ffx config core.longpaths true
Invoke-Git -C $ffx config core.autocrlf false
Invoke-Git -C $ffx remote add origin $repository
Invoke-Git -C $ffx sparse-checkout set --no-cone /ffx-api/ /sdk/ /LICENSE.txt
Invoke-Git -C $ffx fetch -q --depth 1 --filter=blob:none origin $commit
Invoke-Git -C $ffx checkout -q FETCH_HEAD
if ((& git -C $ffx rev-parse HEAD) -ne $commit) { throw "the fetched FidelityFX source is not $commit" }
$trees = @(& git -C $ffx rev-parse "HEAD:ffx-api" "HEAD:sdk")
if ($trees[0] -ne $treeApi -or $trees[1] -ne $treeSdk) { throw "the fetched FidelityFX source's tree hashes ($($trees -join ', ')) are not the pinned ones" }
& git -C $ffx diff --quiet HEAD -- ffx-api sdk LICENSE.txt
if ($LASTEXITCODE -ne 0) { throw "the fetched FidelityFX source has modified files" }
"verified: ffx-api $treeApi, sdk $treeSdk"
Copy-Item -LiteralPath "$ffx\LICENSE.txt" -Destination "$Out\LICENSE-FidelityFX.txt" -Force

# --- the two runtimes
$ninja = (Get-Command ninja -ErrorAction Stop).Source
$crt = if ($DynamicCrt) { @() } else { @("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW") }
$banned = '^(msvcp\d+(_\d+)?|vcruntime\d+(_\d+)?|concrt\d+|ucrtbase|api-ms-win-crt-.*)\.dll$'
$results = @()
foreach ($api in "dx12", "vk") {
    $backend = if ($api -eq "dx12") { "DX12_X64" } else { "VK_X64" }
    $target = "amd_fidelityfx_$api"
    # Its own copy of the wrapper: FidelityFX writes static libraries under the top project's folder.
    $wrapper = "$Work\wrapper-$api"
    Copy-Item -LiteralPath "$sdkRoot\cmake\fidelityfx" -Destination $wrapper -Recurse
    $build = "$Work\build-$api"
    "==> $target ($(if ($DynamicCrt) { 'dynamic' } else { 'static' }) C runtime)"
    & cmake -S $wrapper -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_CXX_COMPILER=$cxx" "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DFFX_ROOT=$($ffx.Replace('\', '/'))" "-DFFX_API_BACKEND=$backend" "-DREXGLUE_FSR_WRAPPER_HASH=ci" `
        "-DREXGLUE_FSR_SHADERS_DIR=" "-DREXGLUE_FSR_SHADERS_HASH=" "-DREXGLUE_FSR_VULKAN_HEADERS=" "-DREXGLUE_FSR_VULKAN_LIBRARY=" `
        "-DREXGLUE_FSR_STATIC=OFF" "-DREXGLUE_FSR_ARCHIVE_DIR=" @crt
    if ($LASTEXITCODE -ne 0) { throw "configure of $target failed" }
    & cmake --build $build --target $target
    if ($LASTEXITCODE -ne 0) { throw "build of $target failed" }
    $dll = "$build\bin\$target.dll"
    if (-not (Test-Path -LiteralPath $dll)) { throw "$dll was not built" }

    $imports = @(& $readobj --coff-imports $dll | ForEach-Object { if ($_ -match '(?i)^\s*Name:\s+(\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } } | Sort-Object -Unique)
    "$target.dll: $([int]((Get-Item -LiteralPath $dll).Length / 1KB)) KB, imports $($imports -join ', ')"
    $crtImports = @($imports | Where-Object { $_ -match $banned })
    if (-not $DynamicCrt -and $crtImports) { throw "$target.dll still imports the C runtime ($($crtImports -join ', ')): the static runtime did not take" }
    if ($DynamicCrt -and -not $crtImports) { "note: the dynamic runtime build imports no C runtime DLL either" }
    Copy-Item -LiteralPath $dll -Destination $Out -Force
    $results += [ordered]@{
        name = "$target.dll"
        size = (Get-Item -LiteralPath $dll).Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash.ToLowerInvariant()
        imports = $imports
    }
}

# What these are, for the bundle and release.json to carry.
$info = [ordered]@{
    format = 1
    fidelityFxCommit = $commit
    repository = $repository
    staticCrt = (-not $DynamicCrt)
    compiler = (& $cc --version | Select-Object -First 1)
    dlls = $results
}
Set-Content -LiteralPath "$Out\fidelityfx-build.json" -Value (ConvertTo-Json -InputObject $info -Depth 5) -Encoding Ascii

"FidelityFX DLLs in $Out"
Get-ChildItem -LiteralPath $Out | ForEach-Object { "  $($_.Name)  $($_.Length)" }
