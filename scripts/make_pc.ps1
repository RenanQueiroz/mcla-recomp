# One command from your own disc image to the game running on Windows.
#
#   .\scripts\make_pc.ps1 -Iso C:\path\to\your.iso
#
# Needs Git, CMake, Ninja, Python 3, Visual Studio Build Tools 2022 (C++
# workload) and Clang 20. Whatever is missing, the script offers to install
# (winget, and Clang from the LLVM project's releases); README.md
# lists them for doing it by hand.
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
param([string]$Iso = "", [switch]$Play, [switch]$InstallTools, [string]$Preset = "win-amd64-release")

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$sdk = "$root\third_party\rexglue-sdk"
$sdkTag = "v0.10.0"
$sdkCommit = "f5337cdc947ff6d4c4196737e2c807a48f2a1fc2"
$install = "$sdk\out\install\win-amd64"
$logs = "$root\out\logs"
New-Item -ItemType Directory -Force $logs | Out-Null

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" }
function Fail([string]$text) { Write-Host ""; Write-Host "FAILED: $text" -ForegroundColor Red; exit 1 }
# Run a command with its output in a log; on failure show the end of the log.
function Logged([string]$name, [scriptblock]$command) {
    $log = "$logs\$name.log"
    # Windows PowerShell 5.1 turns a program's redirected stderr into errors,
    # which "Stop" would end the script on; the exit code is what counts.
    $ErrorActionPreference = "Continue"
    & $command 2>&1 | ForEach-Object { "$_" } > $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 25 | ForEach-Object { Write-Host $_ }
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
function PatchHash([string]$patch) { (Get-FileHash -Algorithm SHA256 $patch).Hash.ToLowerInvariant() }
function RecordPatch([string]$patch) {
    $leaf = Split-Path $patch -Leaf
    $lines = @()
    if (Test-Path "$sdk\.mcla-patches") {
        $lines = @(Get-Content "$sdk\.mcla-patches" | Where-Object { -not $_.EndsWith(" $leaf") })
    }
    $lines += "$(PatchHash $patch) $leaf"
    Set-Content -Force -Encoding Ascii "$sdk\.mcla-patches" $lines
}
function CheckPatch([string]$patch) {
    $leaf = Split-Path $patch -Leaf
    $record = $null
    if (Test-Path "$sdk\.mcla-patches") {
        $record = Get-Content "$sdk\.mcla-patches" | Where-Object { $_.EndsWith(" $leaf") } | Select-Object -First 1
    }
    $again = "Patch the SDK afresh: launcher\build-game.ps1 -ResetSdk, or delete third_party\rexglue-sdk, and run this again."
    if (-not $record) {
        Fail "$leaf is in the SDK without a record in third_party\rexglue-sdk\.mcla-patches: it was patched by an older make_pc.ps1 and may hold an older version of it. $again"
    }
    if ($record.Split(" ")[0] -ne (PatchHash $patch)) {
        Fail "patches\$leaf has changed since it was applied to the SDK. $again"
    }
}

Push-Location $root
try {
    # -----------------------------------------------------------------------
    Step "1/6 tools"
    # What the build needs, how to tell that it is there, and how to get it.
    # Programs come from winget (part of Windows 10 and 11); Clang is the
    # portable build from the LLVM project, unpacked into tools\.
    $llvmVersion = "20.1.8"
    $llvmName = "clang+llvm-$llvmVersion-x86_64-pc-windows-msvc"
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    function HasBuildTools {
        if (-not (Test-Path $vswhere)) { return $false }
        [bool](& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    }
    function RefreshPath {
        $env:PATH = [Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" +
                    [Environment]::GetEnvironmentVariable("PATH", "User")
        . "$PSScriptRoot\env.ps1"
    }
    function MissingTools {
        RefreshPath
        $list = @()
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $list += "Git" }
        if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $list += "CMake" }
        if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { $list += "Ninja" }
        # The Microsoft Store's placeholder python.exe is not Python.
        $python = Get-Command python -ErrorAction SilentlyContinue
        if (-not $python -or $python.Source -like "*WindowsApps*") { $list += "Python" }
        if (-not (HasBuildTools)) { $list += "Visual Studio Build Tools" }
        # Only the project's own Clang counts: another one on the path may not
        # find the Microsoft headers (issue #9).
        if (-not (Test-Path "$root\tools\$llvmName\bin\clang.exe")) { $list += "Clang" }
        # Optional: FSR 3.1's Vulkan runtime is built only with it.
        if (-not (HasVulkanSdk)) { $list += "Vulkan SDK" }
        $list
    }
    # The Vulkan SDK (its headers and loader library): VULKAN_SDK, as its
    # installer sets it for the machine, or its usual folder.
    function HasVulkanSdk {
        $machine = [Environment]::GetEnvironmentVariable("VULKAN_SDK", "Machine")
        if (-not $env:VULKAN_SDK -and $machine) { $env:VULKAN_SDK = $machine }
        if ($env:VULKAN_SDK -and (Test-Path "$env:VULKAN_SDK\Include\vulkan\vulkan.h")) { return $true }
        [bool](Get-ChildItem "C:\VulkanSDK\*\Include\vulkan\vulkan.h" -ErrorAction SilentlyContinue)
    }
    $missing = @(MissingTools)
    $required = @($missing | Where-Object { $_ -ne "Vulkan SDK" })
    if ($missing -and -not $required -and -not $InstallTools) {
        # Only the Vulkan SDK: the game builds without it, FSR then for
        # Direct3D 12 only. Asked only when someone can answer (the
        # launcher runs this with -NonInteractive).
        $nonInteractive = [bool]([Environment]::GetCommandLineArgs() | Where-Object { $_ -like "-NonI*" })
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
        if ($required -and -not $InstallTools) {
            $answer = Read-Host "Install them now? Programs come from winget, Clang from the LLVM project's releases; the Build Tools are several GB and ask for administrator rights, and so does the Vulkan SDK. [y/N]"
            if ($answer -notmatch '^[yY]') { Fail "install them (see 'Windows' in README.md) and run this again, or run with -InstallTools" }
        }
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is not available; install the tools by hand (README.md, 'Windows')" }
        $ids = @{ "Git" = "Git.Git"; "CMake" = "Kitware.CMake"; "Ninja" = "Ninja-build.Ninja"; "Python" = "Python.Python.3.12" }
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
            } elseif ($name -eq "Clang") {
                "downloading Clang $llvmVersion (about 900 MB) into tools\"
                New-Item -ItemType Directory -Force "$root\tools" | Out-Null
                $archive = "$root\tools\$llvmName.tar.xz"
                Logged "install-clang" {
                    curl.exe -L --fail -o $archive "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvmVersion/$llvmName.tar.xz"
                    if ($LASTEXITCODE -eq 0) { tar.exe -xf $archive -C "$root\tools" }
                }
                Remove-Item -LiteralPath $archive -ErrorAction SilentlyContinue
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
    "clang: $env:MCLA_CC ($((& $env:MCLA_CC --version | Select-Object -First 1)))"

    # -----------------------------------------------------------------------
    Step "2/6 ReXGlue SDK $sdkTag with this project's patches"
    if (-not (Test-Path "$sdk\.mcla-patched")) {
        if (-not (Test-Path "$sdk\.git")) {
            Logged "sdk-clone" { git clone --recursive --branch $sdkTag https://github.com/rexglue/rexglue-sdk.git $sdk }
        }
        if ((git -C $sdk rev-parse HEAD) -ne $sdkCommit) { Fail "the SDK checkout is not $sdkTag ($sdkCommit)" }
        Patch "sdk-patch" $sdk "$root\patches\rexglue-v0.10.0-mcla.patch"
        Patch "sdk-patch-ffmpeg" "$sdk\thirdparty\FFmpeg" "$root\patches\rexglue-ffmpeg-ps5-config.patch"
        # Git on Windows checks out symbolic links as small text files holding
        # the link's target; one of the SDK's libraries has some. Replace them
        # with copies of what they point to. With Developer Mode on (or as an
        # administrator) Git makes real links instead, which need nothing:
        # they are skipped, as reading one gives the target's contents.
        $links = "$sdk\thirdparty\libmspack\cabextract\mspack"
        Get-ChildItem $links -File | Where-Object { $_.Length -lt 200 -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | ForEach-Object {
            $target = Join-Path $links (Get-Content $_.FullName -Raw).Trim()
            if (Test-Path $target) { Copy-Item $target $_.FullName -Force }
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
    if (-not (Select-String -Quiet -SimpleMatch "kSysFlag_MinMaxColor0TimesSrcAlpha" "$sdk\include\rex\graphics\pipeline\shader\spirv_translator.h")) {
        Patch "sdk-patch-reflection" $sdk "$root\patches\rexglue-minmax-blend-factor.patch"
        (Get-Item "$sdk\.mcla-patched" -Force).LastWriteTime = Get-Date
    } else {
        CheckPatch "$root\patches\rexglue-minmax-blend-factor.patch"
    }
    # Later patches to the SDK (patches\README.md), applied in the order they
    # were made, as several change neighbouring lines. Each goes onto an SDK
    # patched before it existed, found by a name it adds, and the marker's new
    # time has step 3 build the SDK again. One that is in already must be the
    # version recorded (CheckPatch).
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
    )
    foreach ($later in $laterPatches) {
        # A patch that adds its file is not in while the file isn't there.
        if (-not (Test-Path "$sdk\$($later.File)") -or
            -not (Select-String -Quiet -SimpleMatch $later.Adds "$sdk\$($later.File)")) {
            Patch "sdk-patch-$($later.Patch -replace '^rexglue-|\.patch$')" $sdk "$root\patches\$($later.Patch)"
            (Get-Item "$sdk\.mcla-patched" -Force).LastWriteTime = Get-Date
        } else {
            CheckPatch "$root\patches\$($later.Patch)"
        }
    }

    # -----------------------------------------------------------------------
    Step "3/6 build and install the SDK (the long step, once)"
    # -Force on both: a hidden file (the marker often is one) is invisible to
    # Get-Item otherwise, which would end the script under "Stop".
    $marker = Get-Item "$sdk\.mcla-patched" -Force
    $recompiler = Get-Item "$install\bin\rexglue.exe" -Force -ErrorAction SilentlyContinue
    # FSR's runtime missing (an earlier build without the network, or the
    # Vulkan SDK installed since): configured and built again, which fetches
    # and builds it if it can (the rest of the SDK is up to date).
    $fsrMissing = -not (Test-Path "$install\bin\amd_fidelityfx_dx12.dll") -or
        ((HasVulkanSdk) -and -not (Test-Path "$install\bin\amd_fidelityfx_vk.dll"))
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
        Push-Location $sdk
        try {
            # The compilers by full path; a build folder configured earlier
            # with another Clang is then configured again from scratch.
            Logged "sdk-configure" { cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FSR=ON "-DCMAKE_C_COMPILER=$env:MCLA_CC" "-DCMAKE_CXX_COMPILER=$env:MCLA_CXX" }
            Logged "sdk-build" { cmake --build out\build\win-amd64 --config Release }
            Logged "sdk-install" { cmake --install out\build\win-amd64 --config Release }
        } finally { Pop-Location }
    } else {
        "already installed: $install"
    }
    if (-not (Test-Path "$install\bin\rexglue.exe")) { Fail "the SDK was not installed" }
    # FSR is optional: say plainly when it was left out, and what brings it back.
    if (-not (Test-Path "$install\bin\amd_fidelityfx_dx12.dll")) {
        "note: the FSR anti-aliasing option was not built this time: fetching the FidelityFX SDK needs the internet, or its build failed (out\logs\sdk-configure.log, sdk-build.log). The game offers MSAA and off; the next build (or Update) tries again."
    } elseif (-not (Test-Path "$install\bin\amd_fidelityfx_vk.dll")) {
        if (HasVulkanSdk) {
            "note: FSR was built for Direct3D 12 only: its Vulkan runtime failed to build (out\logs\sdk-build.log). With Vulkan the game offers MSAA and off."
        } else {
            "note: FSR was built for Direct3D 12 only: its Vulkan runtime needs the Vulkan SDK (run with -InstallTools, or install it from vulkan.lunarg.com, then build again)."
        }
    }

    # -----------------------------------------------------------------------
    Step "4/6 your game: extract from the disc image"
    if (-not (Test-Path "$root\game\default.xex")) {
        if (-not $Iso) { Fail "game\ is empty: give your disc image with -Iso" }
        if (-not (Test-Path -LiteralPath $Iso)) { Fail "no such file: $Iso" }
        Logged "extract" { python "$PSScriptRoot\extract_game.py" $Iso "$root\game" }
    } else {
        "already extracted: $root\game"
    }
    if (-not (Test-Path "$root\game\default.xex")) { Fail "game\default.xex is missing after extraction" }

    # -----------------------------------------------------------------------
    Step "5/6 recompile the game's code and build it (several minutes the first time)"
    & "$PSScriptRoot\build.ps1" -Preset $Preset
    $build = "$root\out\build\$Preset"
    if (-not (Test-Path "$build\mcla.exe")) { Fail "mcla.exe was not built (log: $root\out\build.log)" }
    Copy-Item "$install\bin\rexgpu-xenos.dll" $build -Force
    # FSR 3.1's runtime (MIT, THIRD-PARTY-NOTICES.md), which the plugin loads.
    Get-ChildItem "$install\bin" -Filter "amd_fidelityfx_*.dll" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $build -Force }

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
