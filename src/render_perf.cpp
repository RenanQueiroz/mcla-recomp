// Rendering performance hooks. See config/render_perf.toml for what each
// address is and where the analysis came from.

#include "generated/default/mcla_init.h"

#include <cstdint>
#include <thread>

#include <rex/cvar.h>
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

// The size of the EDRAM the GPU emulates, in tiles; 0 until it has started.
uint32_t HostEdramTiles() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
  return graphics ? graphics->edram_tile_count() : 0;
}

// EDRAM tiles of the colour and the depth of a tiled target drawn as one tile,
// in the game's own layout (sub_8217A470): 80x16-pixel tiles without MSAA,
// 80x8 with 2x, 40x8 with 4x, on a width rounded to 160 and a height to 32.
uint32_t OneTileEdramTiles(uint32_t width, uint32_t height, uint32_t msaa) {
  const uint32_t tile_width = msaa > 2 ? 40 : 80;
  const uint32_t tile_height = msaa > 1 ? 8 : 16;
  const uint32_t pitch_tiles = (width + 159) / 160 * 160 / tile_width;
  const uint32_t rows = ((height + 31) & ~uint32_t(31)) / tile_height;
  return 2 * pitch_tiles * rows;
}

}  // namespace

bool MclaSingleTileAvailable() {
  return HostEdramTiles() >= OneTileEdramTiles(1280, 720, 2);
}

void mcla_single_tile(PPCRegister& r7, PPCRegister& r8, PPCRegister& r18, PPCRegister& r25,
                      PPCRegister& r28) {
  if (!REXCVAR_GET(mcla_single_tile)) {
    return;
  }
  uint8_t* count = rex::Runtime::instance()->virtual_membase() + kGuestTileCount;
  if (LoadBe32(count) <= 1 ||
      OneTileEdramTiles(r28.u32, r25.u32, r18.u32) > HostEdramTiles()) {
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
