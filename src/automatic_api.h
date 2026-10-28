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
// AMD's adapters (0: not asked, or Direct3D 12 had no answer).
struct AdapterFacts {
  uint32_t vendor = 0;               // DXGI_ADAPTER_DESC1::VendorId
  bool uma = false;                  // D3D12_FEATURE_DATA_ARCHITECTURE::UMA: shares the CPU's memory
  uint32_t wave_lane_count_min = 0;  // D3D12_FEATURE_DATA_D3D12_OPTIONS1::WaveLaneCountMin
};

// Whether Automatic takes Vulkan for this GPU (the caller also needs a Vulkan driver and a
// Vulkan device that is this GPU). Unknown facts mean Direct3D 12.
constexpr bool PrefersVulkan(const AdapterFacts& adapter) {
  return adapter.vendor == kVendorIntel ||
         (adapter.vendor == kVendorAmd && adapter.uma &&
          adapter.wave_lane_count_min == kRdnaWaveLaneCountMin);
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
