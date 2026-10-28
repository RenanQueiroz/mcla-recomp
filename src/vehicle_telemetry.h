// The player car's state as the HUD shows it, captured once per HUD update by
// the mcla_vehicle_telemetry hook (src/vehicle_telemetry.cpp). Read by the
// controller feedback on platforms that have it (ps5/game/ps5_triggers.h).

#pragma once

#include <atomic>
#include <cstdint>

struct MclaVehicleTelemetry {
  std::atomic<float> speed{0.0f};       // m/s, what the speedometer shows
  std::atomic<float> nitro{0.0f};       // car+664: nitro charge in bars, int16 at +18 + float at +28
  std::atomic<float> nitro_time{0.0f};  // car+664: float at +24, counts while nitro burns
  std::atomic<int32_t> gear{0};         // [car+16]+60 -> +20, minus 1: 1.., -1 reverse
  std::atomic<uint32_t> hud_updates{0}; // counts HUD updates: stop outside driving (front end)
  std::atomic<uint32_t> frozen_frames{0}; // HUD updates with the velocity unchanged (pause)
};

inline MclaVehicleTelemetry g_mcla_vehicle;

// In the pause menu: the physics stand still while the HUD keeps updating, so
// a moving car's velocity stops changing. A car standing still has nothing to
// freeze and does not count.
inline bool MclaVehiclePaused() {
  return g_mcla_vehicle.frozen_frames.load(std::memory_order_relaxed) > 20 &&
         g_mcla_vehicle.speed.load(std::memory_order_relaxed) > 0.3f;
}
