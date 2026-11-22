// The rule of the Automatic graphics API, apart from Windows and the SDK so that
// launcher/dev/api-rule/api_rule_test.cpp can run it with made-up values. src/mcla_app.cpp
// (DecideAutomaticBackend) asks Direct3D 12 for the facts and applies it; the launcher's
// DisplayDrivers.AutomaticApiFor (launcher/src/ShaderStamp.cs) repeats it, and the two must
// agree.
//
// Vulkan on Intel graphics and on AMD's integrated graphics of the RDNA generation; Direct3D 12
// on everything else: NVIDIA, AMD's graphics cards, and AMD's integrated graphics of the GCN
// generation (Vega: the Ryzen 3000G to 5000G APUs), which a user's Ryzen 7 5700G (fork issue
// #14) showed with the HUD, the menus and parts of the city missing on Vulkan. RDNA is told by
// the smallest wave Direct3D 12 reports (D3D12_FEATURE_DATA_D3D12_OPTIONS1.WaveLaneCountMin):
// 32 for RDNA, 64 for GCN. Integrated is Direct3D 12's own answer, the architecture's UMA flag.
//
// The same facts decide one more thing, apart from the API: the SDK's render target path on
// Direct3D 12 for Intel's graphics cards (Arc), PrefersHostRenderTargets below.
#pragma once

#include <cstdint>

namespace mcla::automatic_api {

constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorIntel = 0x8086;
constexpr uint32_t kVendorNvidia = 0x10DE;

// The wave size an RDNA GPU can run at (and GCN cannot: its waves are 64 lanes).
constexpr uint32_t kRdnaWaveLaneCountMin = 32;

// What is known of the adapter the game uses (the one src/gpu_choice.h chooses: on a PC with two
// GPUs not necessarily the first Windows lists). uma and wave_lane_count_min are only asked of
// AMD's and Intel's adapters (0: not asked, or Direct3D 12 had no answer). uma_known tells a
// card (uma false, asked and answered) from an adapter that was not asked.
struct AdapterFacts {
  uint32_t vendor = 0;               // DXGI_ADAPTER_DESC1::VendorId
  bool uma = false;                  // D3D12_FEATURE_DATA_ARCHITECTURE::UMA: shares the CPU's memory
  uint32_t wave_lane_count_min = 0;  // D3D12_FEATURE_DATA_D3D12_OPTIONS1::WaveLaneCountMin
  bool uma_known = false;            // Direct3D 12 answered the UMA question
};

// Whether Automatic takes Vulkan for this GPU (the caller also needs a Vulkan driver and a
// Vulkan device that is this GPU). Unknown facts mean Direct3D 12.
constexpr bool PrefersVulkan(const AdapterFacts& adapter) {
  return adapter.vendor == kVendorIntel ||
         (adapter.vendor == kVendorAmd && adapter.uma &&
          adapter.wave_lane_count_min == kRdnaWaveLaneCountMin);
}

// Intel's graphics cards, with memory of their own (Arc: the Alchemist A-series and the Battlemage
// B-series), as Direct3D 12 tells them from Intel's integrated graphics: UMA asked and answered
// "no". Not asked, or no answer, is not a card.
constexpr bool IsIntelCard(const AdapterFacts& adapter) {
  return adapter.vendor == kVendorIntel && adapter.uma_known && !adapter.uma;
}

// Whether the game makes host render targets ("rtv") the default of the SDK's
// render_target_path_d3d12 for this GPU, where the SDK's own default for every Intel GPU is the
// rasterizer-ordered-view path ("rov"). Intel's cards only: that default was set for a driver bug
// of the integrated graphics in 2021 (the "always" stencil compare of a UHD Graphics 630), which
// Xenia Canary's issue 542 reports gone on Arc and which Xenia Canary left behind for Arc in
// August 2025 (commit 37e3fe9); the path is the one every other GPU uses, and the ROV one is
// slow on a card. Fork issue #92 (an Arc B580): with "rtv" the frame pacing was much steadier
// (the camera no longer jumped). Intel's integrated graphics keep the SDK's default (Xenia
// Canary's issue 542 reports broken pictures on Iris Xe with "rtv"). A value the player sets
// for render_target_path_d3d12 always wins.
constexpr bool PrefersHostRenderTargets(const AdapterFacts& adapter) {
  return IsIntelCard(adapter);
}

// The kind of GPU, for the log line "graphics API: ... (Automatic: <kind> '<name>')".
constexpr const char* KindOf(const AdapterFacts& adapter) {
  if (adapter.vendor == kVendorAmd) {
    if (!adapter.uma) {
      return "AMD discrete graphics";
    }
    return adapter.wave_lane_count_min == kRdnaWaveLaneCountMin
               ? "AMD integrated graphics (RDNA, 32-lane waves)"
           : adapter.wave_lane_count_min == 64
               ? "AMD integrated graphics (GCN or Vega, 64-lane waves)"
               : "AMD integrated graphics (wave size not known)";
  }
  return adapter.vendor == kVendorIntel    ? "Intel graphics"
         : adapter.vendor == kVendorNvidia ? "NVIDIA graphics"
                                           : "graphics";
}

}  // namespace mcla::automatic_api
