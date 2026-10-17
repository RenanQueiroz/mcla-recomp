// mcla - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#include "achievement_toast.h"

class MclaApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MclaApp>(new MclaApp(ctx, "mcla",
        PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnPreLaunchModule() override;
  // --mcla_prepare_shaders: the storage is prepared before the module starts, the SDK creates its
  // pipelines, and the game quits here instead of starting (src/shader_prepare.cpp).
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  // Saves what was changed in the pause menu's settings, in case the window is
  // closed while one of its submenus is open (src/settings_menu.cpp).
  bool OnWindowCloseRequested() override;

  // Achievement pop-ups, registered with the ImGui drawer only while one is
  // on screen (src/achievement_toast.h). The SDK's toast is registered for the
  // whole session, which made the presenter repaint at the monitor's refresh
  // rate whatever the frame cap.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    return mcla::MakeOnDemandToast(rex::ReXApp::CreateAchievementNotificationDialog(),
                                   imgui_drawer(), app_context());
  }

 private:
  void InstallCrashTrace();
  void ConfigureFrameTiming();
  void ApplyPcDefaults();
  void FollowCursorSetting();
  void StartProfilerIfRequested();
};
