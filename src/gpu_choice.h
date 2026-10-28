// Which GPU the game uses on a PC with more than one, apart from Windows and the SDK so that
// launcher/dev/api-rule/gpu_choice_test.cpp can run it with made-up adapters. src/mcla_app.cpp
// (ChooseGpu) lists the adapters, asks Windows for their order and applies the answer; the
// launcher's DisplayDrivers.ChooseAdapter (launcher/src/ShaderStamp.cs) repeats it, and the two
// must agree. Windows only: Linux and the PS5 do not use it.
//
// Why: the SDK's Direct3D 12 provider takes the FIRST adapter DXGI lists that isn't software
// (d3d12_adapter = -1), and the Vulkan provider the loader's first device. On a laptop with
// integrated and dedicated graphics Windows often lists the integrated GPU first (it drives the
// screen), so the game ran on the weaker one (fork issues #1 and #15, likely cases, not confirmed).
// The rule, in this order:
//  1. d3d12_adapter set by the user (the command line, an environment variable or mcla.toml)
//     always wins, with the SDK's own meaning: a number is that adapter in IDXGIFactory1's order,
//     -2 the first software adapter (WARP), anything else the first that isn't software.
//  2. mcla_prefer_dedicated_gpu off: the first adapter that isn't software, as before.
//  3. Windows' own preference for this exe (Settings > System > Display > Graphics, the value
//     named by the exe's full path in HKCU\Software\Microsoft\DirectX\UserGpuPreferences, data
//     such as "GpuPreference=2;"): 1 (power saving) takes the first of the adapters in
//     IDXGIFactory6::EnumAdapterByGpuPreference's DXGI_GPU_PREFERENCE_MINIMUM_POWER order, 2
//     (high performance) the first of its DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE order.
//  4. Otherwise (no entry, or 0: "let Windows decide") the high-performance order's first.
// An adapter counts only if it isn't software and Direct3D 12 can make a device on it at
// feature level 11_0, which the SDK requires. Without IDXGIFactory6 (Windows before 10 1803), or
// when no adapter of its order qualifies, the first adapter that does is taken, as the SDK would.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mcla::gpu_choice {

// What the entry of Windows' graphics settings says, from the data of the exe's registry value
// (a list of "name=value;" items, such as "GpuPreference=2;" or "AppStatus=0;GpuPreference=1;"):
// 0 let Windows decide, 1 power saving, 2 high performance; -1 when there is no GpuPreference
// item or it isn't one of those.
constexpr int kWindowsPreferenceNone = -1;

namespace detail {
constexpr std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

constexpr bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
    if (x != y) {
      return false;
    }
  }
  return true;
}
}  // namespace detail

constexpr int ParseWindowsPreference(std::string_view data) {
  size_t position = 0;
  while (position <= data.size()) {
    size_t end = data.find(';', position);
    if (end == std::string_view::npos) {
      end = data.size();
    }
    const std::string_view item = data.substr(position, end - position);
    position = end + 1;
    const size_t equals = item.find('=');
    if (equals == std::string_view::npos ||
        !detail::EqualsIgnoreCase(detail::Trim(item.substr(0, equals)), "GpuPreference")) {
      continue;
    }
    const std::string_view value = detail::Trim(item.substr(equals + 1));
    return value.size() == 1 && value[0] >= '0' && value[0] <= '2' ? value[0] - '0'
                                                                    : kWindowsPreferenceNone;
  }
  return kWindowsPreferenceNone;
}

// What is known of one adapter without asking Direct3D 12 (that is `usable`, below).
struct Adapter {
  bool software = false;  // DXGI_ADAPTER_DESC1::Flags has DXGI_ADAPTER_FLAG_SOFTWARE
};

// Why an adapter was chosen, for the log line "GPU: '<name>' (<why>)".
enum class Why {
  kNone,                   // no adapter can be used
  kUserIndex,              // d3d12_adapter = N was set by the user
  kUserSoftware,           // d3d12_adapter = -2 was set by the user: the software adapter
  kUserFirst,              // d3d12_adapter = -1 (or another negative number) was set by the user
  kOff,                    // mcla_prefer_dedicated_gpu is off: the first adapter Windows lists
  kWindowsPowerSaving,     // Windows' graphics settings ask for power saving for this app
  kWindowsHighPerformance, // Windows' graphics settings ask for high performance for this app
  kHighPerformance,        // the high-performance GPU
  kFirstListed,            // no order to ask for, or none of its adapters qualifies
};

struct Request {
  int adapter_option = -1;            // d3d12_adapter's value
  bool adapter_option_set = false;    // set by the user: its source is not the default
  bool prefer_dedicated = true;       // mcla_prefer_dedicated_gpu
  int windows_preference = kWindowsPreferenceNone;  // ParseWindowsPreference of the exe's entry
};

struct Choice {
  int index = -1;        // the adapter's number in IDXGIFactory1::EnumAdapters1's order; -1: none
  int first_index = -1;  // the adapter the SDK would take with d3d12_adapter = -1 (-1: none, or not
                         // looked for because d3d12_adapter was set)
  Why why = Why::kNone;
  bool set_default = false;  // d3d12_adapter's default becomes `index` (the SDK then takes it)
};

// `adapters` are in EnumAdapters1's order. `high_performance` and `power_saving` are the same
// adapters (by that order's numbers) in the order of IDXGIFactory6::EnumAdapterByGpuPreference
// for DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE and DXGI_GPU_PREFERENCE_MINIMUM_POWER, empty when
// IDXGIFactory6 isn't there. `usable(i)` says whether Direct3D 12 can make a device on adapter i
// at feature level 11_0; it is only asked of the adapters the rule needs.
template <class Usable>
Choice Choose(std::span<const Adapter> adapters, std::span<const int> high_performance,
              std::span<const int> power_saving, const Request& request, Usable&& usable) {
  const int count = int(adapters.size());
  const auto first_usable_hardware = [&] {
    for (int i = 0; i < count; ++i) {
      if (!adapters[i].software && usable(i)) {
        return i;
      }
    }
    return -1;
  };
  Choice choice;
  if (request.adapter_option_set) {
    // The SDK's own reading of the number (src/ui/d3d12/d3d12_provider.cpp): that adapter if
    // Direct3D 12 can use it, else it gives up; -2 the first software adapter that can be
    // used; any other negative number the first adapter that isn't software.
    if (request.adapter_option >= 0) {
      const int i = request.adapter_option;
      choice.why = Why::kUserIndex;
      choice.index = i < count && usable(i) ? i : -1;
    } else if (request.adapter_option == -2) {
      choice.why = Why::kUserSoftware;
      for (int i = 0; i < count && choice.index < 0; ++i) {
        choice.index = adapters[i].software && usable(i) ? i : -1;
      }
    } else {
      choice.why = Why::kUserFirst;
      choice.index = first_usable_hardware();
      choice.first_index = choice.index;
    }
    return choice;
  }
  choice.first_index = first_usable_hardware();
  if (!request.prefer_dedicated) {
    choice.index = choice.first_index;
    choice.why = choice.index >= 0 ? Why::kOff : Why::kNone;
    return choice;
  }
  const bool power_saving_wanted = request.windows_preference == 1;
  for (const int i : power_saving_wanted ? power_saving : high_performance) {
    if (i >= 0 && i < count && !adapters[i].software && usable(i)) {
      choice.index = i;
      break;
    }
  }
  if (choice.index >= 0) {
    choice.why = power_saving_wanted                 ? Why::kWindowsPowerSaving
                 : request.windows_preference == 2 ? Why::kWindowsHighPerformance
                                                   : Why::kHighPerformance;
    choice.set_default = true;
    return choice;
  }
  choice.index = choice.first_index;
  choice.why = choice.index >= 0 ? Why::kFirstListed : Why::kNone;
  return choice;
}

}  // namespace mcla::gpu_choice
