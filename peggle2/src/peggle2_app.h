// peggle2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

class Peggle2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Peggle2App>(new Peggle2App(ctx, "peggle2",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override;
  bool OnWindowCloseRequested() override;
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  void OnShutdown() override;
};
