// The rule of the Automatic graphics API (src/automatic_api.h) with made-up values: the vendor,
// whether the GPU shares the CPU's memory (UMA) and the smallest wave Direct3D 12 reports. The
// launcher's DisplayDrivers.AutomaticApiFor (launcher/src/ShaderStamp.cs) is tried with the same
// table by launcher/dev/api-rule/ApiRuleTest.cs; the two tables must stay equal.
//
//   clang++ -std=c++20 -static -I src launcher/dev/api-rule/api_rule_test.cpp -o api_rule_test.exe
//   api_rule_test.exe          (exit code 0: every row as expected)
#include <cstdio>
#include <cstring>

#include "automatic_api.h"

using namespace mcla::automatic_api;

struct Row {
  const char* what;
  uint32_t vendor;
  bool uma;
  uint32_t wave;
  bool vulkan;
  const char* kind;
};

constexpr Row kRows[] = {
    // AMD integrated: RDNA (wave 32) is Vulkan's, GCN / Vega (wave 64) is not.
    {"Ryzen 9800X3D Radeon (RDNA 2, UMA, wave 32)", 0x1002, true, 32, true,
     "AMD integrated graphics (RDNA, 32-lane waves)"},
    {"Ryzen 7 5700G Vega 8 (GCN, UMA, wave 64)", 0x1002, true, 64, false,
     "AMD integrated graphics (GCN or Vega, 64-lane waves)"},
    {"AMD integrated, wave size not answered (0)", 0x1002, true, 0, false,
     "AMD integrated graphics (wave size not known)"},
    {"AMD integrated, an unexpected wave size (16)", 0x1002, true, 16, false,
     "AMD integrated graphics (wave size not known)"},
    // AMD discrete: Direct3D 12 whatever the wave (an RX 6600 XT is RDNA 2, wave 32; fork issue #7).
    {"RX 6600 XT (RDNA 2, discrete, wave 32)", 0x1002, false, 32, false, "AMD discrete graphics"},
    {"RX 580 (GCN, discrete, wave 64)", 0x1002, false, 64, false, "AMD discrete graphics"},
    {"AMD discrete, nothing answered", 0x1002, false, 0, false, "AMD discrete graphics"},
    // Intel: Vulkan as before, the facts do not matter (they are not asked).
    {"Intel, not asked (UMA no, wave 0)", 0x8086, false, 0, true, "Intel graphics"},
    {"Intel, UMA yes, wave 16", 0x8086, true, 16, true, "Intel graphics"},
    {"Intel, UMA no, wave 32", 0x8086, false, 32, true, "Intel graphics"},
    // NVIDIA and the rest: Direct3D 12, even when the (unasked) facts look like RDNA's.
    {"RTX 5080 (not asked)", 0x10DE, false, 0, false, "NVIDIA graphics"},
    {"NVIDIA, UMA yes, wave 32", 0x10DE, true, 32, false, "NVIDIA graphics"},
    {"WARP (Microsoft Basic Render Driver)", 0x1414, false, 0, false, "graphics"},
    {"WARP, UMA yes, wave 32", 0x1414, true, 32, false, "graphics"},
    {"no vendor", 0, false, 0, false, "graphics"},
};

int main() {
  int failures = 0;
  for (const Row& row : kRows) {
    const AdapterFacts facts{row.vendor, row.uma, row.wave};
    const bool vulkan = PrefersVulkan(facts);
    const char* kind = KindOf(facts);
    const bool ok = vulkan == row.vulkan && std::strcmp(kind, row.kind) == 0;
    std::printf("%s  %-48s vendor %#06x uma %d wave %2u -> %s | %s\n", ok ? "ok  " : "FAIL",
                row.what, row.vendor, row.uma ? 1 : 0, row.wave, vulkan ? "vulkan" : "d3d12", kind);
    failures += ok ? 0 : 1;
  }
  // The rule is a constant expression: the build can check the two cases that matter most.
  static_assert(PrefersVulkan({0x1002, true, 32}));
  static_assert(!PrefersVulkan({0x1002, true, 64}));
  std::printf("%d row(s), %d failure(s)\n", int(sizeof(kRows) / sizeof(kRows[0])), failures);
  return failures ? 1 : 0;
}
