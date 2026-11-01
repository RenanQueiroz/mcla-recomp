// A dead zone for each stick, for a controller with stick drift (fork issue #49): mcla_left_stick_deadzone and
// mcla_right_stick_deadzone, in percent, 0 by default.
//
// The game reads every pad through sub_8275CB78 (r3 = the user, r4 = its X_INPUT_STATE), a small thunk in front of
// XamInputGetState, so a hook there is above every input driver and below nothing of the game's. A stick is taken
// as a point: inside the circle it reads as centred, and outside it the distance from the circle's edge is
// stretched back to the full range, so the stick moves on from 0 without a jump and still reaches full lock. 0
// leaves the values as they are. The pause menu's and the settings pages' own stick reads keep their thresholds.
//
// Development: mcla_stick_deadzone_log logs what goes in and out when it changes, up to 200 lines
// (launcher/dev/scripts/deadzone.pad, launcher/dev/deadzone_check.py).
#include "generated/default/mcla_init.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_DOUBLE(mcla_left_stick_deadzone, 0.0, "MCLA",
                      "Dead zone of the left stick, in percent of its range: a stick that rests off centre "
                      "(drift) reads as centred inside it. 0 = none (changes during play)")
    .range(0.0, 50.0);
REXCVAR_DEFINE_DOUBLE(mcla_right_stick_deadzone, 0.0, "MCLA",
                      "Dead zone of the right stick, in percent of its range: a stick that rests off centre "
                      "(drift) reads as centred inside it. 0 = none (changes during play)")
    .range(0.0, 50.0);
REXCVAR_DEFINE_BOOL(mcla_stick_deadzone_log, false, "MCLA",
                    "Development: log the sticks in and out of the dead zones when they change");

namespace {

// X_INPUT_STATE in guest memory: big-endian, the sticks from offset 8 as lx, ly, rx, ry.
constexpr uint32_t kLeftStick = 8;
constexpr uint32_t kRightStick = 12;

int16_t ReadAxis(const uint8_t* p) {
  return static_cast<int16_t>(static_cast<uint16_t>((p[0] << 8) | p[1]));
}

void WriteAxis(uint8_t* p, int16_t v) {
  p[0] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
  p[1] = static_cast<uint8_t>(static_cast<uint16_t>(v));
}

int16_t ToAxis(double v) {
  return static_cast<int16_t>(std::clamp(std::lround(v * 32767.0), -32768L, 32767L));
}

void ApplyToStick(uint8_t* stick, double percent) {
  if (!(percent > 0.0)) {
    return;
  }
  const double zone = std::min(percent, 50.0) / 100.0;
  const double x = ReadAxis(stick) / 32767.0;
  const double y = ReadAxis(stick + 2) / 32767.0;
  const double length = std::sqrt(x * x + y * y);
  if (length <= zone) {
    WriteAxis(stick, 0);
    WriteAxis(stick + 2, 0);
    return;
  }
  const double scale = (length - zone) / (1.0 - zone) / length;
  WriteAxis(stick, ToAxis(x * scale));
  WriteAxis(stick + 2, ToAxis(y * scale));
}

void LogChange(int lx, int ly, int rx, int ry, const uint8_t* state, double left, double right) {
  static int last[6] = {1 << 20};
  static double last_left = -1.0, last_right = -1.0;
  static std::atomic<int> lines{0};
  const int now[6] = {lx, ly, rx, ry, ReadAxis(state + kRightStick), ReadAxis(state + kLeftStick)};
  if (std::equal(now, now + 6, last) && left == last_left && right == last_right) {
    return;
  }
  std::copy(now, now + 6, last);
  last_left = left;
  last_right = right;
  if (lines.fetch_add(1, std::memory_order_relaxed) < 200) {
    REXLOG_INFO("stick dead zone: left {}% ({},{}) -> ({},{}), right {}% ({},{}) -> ({},{})", left, lx, ly,
                ReadAxis(state + kLeftStick), ReadAxis(state + kLeftStick + 2), right, rx, ry,
                ReadAxis(state + kRightStick), ReadAxis(state + kRightStick + 2));
  }
}

}  // namespace

extern "C" REX_FUNC(sub_8275CB78) {
  const uint32_t state = ctx.r4.u32;
  __imp__sub_8275CB78(ctx, base);
  if (state == 0 || ctx.r3.u32 != 0) {
    return;
  }
  const double left = REXCVAR_GET(mcla_left_stick_deadzone);
  const double right = REXCVAR_GET(mcla_right_stick_deadzone);
  uint8_t* guest = base + state;
  const bool log = REXCVAR_GET(mcla_stick_deadzone_log);
  int in[4] = {};
  if (log) {
    in[0] = ReadAxis(guest + kLeftStick);
    in[1] = ReadAxis(guest + kLeftStick + 2);
    in[2] = ReadAxis(guest + kRightStick);
    in[3] = ReadAxis(guest + kRightStick + 2);
  }
  ApplyToStick(guest + kLeftStick, left);
  ApplyToStick(guest + kRightStick, right);
  if (log) {
    LogChange(in[0], in[1], in[2], in[3], guest, left, right);
  }
}
