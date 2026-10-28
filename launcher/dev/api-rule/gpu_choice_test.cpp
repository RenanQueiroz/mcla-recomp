// The choice of the GPU (src/gpu_choice.h) with made-up PCs: the adapters in the order DXGI
// lists them, the two orders Windows ranks them in (high performance, power saving), whether
// Direct3D 12 can use each, Windows' per-app graphics preference for the exe, the d3d12_adapter
// option and the mcla_prefer_dedicated_gpu switch. The launcher's DisplayDrivers.ChooseAdapter
// (launcher/src/ShaderStamp.cs) is tried with the same table by launcher/dev/api-rule/apitest;
// the two tables must stay equal.
//
//   clang++ -std=c++20 -static -I src launcher/dev/api-rule/gpu_choice_test.cpp -o gpu_choice_test.exe
//   gpu_choice_test.exe          (exit code 0: every row as expected)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gpu_choice.h"

using namespace mcla::gpu_choice;

namespace {

// Adapter shorthands, for the rows below.
constexpr Adapter kIntel{false};   // an Intel iGPU
constexpr Adapter kNvidia{false};  // a GeForce
constexpr Adapter kAmdIgpu{false}; // the Radeon of an APU
constexpr Adapter kAmdCard{false}; // a Radeon card
constexpr Adapter kWarp{true};     // Microsoft Basic Render Driver

struct Case {
  const char* what;
  std::vector<Adapter> adapters;  // EnumAdapters1's order
  std::vector<int> high, power;   // Windows' rankings (empty: no IDXGIFactory6)
  Request request;
  std::vector<bool> usable;       // empty: Direct3D 12 can use every adapter
  int index;
  int first_index;
  Why why;
  bool set_default;
};

Request Req(int windows_preference = kWindowsPreferenceNone, bool prefer_dedicated = true,
            bool option_set = false, int option = -1) {
  Request request;
  request.windows_preference = windows_preference;
  request.prefer_dedicated = prefer_dedicated;
  request.adapter_option_set = option_set;
  request.adapter_option = option;
  return request;
}

const char* WhyName(Why why) {
  switch (why) {
    case Why::kNone: return "none";
    case Why::kUserIndex: return "user index";
    case Why::kUserSoftware: return "user software";
    case Why::kUserFirst: return "user first";
    case Why::kOff: return "off";
    case Why::kWindowsPowerSaving: return "windows power saving";
    case Why::kWindowsHighPerformance: return "windows high performance";
    case Why::kHighPerformance: return "high performance";
    case Why::kFirstListed: return "first listed";
  }
  return "?";
}

const std::vector<Case>& Cases() {
  static const std::vector<Case> cases = {
      // The report: a laptop where Windows lists the Intel iGPU first. Defaults take the NVIDIA GPU.
      {"laptop, Intel listed first, GeForce second: defaults", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(), {}, 1, 0, Why::kHighPerformance, true},
      {"laptop, Windows already lists the GeForce first", {kNvidia, kIntel, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(), {}, 0, 0, Why::kHighPerformance, true},
      // An APU with a Radeon card, and an RDNA iGPU with a GeForce (this PC: the monitor is on the GeForce).
      {"RDNA iGPU + GeForce, GeForce first (this PC)", {kNvidia, kAmdIgpu, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(), {}, 0, 0, Why::kHighPerformance, true},
      {"RDNA iGPU listed first + GeForce", {kAmdIgpu, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(), {}, 1, 0, Why::kHighPerformance, true},
      {"APU's Radeon listed first + Radeon card", {kAmdIgpu, kAmdCard, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(), {}, 1, 0, Why::kHighPerformance, true},
      {"three GPUs: Intel, AMD iGPU, GeForce", {kIntel, kAmdIgpu, kNvidia, kWarp}, {2, 1, 0, 3}, {0, 1, 2, 3}, Req(), {}, 2, 0, Why::kHighPerformance, true},
      // One GPU.
      {"Intel only (and WARP)", {kIntel, kWarp}, {0, 1}, {0, 1}, Req(), {}, 0, 0, Why::kHighPerformance, true},
      {"GeForce only (and WARP)", {kNvidia, kWarp}, {0, 1}, {0, 1}, Req(), {}, 0, 0, Why::kHighPerformance, true},
      {"RDNA iGPU only (and WARP)", {kAmdIgpu, kWarp}, {0, 1}, {0, 1}, Req(), {}, 0, 0, Why::kHighPerformance, true},
      // Software adapters (WARP).
      {"WARP only: nothing to use", {kWarp}, {0}, {0}, Req(), {}, -1, -1, Why::kNone, false},
      {"no adapter at all", {}, {}, {}, Req(), {}, -1, -1, Why::kNone, false},
      {"d3d12_adapter = -2 set: the software adapter", {kNvidia, kAmdIgpu, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(kWindowsPreferenceNone, true, true, -2), {}, 2, -1, Why::kUserSoftware, false},
      {"d3d12_adapter = -2 set, no software adapter", {kNvidia}, {0}, {0}, Req(kWindowsPreferenceNone, true, true, -2), {}, -1, -1, Why::kUserSoftware, false},
      {"the ranking lists WARP first: skipped", {kIntel, kWarp}, {1, 0}, {1, 0}, Req(), {}, 0, 0, Why::kHighPerformance, true},
      // Windows' graphics settings for the exe: 0 let Windows decide, 1 power saving, 2 high performance.
      {"registry 0 (let Windows decide): high performance", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(0), {}, 1, 0, Why::kHighPerformance, true},
      {"registry 1 (power saving): the Intel iGPU", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(1), {}, 0, 0, Why::kWindowsPowerSaving, true},
      {"registry 2 (high performance): the GeForce", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(2), {}, 1, 0, Why::kWindowsHighPerformance, true},
      {"registry 1 (power saving), RDNA iGPU + GeForce (this PC): the Radeon", {kNvidia, kAmdIgpu, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(1), {}, 1, 0, Why::kWindowsPowerSaving, true},
      {"registry 2 (high performance), RDNA iGPU + GeForce (this PC): the GeForce", {kNvidia, kAmdIgpu, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(2), {}, 0, 0, Why::kWindowsHighPerformance, true},
      {"registry 1 (power saving), one GPU", {kIntel, kWarp}, {0, 1}, {0, 1}, Req(1), {}, 0, 0, Why::kWindowsPowerSaving, true},
      {"registry not understood (-1): high performance", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(-1), {}, 1, 0, Why::kHighPerformance, true},
      // d3d12_adapter set by the user: the SDK's own numbering and meaning, the registry and the ranking not asked.
      {"d3d12_adapter = 0 set", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, true, true, 0), {}, 0, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 1 set", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, true, true, 1), {}, 1, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 2 set: WARP by its number", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, true, true, 2), {}, 2, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 7 set: names no adapter", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, true, true, 7), {}, -1, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 1 set, Direct3D 12 cannot use it", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, true, true, 1), {true, false, true}, -1, -1, Why::kUserIndex, false},
      {"d3d12_adapter = -1 set: the first, as the SDK", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, true, true, -1), {}, 0, 0, Why::kUserFirst, false},
      {"d3d12_adapter = 0 set wins over registry 2", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(2, true, true, 0), {}, 0, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 0 set wins over registry 1", {kNvidia, kAmdIgpu, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(1, true, true, 0), {}, 0, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 0 set wins over the off switch", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, false, true, 0), {}, 0, -1, Why::kUserIndex, false},
      {"d3d12_adapter = 1 set on this PC: the Radeon", {kNvidia, kAmdIgpu, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(kWindowsPreferenceNone, true, true, 1), {}, 1, -1, Why::kUserIndex, false},
      // The off switch (mcla_prefer_dedicated_gpu = false): the first adapter that isn't software, as before.
      {"off switch: the Intel iGPU, listed first", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(kWindowsPreferenceNone, false), {}, 0, 0, Why::kOff, false},
      {"off switch ignores registry 2", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(2, false), {}, 0, 0, Why::kOff, false},
      {"off switch, Windows lists the GeForce first", {kNvidia, kIntel, kWarp}, {0, 1, 2}, {1, 0, 2}, Req(kWindowsPreferenceNone, false), {}, 0, 0, Why::kOff, false},
      {"off switch, WARP only", {kWarp}, {0}, {0}, Req(kWindowsPreferenceNone, false), {}, -1, -1, Why::kNone, false},
      // What Direct3D 12 cannot use, and a Windows without a ranking.
      {"no ranking (no IDXGIFactory6): the first, as the SDK", {kIntel, kNvidia, kWarp}, {}, {}, Req(), {}, 0, 0, Why::kFirstListed, false},
      {"no ranking, registry 2: still the first", {kIntel, kNvidia, kWarp}, {}, {}, Req(2), {}, 0, 0, Why::kFirstListed, false},
      {"the ranked GeForce cannot make a Direct3D 12 device: the next", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(), {true, false, true}, 0, 0, Why::kHighPerformance, true},
      {"the first listed cannot make a Direct3D 12 device: it is not 'first'", {kIntel, kNvidia, kWarp}, {1, 0, 2}, {0, 1, 2}, Req(), {false, true, true}, 1, 1, Why::kHighPerformance, true},
      {"the ranking names an adapter that is not there: skipped", {kIntel, kNvidia, kWarp}, {9, -1, 1, 0}, {0, 1, 2}, Req(), {}, 1, 0, Why::kHighPerformance, true},
      {"the ranking names no usable adapter: the first, as the SDK", {kIntel, kNvidia, kWarp}, {2}, {2}, Req(), {}, 0, 0, Why::kFirstListed, false},
  };
  return cases;
}

struct ParseRow {
  const char* data;
  int expected;
};

constexpr ParseRow kParseRows[] = {
    {"GpuPreference=2;", 2},
    {"GpuPreference=1;", 1},
    {"GpuPreference=0;", 0},
    {"AppStatus=0;", -1},  // the entry Windows wrote for another game on this PC
    {"AppStatus=0;GpuPreference=2;", 2},
    {"GpuPreference=1;AppStatus=0;", 1},
    {"GpuPreference=2", 2},  // no closing ;
    {"gpupreference=1;", 1},
    {" GpuPreference = 2 ; ", 2},
    {"GpuPreference=3;", -1},
    {"GpuPreference=12;", -1},
    {"GpuPreference=;", -1},
    {"GpuPreference=a;", -1},
    {"GpuPreference;", -1},
    {"XGpuPreference=2;", -1},
    {"", -1},
    {";;;", -1},
    {"GpuPreference=1;GpuPreference=2;", 1},  // the first item counts
};

// The cases that matter most are also checked when this file is compiled.
static_assert(ParseWindowsPreference("GpuPreference=2;") == 2);
static_assert(ParseWindowsPreference("AppStatus=0;") == kWindowsPreferenceNone);

}  // namespace

int main() {
  int failures = 0;
  for (const Case& c : Cases()) {
    const auto usable = [&](int i) {
      return c.usable.empty() ? true : (i >= 0 && size_t(i) < c.usable.size() && c.usable[i]);
    };
    const Choice choice = Choose(c.adapters, c.high, c.power, c.request, usable);
    const bool ok = choice.index == c.index && choice.first_index == c.first_index &&
                    choice.why == c.why && choice.set_default == c.set_default;
    std::printf("%s  %-72s -> index %2d (first %2d) %-24s default %d\n", ok ? "ok  " : "FAIL", c.what,
                choice.index, choice.first_index, WhyName(choice.why), choice.set_default ? 1 : 0);
    if (!ok) {
      std::printf("      expected index %d (first %d) %s default %d\n", c.index, c.first_index,
                  WhyName(c.why), c.set_default ? 1 : 0);
    }
    failures += ok ? 0 : 1;
  }
  for (const ParseRow& row : kParseRows) {
    const int got = ParseWindowsPreference(row.data);
    const bool ok = got == row.expected;
    std::printf("%s  parse '%s' -> %d\n", ok ? "ok  " : "FAIL", row.data, got);
    failures += ok ? 0 : 1;
  }
  std::printf("%d choice row(s), %d parse row(s), %d failure(s)\n", int(Cases().size()),
              int(sizeof(kParseRows) / sizeof(kParseRows[0])), failures);
  return failures ? 1 : 0;
}
