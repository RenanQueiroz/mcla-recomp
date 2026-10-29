// The player car's state, read where the HUD reads it. See
// config/vehicle_telemetry.toml for the address.
//
// sub_822112A8 updates the race HUD once a frame. At 0x822117C0 r30 is the
// player; [r30+8] is the car, whose physics body ([car+128]) has the velocity
// at +272/+276/+280 (the speedometer is its length). The same function then
// feeds the nitro gauge from car+664 (bars: an int16 at +18 plus a float at
// +28; +24 counts while it burns) and the gear display from [car+16]+60 -> +20
// (1 and up, -1 in reverse). Checked against a logged drive on a PS5. Every
// pointer read here is one the function itself dereferences right after, so
// they are valid whenever it runs.
//
// The HUD keeps updating in the pause menu, where the physics stand still: the
// velocity is then bit for bit the same every frame, which frozen_frames
// counts. mcla_telemetry_log writes the values to the log twice a second.

#include "generated/default/mcla_init.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "vehicle_telemetry.h"

REXCVAR_DEFINE_BOOL(mcla_telemetry_log, false, "MCLA",
                    "Development: log the player car's state twice a second");

namespace {

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Read16(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

float ReadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return std::isfinite(value) ? value : 0.0f;
}

bool Plausible(uint32_t pointer) {
  return pointer >= 0x40000000u && pointer < 0xC0000000u && (pointer & 3) == 0;
}

}  // namespace

void mcla_vehicle_telemetry(PPCRegister& r30) {
  auto* runtime = rex::Runtime::instance();
  const uint8_t* base = runtime ? runtime->virtual_membase() : nullptr;
  if (!base || !Plausible(r30.u32)) return;
  const uint32_t car = Read32(base, r30.u32 + 8);
  if (!Plausible(car)) return;
  const uint32_t body = Read32(base, car + 128);
  const uint32_t tach = Read32(base, car + 664);
  const uint32_t drive = Read32(base, car + 16);
  if (!Plausible(body) || !Plausible(tach) || !Plausible(drive)) return;
  const uint32_t gears = Read32(base, drive + 60);
  if (!Plausible(gears)) return;

  const float vx = ReadFloat(base, body + 272);
  const float vy = ReadFloat(base, body + 276);
  const float vz = ReadFloat(base, body + 280);
  const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
  static uint32_t last_velocity[3] = {};
  const uint32_t velocity[3] = {Read32(base, body + 272), Read32(base, body + 276),
                                Read32(base, body + 280)};
  if (std::memcmp(velocity, last_velocity, sizeof velocity) == 0) {
    g_mcla_vehicle.frozen_frames.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_mcla_vehicle.frozen_frames.store(0, std::memory_order_relaxed);
    std::memcpy(last_velocity, velocity, sizeof velocity);
  }
  const float nitro =
      static_cast<float>(static_cast<int16_t>(Read16(base, tach + 18))) + ReadFloat(base, tach + 28);
  const float nitro_time = ReadFloat(base, tach + 24);
  const int32_t gear = static_cast<int32_t>(Read32(base, gears + 20)) - 1;

  g_mcla_vehicle.speed.store(speed, std::memory_order_relaxed);
  g_mcla_vehicle.nitro.store(nitro, std::memory_order_relaxed);
  g_mcla_vehicle.nitro_time.store(nitro_time, std::memory_order_relaxed);
  g_mcla_vehicle.gear.store(gear, std::memory_order_relaxed);
  g_mcla_vehicle.hud_updates.fetch_add(1, std::memory_order_relaxed);

  if (!REXCVAR_GET(mcla_telemetry_log)) return;
  static auto next = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(500);
  REXLOG_WARN("telemetry: speed {:.1f} gear {} nitro {:.3f} nitro time {:.3f}", speed, gear, nitro,
              nitro_time);
}
