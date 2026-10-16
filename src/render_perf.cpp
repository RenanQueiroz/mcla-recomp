// Rendering performance hooks. See config/render_perf.toml for what each
// address is and where the analysis came from.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_single_tile, true, "MCLA",
                    "Render the main scene as one tile instead of the Xbox 360's two, where the "
                    "GPU emulates an EDRAM large enough (edram_tiles = 4096)");
REXCVAR_DEFINE_BOOL(mcla_fence_yield, true, "MCLA",
                    "Yield the CPU while the game waits on the GPU instead of spinning");

namespace {

// Tile count the game computed for the current tiled pass.
constexpr uint32_t kGuestTileCount = 0x827D42A4;

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t LoadBe16(const uint8_t* p) {
  return (uint32_t(p[0]) << 8) | p[1];
}

// The size of the EDRAM the GPU emulates, in tiles; 0 until it has started.
uint32_t HostEdramTiles() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->edram_tile_count() : 0;
}

// The game's tile surfaces are sized on a width rounded to 160 and a height
// rounded to 32 (sub_8217A470, at 0x8217A700).
uint32_t TileSurfaceWidth(uint32_t width) {
  return (width + 159) / 160 * 160;
}
uint32_t TileSurfaceHeight(uint32_t height) {
  return (height + 31) & ~uint32_t(31);
}

// EDRAM tiles of one surface (the colour or the depth) of a tiled target drawn
// as one tile, in the game's own layout: 80x16-pixel tiles without MSAA, 80x8
// with 2x, 40x8 with 4x.
uint32_t OneTileSurfaceTiles(uint32_t width, uint32_t height, uint32_t msaa) {
  const uint32_t tile_width = msaa > 2 ? 40 : 80;
  const uint32_t tile_height = msaa > 1 ? 8 : 16;
  return TileSurfaceWidth(width) / tile_width * (TileSurfaceHeight(height) / tile_height);
}

// Whether the colour and the depth fit in the EDRAM the GPU emulates, one after
// the other, each within the console's 2,048 tiles (as far as a host render
// target reaches from its base).
bool OneTileFits(uint32_t width, uint32_t height, uint32_t msaa) {
  const uint32_t surface = OneTileSurfaceTiles(width, height, msaa);
  return surface <= 2048 && 2 * surface <= HostEdramTiles();
}

// The tile surface cache of BeginTiledRendering (the loop at loc_8217A724):
// 8 slots, each a colour surface pointer (0 for a free slot) with its depth
// surface 8 slots on and its MSAA mode in an array of its own; a surface keeps
// its width at +56 and its height at +58 (16 bits each) and its format at +60.
// Slots are never freed, and a ninth key ends in sub_821BD618, which never
// returns. The game's own keys take 4 or 5; one tile adds one.
constexpr uint32_t kTileCacheColour = 0x828393D8;
constexpr uint32_t kTileCacheMsaa = 0x827D4284;
constexpr uint32_t kTileCacheSlots = 8;

struct TileCacheLookup {
  uint32_t used = 0;
  bool has_key = false;
  std::string keys;
};

TileCacheLookup LookUpTileCache(const uint8_t* base, uint32_t msaa, uint32_t width,
                                uint32_t height, uint32_t format, bool list_keys) {
  TileCacheLookup lookup;
  for (uint32_t i = 0; i < kTileCacheSlots; ++i) {
    const uint32_t surface = LoadBe32(base + kTileCacheColour + 4 * i);
    if (!surface) {
      continue;
    }
    ++lookup.used;
    if (surface == 0xFFFFFFFFu) {
      continue;
    }
    const uint32_t slot_msaa = LoadBe32(base + kTileCacheMsaa + 4 * i);
    const uint32_t slot_width = LoadBe16(base + surface + 56);
    const uint32_t slot_height = LoadBe16(base + surface + 58);
    const uint32_t slot_format = LoadBe32(base + surface + 60);
    lookup.has_key |= slot_msaa == msaa && slot_width == width && slot_height == height &&
                      slot_format == format;
    if (list_keys) {
      lookup.keys += " " + std::to_string(slot_msaa) + "x " + std::to_string(slot_width) + "x" +
                     std::to_string(slot_height) + " f" + std::to_string(slot_format);
    }
  }
  return lookup;
}

// Passes drawn in tiles around a change of MSAA mode between one-tile passes
// (mcla_single_tile below). Three were not always enough; four and more were,
// at native and 3x, capped and uncapped.
constexpr int kSettlePasses = 8;

// The last tiled pass: its MSAA mode and the tile count it was drawn with
// (BeginTiledRendering runs on the thread that renders frames), and how many
// passes in a row were drawn in tiles since the hook last made one tile.
uint32_t g_last_msaa = 0;
uint32_t g_last_count = 0;
std::atomic<int> g_tiled_passes{kSettlePasses};
std::atomic<int> g_hold_off{0};

}  // namespace

bool MclaSingleTileAvailable() {
  return OneTileFits(1280, 720, 2);
}

// Whether the MSAA mode may change now (perf_options.cpp, ApplyMsaa, at the
// swap): not while recent passes were drawn as one tile. Then the next ones
// are drawn in tiles, and this says yes once enough of them have been.
bool MclaSingleTileReadyForMsaaChange() {
  if (g_tiled_passes.load(std::memory_order_acquire) >= kSettlePasses) {
    return true;
  }
  if (g_hold_off.load(std::memory_order_acquire) <= 0) {
    g_hold_off.store(kSettlePasses, std::memory_order_release);
  }
  return false;
}

// Changing the MSAA mode between two one-tile passes leaves the vehicle pass
// (phase 0x10000), and what is drawn after it, with the other mode's
// RB_SURFACE_INFO: 1x draws in the 2x scene, or 2x in the 1x one, until the
// pause menu next opens or the scene is drawn in tiles for a few frames. On
// the console the MSAA mode never changed, and one tile only came without
// MSAA. So a change of MSAA mode is surrounded by passes drawn in tiles:
// - here, after a one-tile pass in another MSAA mode (anti-aliasing turned on
//   with single tile on, which the game itself draws as one tile with it off),
//   the next kSettlePasses passes are drawn in tiles;
// - perf_options.cpp (ApplyMsaa) holds anti-aliasing off back until that many
//   passes have been drawn in tiles (MclaSingleTileReadyForMsaaChange).
// launcher/dev/single-tile-edram-2026-10-16.md.
void mcla_single_tile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r18, PPCRegister& r19,
                      PPCRegister& r25, PPCRegister& r28) {
  uint8_t* base = rex::Runtime::instance()->virtual_membase();
  uint8_t* count = base + kGuestTileCount;
  const uint32_t game_count = LoadBe32(count);
  const uint32_t msaa = r18.u32;
  bool act = REXCVAR_GET(mcla_single_tile) && game_count > 1 &&
             OneTileFits(r28.u32, r25.u32, msaa);
  if (act && g_last_count == 1 && g_last_msaa != msaa) {
    g_hold_off.store(kSettlePasses, std::memory_order_release);
  }
  if (act && g_hold_off.load(std::memory_order_acquire) > 0) {
    g_hold_off.fetch_sub(1, std::memory_order_acq_rel);
    act = false;
  }
  // One tile must not take the cache's last free slots.
  const uint32_t width = TileSurfaceWidth(r28.u32);
  const uint32_t height = TileSurfaceHeight(r25.u32);
  const TileCacheLookup cache = LookUpTileCache(base, msaa, width, height, r19.u32, false);
  static uint32_t logged_used = 0;
  if (cache.used > logged_used) {
    logged_used = cache.used;
    REXLOG_INFO("single tile: the tile surface cache holds {} of {} surfaces:{}", cache.used,
                kTileCacheSlots, LookUpTileCache(base, msaa, width, height, r19.u32, true).keys);
  }
  if (act && !cache.has_key && cache.used >= kTileCacheSlots - 1) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("single tile: not used, {} of the tile surface cache's {} slots are taken",
                  cache.used, kTileCacheSlots);
    }
    act = false;
  }
  g_last_msaa = msaa;
  g_last_count = act ? 1 : game_count;
  if (act) {
    g_tiled_passes.store(0, std::memory_order_release);
  } else if (game_count > 1 &&
             g_tiled_passes.load(std::memory_order_acquire) < kSettlePasses * 4) {
    g_tiled_passes.fetch_add(1, std::memory_order_acq_rel);
  }
  if (!act) {
    return;
  }
  r7.u64 = r28.u64;
  r8.u64 = r25.u64;
  // The count was stored before this point; EndTiledRendering reads it back.
  count[0] = 0;
  count[1] = 0;
  count[2] = 0;
  count[3] = 1;
}

bool mcla_edram_limit(PPCRegister& r3, PPCRegister& r30, PPCRegister& r11) {
  (void)r30;
  if (!REXCVAR_GET(mcla_single_tile)) {
    return false;
  }
  // The console's check unless the GPU emulates a larger EDRAM.
  const uint32_t host_tiles = HostEdramTiles();
  return host_tiles > 0x800 && r3.u32 < host_tiles && r11.u32 <= host_tiles;
}

bool mcla_fence_spin() {
  if (!REXCVAR_GET(mcla_fence_yield)) {
    return false;
  }
  // The caller loops until the GPU thread advances. A short pause keeps the
  // wake-up latency low; every so often give the core away entirely.
  static thread_local uint32_t spins = 0;
  if ((++spins & 0x3F) == 0) {
    std::this_thread::yield();
  } else {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#endif
  }
  return true;
}
