// Before the game ends, a career save that is being written is let finish.
// Used by the PC host (QUIT GAME, RESTART GAME and the window's close button,
// src/mcla_app.cpp and src/settings_menu.cpp) and the PS5 host (QUIT GAME,
// ps5/game/main_ps5.cpp).

#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xfile.h>

// A career save being written: a file open for writing on a content package,
// which the SDK mounts at \Device\Content\<n>\ (xam/content_manager.cpp).
static bool SaveInProgress() {
  auto* runtime = rex::Runtime::instance();
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel) {
    return false;
  }
  // GENERIC_WRITE, GENERIC_ALL, FILE_WRITE_DATA, FILE_APPEND_DATA.
  constexpr uint32_t kWriteAccess = 0x40000000u | 0x10000000u | 0x2u | 0x4u;
  for (const auto& file : kernel->object_table()->GetObjectsByType<rex::system::XFile>()) {
    if (file && (file->file_access() & kWriteAccess) &&
        file->device()->mount_path().starts_with("\\Device\\Content\\")) {
      return true;
    }
  }
  return false;
}

// For the dev pad's `saving` (src/dev_pad.cpp, the PC only).
bool MclaSaveInProgress() {
  return SaveInProgress();
}

// A save that is being written is let finish first, for up to ten seconds.
// Ending the title gives the game's threads only 200 ms on the PC, and on the
// PS5 the system closes it at once, either of which could cut a save short.
// A save takes well under a second (50 ms measured), and the log says when
// closing waited for one.
void MclaWaitForSaves() {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  if (!SaveInProgress()) {
    return;
  }
  REXLOG_INFO("closing: a save is being written; waiting for it");
  while (SaveInProgress() && Clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const auto waited =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  if (SaveInProgress()) {
    REXLOG_WARN("closing: the save was still being written after {} ms", waited);
  } else {
    REXLOG_INFO("closing: the save finished after {} ms", waited);
  }
}
