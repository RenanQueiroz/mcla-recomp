#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xfile.h>

REXCVAR_DEFINE_STRING(mcla_gpu_backend, "any", "MCLA",
                      "Graphics backend for the Xenos GPU plugin: any, vulkan or d3d12");


std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);

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

// Before the game ends (QUIT GAME, the window's close button): a career save
// that is being written is let finish first, for up to ten seconds. Ending the
// title gives the game's threads only 200 ms, which could cut a save short. A
// save takes well under a second (50 ms measured), and the log says when
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

// Started with no options (a double-click on mcla.exe), the game should just
// run: find the extracted game next to the executable or, as the build leaves
// it, in the repository a few folders up, and load the Xenos GPU plugin.
void MclaApp::OnConfigurePaths(rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return;
  }
  std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
  for (int level = 0; level < 5 && !folder.empty(); ++level) {
    std::error_code error;
    if (std::filesystem::exists(folder / "game" / "default.xex", error)) {
      paths.game_data_root = folder / "game";
      return;
    }
    if (folder == folder.parent_path()) {
      break;
    }
    folder = folder.parent_path();
  }
}

void MclaApp::OnPreSetup(rex::RuntimeConfig& config) {
  config.audio_factory = &CreateMclaAudioSystem;
  // Every controller is player one. The SDK's default gives each pad its own
  // player in connection order and never reuses a number, so a second gamepad
  // device (Steam Input, DS4Windows, a wheel) or a pad that reconnects becomes
  // player two, which has no profile: the game then says it is not signed in
  // and never saves (issue #22). The game is single-player on one console.
  config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = rex::input::CreateDefaultInputSystem(tool_mode);
    if (input) {
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
    }
    return input;
  };
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }

  // Left to the SDK when "any"; it then loads the plugin named by --gpu_plugin
  // with its own default backend.
  std::string backend = REXCVAR_GET(mcla_gpu_backend);
  if (backend != "any") {
    config.graphics = rex::system::LoadGpuPlugin("xenos", backend);
    if (!config.graphics) {
      REXLOG_ERROR("could not load the xenos GPU plugin with backend '{}'", backend);
    }
  }
}

bool MclaApp::OnWindowCloseRequested() {
  MclaWaitForSaves();
  return true;
}

void MclaApp::OnPostSetup() {
  ConfigureFrameTiming();


  // The game probes for loose city data on a "t:" drive that retail consoles
  // do not have. Give it a device so the probes fail with "not found" rather
  // than "no such device". LARecomp does the same.
  if (runtime() && runtime()->file_system()) {
    runtime()->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
  }
}
