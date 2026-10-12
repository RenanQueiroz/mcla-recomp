// Development aid for function recovery.
//
// Functions reached only through pointers (static initialisers, vtables,
// callbacks) are invisible to the recompiler's call-graph analysis and fail at
// run time one at a time. This scans the loaded image's data for values that
// look like pointers to function entries the recompiler did not emit, and
// writes them as TOML hints for review. It reads guest memory only; nothing
// from the image is written out except the addresses.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(mcla_scan_code_pointers, "", "MCLA",
                      "Write unrecovered function-pointer candidates to this TOML file, then exit");

namespace {

// default.xex 0.0.0.8 layout, from the XEX page descriptors (docs/phase0-recon.md).
constexpr uint32_t kImageBase = 0x82000000;
constexpr uint32_t kCodeBegin = 0x82130000;
constexpr uint32_t kCodeEnd = 0x827CD054;
constexpr uint32_t kImageEnd = 0x829E0000;

constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBctr = 0x4E800420;

uint32_t ReadBE(const rex::memory::Memory* memory, uint32_t address) {
  const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// A function entry normally follows the end of another function: a return, an
// indirect jump, an unconditional branch (tail call), or alignment padding.
bool FollowsFunctionEnd(const rex::memory::Memory* memory, uint32_t address) {
  if (address == kCodeBegin) {
    return true;
  }
  uint32_t prev = ReadBE(memory, address - 4);
  if (prev == kBlr || prev == kBctr || prev == 0) {
    return true;
  }
  // b target (opcode 18, AA=0, LK=0)
  return (prev & 0xFC000003) == 0x48000000;
}

// Target of a branch that stays in the function (b or bc, not absolute, not
// linking), or 0.
uint32_t LocalBranchTarget(uint32_t address, uint32_t insn) {
  if (insn & 3) {
    return 0;
  }
  if ((insn >> 26) == 18) {
    return address + uint32_t(int32_t(insn << 6) >> 6 & ~3);
  }
  if ((insn >> 26) == 16) {
    return address + uint32_t(int32_t(int16_t(insn & 0xFFFC)));
  }
  return 0;
}

// True when no branch crosses `address` between the recovered entries on
// either side of it: then it is a function of its own and not a label inside
// its neighbour.
bool StandsAlone(const rex::memory::Memory* memory, uint32_t begin, uint32_t address,
                 uint32_t end) {
  for (uint32_t at = begin; at < end; at += 4) {
    uint32_t target = LocalBranchTarget(at, ReadBE(memory, at));
    if (target < begin || target >= end) {
      continue;
    }
    if ((at < address) != (target < address)) {
      return false;
    }
  }
  return true;
}

}  // namespace

void MclaApp::OnPreLaunchModule() {
  std::string out_path = REXCVAR_GET(mcla_scan_code_pointers);
  if (out_path.empty()) {
    InstallCrashTrace();
    return;
  }
  auto* memory = runtime()->memory();
  auto* dispatcher = runtime()->function_dispatcher();

  std::set<uint32_t> candidates;
  uint32_t pointers_seen = 0;
  for (uint32_t address = kImageBase; address + 4 <= kImageEnd; address += 4) {
    if (address >= kCodeBegin && address < kCodeEnd) {
      continue;
    }
    uint32_t value = ReadBE(memory, address);
    if (value < kCodeBegin || value >= kCodeEnd || (value & 3)) {
      continue;
    }
    ++pointers_seen;
    if (dispatcher->GetFunction(value)) {
      continue;
    }
    if (ReadBE(memory, value) == 0 || !FollowsFunctionEnd(memory, value)) {
      continue;
    }
    candidates.insert(value);
  }

  // Pointers built in code: lis rA,hi then addi rD,rA,lo within a few
  // instructions (callbacks handed to a registrar are made this way and never
  // appear as a data word).
  std::set<uint32_t> built;
  uint32_t built_seen = 0;
  for (uint32_t address = kCodeBegin; address + 4 <= kCodeEnd; address += 4) {
    uint32_t insn = ReadBE(memory, address);
    uint32_t ra = (insn >> 16) & 31;
    if ((insn >> 26) != 14 || ra == 0) {
      continue;
    }
    for (uint32_t back = 4; back <= 32 && address - back >= kCodeBegin; back += 4) {
      uint32_t prev = ReadBE(memory, address - back);
      if (prev == kBlr || prev == kBctr) {
        break;
      }
      if ((prev >> 26) != 15 || ((prev >> 16) & 31) != 0 || ((prev >> 21) & 31) != ra) {
        continue;
      }
      uint32_t value = (prev << 16) + uint32_t(int32_t(int16_t(insn & 0xFFFF)));
      if (value < kCodeBegin || value >= kCodeEnd || (value & 3)) {
        break;
      }
      ++built_seen;
      if (!dispatcher->GetFunction(value) && ReadBE(memory, value) != 0 &&
          FollowsFunctionEnd(memory, value)) {
        built.insert(value);
      }
      break;
    }
  }
  // Keep only the built ones that are not a label inside a recovered function.
  uint32_t built_inside = 0;
  for (uint32_t value : built) {
    uint32_t begin = value, end = value + 4;
    while (begin > kCodeBegin && !dispatcher->GetFunction(begin)) {
      begin -= 4;
    }
    while (end < kCodeEnd && !dispatcher->GetFunction(end)) {
      end += 4;
    }
    if (StandsAlone(memory, begin, value, end)) {
      candidates.insert(value);
    } else {
      ++built_inside;
    }
  }

  if (FILE* f = std::fopen(out_path.c_str(), "w")) {
    std::fprintf(f, "# Generated by --mcla_scan_code_pointers. Review before use.\n");
    for (uint32_t address : candidates) {
      std::fprintf(f, "[functions.\"0x%08X\"]\nname = \"sub_%08X\"\n\n", address, address);
    }
    std::fclose(f);
  }
  REXLOG_INFO("code pointer scan: {} code pointers in data, {} built in code ({} of them labels "
              "inside a function, left out), {} unrecovered candidates -> {}",
              pointers_seen, built_seen, built_inside, candidates.size(), out_path);
  std::exit(0);
}
