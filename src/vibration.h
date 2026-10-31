// Controller vibration: the mcla_vibration switch (src/vibration.cpp).

#pragma once

#include <cstdint>

// Every game update, from mcla_frame_delta (src/frame_timing.cpp), on the game loop's thread: brings the
// controllers in line with the switch. Turned off while a motor runs: the motors are stopped now, without
// waiting for the game to ask. Turned on again: the rumble the game has set in the meantime is sent.
void MclaVibrationFrame();

// Development (mcla_vibration_log): a driver of the input system was asked to set the motors. Called by the
// dev pad's synthetic device (src/dev_pad.cpp), which stands in for the SDL and XInput drivers in a run
// with no controller, so that the log shows what reaches a driver.
void MclaVibrationDriverCall(uint32_t left, uint32_t right);
