# Which GPU the game uses on a PC with two (2026-10-27)

Fork issues #1 (an RTX 4060 user, and a second user, at 3 to 6 FPS right after the Rockstar logos) and #15 (a laptop with an i5-7300, a GTX 1050 4 GB and
8 GB of RAM: choppy, with audio hiccups) are likely cases of the game running on the integrated GPU of a laptop with switchable graphics. Neither report came
with a log and no PC here lists an integrated GPU first, so this is the cause the code points to, not one that was seen. `KNOWN-ISSUES.md` ("The game ran on
the integrated graphics on PCs with two GPUs") has the user-facing version; this note has the details and the runs.

## What the code did

- The SDK's Direct3D 12 provider (`src/ui/d3d12/d3d12_provider.cpp`, "Choose the adapter") walks `IDXGIFactory::EnumAdapters1`, skips the adapters Direct3D 12 can't
  make a feature level 11_0 device on, and with `d3d12_adapter` = -1 (the default) stops at the first that isn't software; a number stops at that adapter, -2 at the first
  software one.
- The Vulkan provider (`src/ui/vulkan/vulkan_provider.cpp`) takes `vulkan_device` if it is set, else the first device of the loader's list that works.
- The fork's Automatic graphics API (`DecideAutomaticBackend`, and the launcher's `DisplayDrivers.ReadAutomaticApi`) looked at the same first adapter, so on an Intel +
  NVIDIA laptop it would have chosen Vulkan for the Intel GPU. It set `vulkan_device` only when Automatic itself chose Vulkan.
- On a laptop with switchable graphics Windows lists the GPU wired to the screen first, normally the integrated one, unless the program has an entry in *Settings ›
  System › Display › Graphics*.

## The rule (`src/gpu_choice.h`; the launcher's `DisplayDrivers.ChooseAdapter` is the same code in C#; the two must agree)

In this order:

1. `d3d12_adapter` set by the user (the command line, an environment variable or `mcla.toml`: its source is not the default): the SDK's meaning. A number is that adapter
   in `EnumAdapters1`'s order, -2 the first software adapter, another negative number the first that isn't software.
2. `mcla_prefer_dedicated_gpu` off: the first adapter that isn't software and can make a Direct3D 12 device (the old behaviour).
3. Windows' entry for the exe: the value named by the exe's full path (`GetModuleFileNameW`) in `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`, data a list of
   `name=value;` items, `GpuPreference=` 0 (let Windows decide), 1 (power saving) or 2 (high performance). 1 takes the first usable GPU of
   `IDXGIFactory6::EnumAdapterByGpuPreference(DXGI_GPU_PREFERENCE_MINIMUM_POWER)`, 2 the first of `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`.
4. No entry, 0, or something not understood: the high-performance order's first usable GPU.
5. Nothing usable in the ranking, or no `IDXGIFactory6`: the first adapter that isn't software and can make a device, as the SDK.

The registry's data is not only `GpuPreference`: this PC's key holds `C:\Program Files (x86)\Steam\steamapps\common\F1 25\F1_25.exe` = `AppStatus=0;` (REG_SZ), which
`ParseWindowsPreference` reads as no preference. The exe's own path is the key, as Windows writes it when the program is added by hand in the Graphics settings page.

The chosen adapter's `EnumAdapters1` number becomes `d3d12_adapter`'s default with `rex::cvar::SetDefaultValue` after the GPU plugin is loaded (like `vulkan_device`'s):
the SDK takes exactly that adapter, the value is not written to `mcla.toml`, and an explicit `--d3d12_adapter=N` keeps `EnumAdapters1`'s numbering. Both options are defined
in `rexruntime.dll` (the SDK's `rexui` objects), not in the GPU plugin, so they exist when `mcla.exe` starts and the command line, environment and `mcla.toml` values are
already applied: `GetFlagSource` is exact when the choice is made (`[RUNS PENDING]`).

Whenever the API ends up Vulkan (Automatic's, or `--mcla_gpu_backend=vulkan`) and `vulkan_device` is at its default, `vulkan_device` becomes the Vulkan physical device with
the chosen adapter's LUID (`VkPhysicalDeviceIDProperties.deviceLUID`; else vendor and device id). With `mcla_prefer_dedicated_gpu=false` only Automatic's Vulkan does that, as
before.

The Automatic API (`src/automatic_api.h`) is then decided on the chosen GPU: Intel + NVIDIA laptop → NVIDIA → Direct3D 12; RDNA iGPU + NVIDIA → Direct3D 12; Intel only or
RDNA iGPU only → Vulkan, as before.

`mcla.exe` exports `NvOptimusEnablement = 1` and `AmdPowerXpressRequestHighPerformance = 1` (`extern "C" __declspec(dllexport)`, `src/mcla_app.cpp`). Checked in the
stripped llvm-mingw build with `llvm-objdump -p mcla.exe`: the export table (`DLL name: mcla.exe`) lists ordinal 1 `AmdPowerXpressRequestHighPerformance` and ordinal 2
`NvOptimusEnablement`, and both variables are 1 in the file's `.data`; the build before has no named exports. What the drivers do with them was not tried (this PC has no
switchable graphics).

## The log

[RUNS PENDING: the lines as the game logged them on this PC]

## Runs

- The launcher's choice on this PC (`apitest`, real adapters; `SP\gpu-choice\scripts\apitest-run.ps1`): see KNOWN-ISSUES.md. [RUNS PENDING: the game's own starts]

## Not verified

- A laptop where Windows lists the integrated GPU first: the rule is tried with made-up adapters only (`launcher/dev/api-rule/gpu_choice_test.cpp`, C++, and `apitest`, C#,
  41 choice rows and 18 registry-data rows each, the same table). The runs above are on a desktop where Windows lists the GeForce first and ranks it first for high performance.
- What `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` change on a real Optimus or PowerXpress laptop.
- Windows 10 before 1803 (no `IDXGIFactory6`: the first usable adapter is taken); Windows' per-app entry for a program started from a path that is a link.
- Linux and the PS5: not touched (the option, the registry read and the exports exist in the Windows build only; the PS5 host does not use `mcla_app.cpp`).
- The launcher's stamp check does not notice a switch between two adapters that both stay installed and use the same API (an explicit Direct3D 12 player on a laptop whose
  shader storage was made on the integrated GPU); the API check does notice Automatic's change from Vulkan on the Intel GPU to Direct3D 12 on the NVIDIA one.
