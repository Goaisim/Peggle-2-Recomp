// peggle2 - ReXGlue Recompiled Project

#include "generated/default/peggle2_init.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/platform.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#else
#include <cstdlib>
#include <unistd.h>
#endif

#if REX_PLATFORM_IOS
#include "../ios/ios_bridge.h"
#include <rex/audio/audio_system.h>
#include <rex/input/input_system.h>
#endif

#include <rex/graphics/graphics_system.h>
#include <rex/runtime.h>

#include "peggle2_app.h"
#include "rex_engine_monitor.h"
#include <rex/cvar.h>
#include <rex/system/flags.h>

REXCVAR_DEFINE_STRING(record_input, "", "Input",
                      "Path to record guest input ticks (.rec)");
REXCVAR_DEFINE_STRING(replay_input, "", "Input",
                      "Path to replay guest input ticks (.rec)");
REXCVAR_DEFINE_BOOL(locate_game, false, "Paths",
                    "Force Locate Game Data / ISO prompt on startup");
REXCVAR_DEFINE_STRING(game, "", "Paths",
                      "Explicit path to Peggle 2 folder, .iso, XBLA container, or Default.xex");
REXCVAR_DEFINE_STRING(game_path, "", "Paths",
                      "Saved path to Peggle 2 game source in peggle2.toml");
REXCVAR_DEFINE_BOOL(test_colliders, false, "Debug",
                    "Run 2D UI collider self-checks and exit");

// ---------------------------------------------------------------------------
// Framerate & Setting Presets
// ---------------------------------------------------------------------------
enum TargetFpsOption {
  kFps30 = 0,
  kFps60 = 1,
  kFps72 = 2,
  kFps120 = 3,
  kFps144 = 4,
  kFpsUncapped = 5,
  kFps40 = 6,
  kFpsCount = 7,
};

enum VSyncOption {
  kVSyncOff = 0, // Off (Host Pacer)
  kVSyncOn = 1,  // On (Swapchain)
  kVSyncCount = 2,
};

enum FpsLimiterOption {
  kLimiterMatchTargetHz = 0, // Match Target Hz (Default)
  kLimiterUncapped = 1,      // Uncapped Present
  kLimiterCount = 2,
};

enum ResScaleOption {
  kRes180p = 0,    // 0.25x (180p)
  kRes360p = 1,    // 0.5x (360p)
  kRes720p = 2,    // 1.0x (720p)
  kRes1080p = 3,   // 1.5x (1080p)
  kRes1440p = 4,   // 2.0x (1440p)
  kRes4K = 5,      // 3.0x (4K)
  kResCount = 6,
};

enum TexUpscaleOption {
  kTexOriginal = 0,    // Original 720p
  kTexAniso = 1,       // 2x CPU Bicubic/xBRZ + Anisotropic
  kTexOverride = 2,    // Dump/Override
  kTexCount = 3,
};

enum TelemetryHudOption {
  kHudVisible = 0,
  kHudMinimal = 1,
  kHudHidden = 2,
  kHudCount = 3,
};

enum AudioSyncOption {
  kAudioNormal = 0,
  kAudioSafe = 1,
  kAudioCount = 2,
};

enum InputStyleOption {
  kInputPcMouse = 0,        // PC Mouse (Hover + Click)
  kInputMobileTouch = 1,    // Mobile Touch Sim (Drag Aim + Tap Release)
  kInputControllerOnly = 2, // Controller / Keyboard Only
  kInputStyleCount = 3,
};

enum UiCollidersOption {
  kCollidersOff = 0,
  kCollidersOn = 1,
  kCollidersCount = 2,
};

enum MouseSensOption {
  kSensLow = 0,
  kSensMedium = 1,
  kSensHigh = 2,
  kSensVeryHigh = 3,
  kSensCount = 4,
};

enum MouseSwapOption {
  kSwapAuto = 0,    // Windows setting (SM_SWAPBUTTON)
  kSwapNormal = 1,  // Normal (Left=Primary/Fire, Right=Secondary/Power)
  kSwapSwapped = 2, // Swapped (Right=Primary/Fire, Left=Secondary/Power)
  kSwapCount = 3,
};

enum MouseAimModeOption {
  kAimGuestTurretLock = 0, // 0: Guest Turret Lock (1:1 Direct Angle)
  kAimStickServo = 1,      // 1: Closed-Loop Stick Servo (Real Guest RAM Feedback)
  kAimMouseDelta = 2,      // 2: 1:1 Mouse Delta (Velocity + Auto Slow-Aim)
  kAimTouchSlingshot = 3,  // 3: Mobile Touch Slingshot (Drag to Aim, Release to Fire)
  kAimModeCount = 4,
};

enum MenuModeOption {
  kMenuClickToTarget = 0,   // 0: Click-to-Target Hitboxes (No Hover Jumping)
  kMenuDirectHover = 1,     // 1: Hover + Click (Direct Focus Step)
  kMenuHoverDebounced = 2,  // 2: Hover Auto-Step (90ms Dwell)
  kMenuWheelFlickClick = 3, // 3: Wheel / Flick Step + Click Anywhere
  kMenuModeCount = 4,
};

struct FpsPresetInfo {
  const char* name;
  uint32_t hz;
  std::chrono::nanoseconds interval;
  float dt_sec;
  float dt_ms;
};

static const FpsPresetInfo kFpsPresets[kFpsCount] = {
  { "30 Hz",    30, std::chrono::nanoseconds(33333333), 1.0f / 60.0f, 16.666666f },
  { "60 Hz",    60, std::chrono::nanoseconds(16666667), 1.0f / 60.0f, 16.666666f },
  { "72 Hz",    72, std::chrono::nanoseconds(13888889), 1.0f / 60.0f, 16.666666f },
  { "120 Hz",  120, std::chrono::nanoseconds(8333333),  1.0f / 60.0f, 16.666666f },
  { "144 Hz",  144, std::chrono::nanoseconds(6944444),  1.0f / 60.0f, 16.666666f },
  { "Uncapped",  0, std::chrono::nanoseconds(0),        1.0f / 60.0f, 16.666666f },
  { "40 Hz",    40, std::chrono::nanoseconds(25000000), 1.0f / 60.0f, 16.666666f },
};

#if REX_PLATFORM_IOS
static constexpr int kFpsCycleOrder[] = {
  kFps40, kFps60, kFps120,
};
#else
static constexpr int kFpsCycleOrder[] = {
  kFps40, kFps60, kFps72, kFps120, kFps144, kFpsUncapped,
};
#endif

static int StepFpsPresetIndex(int cur, int dir) {
  constexpr int n = int(sizeof(kFpsCycleOrder) / sizeof(kFpsCycleOrder[0]));
  int pos = 1;  // Default to 60 Hz if currently on legacy kFps30
  for (int i = 0; i < n; ++i) {
    if (kFpsCycleOrder[i] == cur) {
      pos = i;
      break;
    }
  }
  return kFpsCycleOrder[(pos + (dir >= 0 ? 1 : -1) + n) % n];
}

static int TargetFpsIndexFromHz(int32_t hz) {
  switch (hz) {
    case 40: return kFps40;
    case 60: return kFps60;
#if REX_PLATFORM_IOS
    case 72: return kFps60;
    case 120:
    case 144:
    case 0: return kFps120;
#else
    case 72: return kFps72;
    case 120: return kFps120;
    case 144: return kFps144;
    case 0: return kFpsUncapped;
#endif
    default: return -1;
  }
}

enum EdramModeOption {
  kEdramAuto = 0,  // Auto (RTV on iGPU)
  kEdramRtv = 1,   // RTV
  kEdramRov = 2,   // ROV
  kEdramCount = 3,
};

enum TouchControlsOption {
  kTouchControlsShow = 0,
  kTouchControlsHide = 1,
  kTouchControlsCount = 2,
};

#if REX_PLATFORM_IOS
constexpr int kDefaultTargetFpsIdx = kFps60;
constexpr int kDefaultVSync = kVSyncOn;
constexpr int kDefaultTexFilter = kTexOriginal;  // Original 720p bilinear on iOS (no 2x CPU/CAS cost).
constexpr int kDefaultInputStyle = kInputMobileTouch;
constexpr int kDefaultMouseAim = kAimTouchSlingshot;
#else
constexpr int kDefaultTargetFpsIdx = kFps60;
constexpr int kDefaultVSync = kVSyncOff;
constexpr int kDefaultTexFilter = kTexAniso;
constexpr int kDefaultInputStyle = kInputPcMouse;
constexpr int kDefaultMouseAim = kAimGuestTurretLock;
#endif

// Global Interactive Settings State
static std::atomic<int> g_target_fps_index{kDefaultTargetFpsIdx}; // Default: 60 Hz
static std::atomic<int> g_opt_vsync{kDefaultVSync};     // Default: Off (Host Pacer) / On (VSync) on iOS
static std::atomic<int> g_opt_limiter{kLimiterMatchTargetHz}; // Default: Match Target Hz
static std::atomic<int> g_opt_res{kRes720p};        // Default: 1.0x (720p)
static std::atomic<int> g_opt_tex{kDefaultTexFilter}; // Default: 2x Anisotropic (Original 720p on iOS)
static std::atomic<int> g_opt_hud{kHudVisible};     // Default: Visible
static std::atomic<int> g_opt_audio{kAudioSafe};    // Default: Safe
static std::atomic<int> g_opt_input_style{kDefaultInputStyle};   // Default: PC Mouse / iOS Touch
static std::atomic<int> g_opt_ui_colliders{kCollidersOff};  // Default: Off
static std::atomic<int> g_opt_mouse_aim{kDefaultMouseAim}; // Default: Turret Lock / iOS Slingshot
static std::atomic<int> g_opt_menu_mode{kMenuClickToTarget};  // Default: Click-to-Target Hitboxes
static std::atomic<int> g_opt_sens{kSensMedium};            // Default: Medium
static std::atomic<int> g_opt_mouse_swap{kSwapAuto};        // Default: Auto (Windows SM_SWAPBUTTON)
static std::atomic<int> g_opt_touch_controls{kTouchControlsShow};  // Show corner A/B/Start strip
#if REX_PLATFORM_IOS
static std::atomic<int> g_opt_low_power_scale{1};  // Auto 0.75x in Low Power Mode / Serious thermal
#else
static std::atomic<int> g_opt_edram_mode{kEdramAuto};  // Auto (RTV on iGPU) / RTV / ROV
#endif

static std::atomic<bool> g_menu_open{false};
static int g_selected_menu_item = 0;
constexpr int kMenuItemCount = 15;

// Corner touch strip (A / B / Start) + HUD toggle exclusion rect state shared with XInput hook.
static std::atomic<uint16_t> g_ios_corner_held_mask{0};
static std::atomic<uint16_t> g_ios_corner_latch_mask{0};
struct MobileUiExcludeRect {
  std::atomic<bool> active{false};
  std::atomic<float> u0{0.0f}, v0{0.0f}, u1{0.0f}, v1{0.0f};
};
static MobileUiExcludeRect g_ios_hud_toggle_rect;
static MobileUiExcludeRect g_ios_corner_strip_rect;

static void UpdateMobileExcludeRect(MobileUiExcludeRect& dst, bool active,
                                    float x0, float y0, float x1, float y1,
                                    float vp_x, float vp_y, float vp_w, float vp_h) {
  if (!active || vp_w <= 1.0f || vp_h <= 1.0f) {
    dst.active.store(false, std::memory_order_relaxed);
    return;
  }
  dst.u0.store((x0 - vp_x) / vp_w, std::memory_order_relaxed);
  dst.v0.store((y0 - vp_y) / vp_h, std::memory_order_relaxed);
  dst.u1.store((x1 - vp_x) / vp_w, std::memory_order_relaxed);
  dst.v1.store((y1 - vp_y) / vp_h, std::memory_order_relaxed);
  dst.active.store(true, std::memory_order_release);
}

static bool PointInMobileExcludeRect(const MobileUiExcludeRect& r, float u, float v) {
  if (!r.active.load(std::memory_order_acquire)) return false;
  return u >= r.u0.load(std::memory_order_relaxed) &&
         u <= r.u1.load(std::memory_order_relaxed) &&
         v >= r.v0.load(std::memory_order_relaxed) &&
         v <= r.v1.load(std::memory_order_relaxed);
}

struct SettingEntry {
  const char* key;
  std::atomic<int>* value;
  int default_val;
  int count;
};

static const SettingEntry kSettingEntries[kMenuItemCount] = {
  { "target_fps",    &g_target_fps_index,   kDefaultTargetFpsIdx,  kFpsCount },
  { "vsync",         &g_opt_vsync,          kDefaultVSync,         kVSyncCount },
  { "limiter",       &g_opt_limiter,        kLimiterMatchTargetHz, kLimiterCount },
  { "res",           &g_opt_res,            kRes720p,              kResCount },
  { "tex",           &g_opt_tex,            kDefaultTexFilter,     kTexCount },
  { "hud",           &g_opt_hud,            kHudVisible,           kHudCount },
  { "audio",         &g_opt_audio,          kAudioSafe,            kAudioCount },
  { "input_style",   &g_opt_input_style,    kDefaultInputStyle,    kInputStyleCount },
  { "ui_colliders",  &g_opt_ui_colliders,   kCollidersOff,         kCollidersCount },
  { "mouse_aim",     &g_opt_mouse_aim,      kDefaultMouseAim,      kAimModeCount },
  { "menu_mode",     &g_opt_menu_mode,      kMenuClickToTarget,    kMenuModeCount },
  { "sens",          &g_opt_sens,           kSensMedium,           kSensCount },
  { "mouse_swap",    &g_opt_mouse_swap,     kSwapAuto,             kSwapCount },
  { "touch_buttons", &g_opt_touch_controls, kTouchControlsShow,    kTouchControlsCount },
#if REX_PLATFORM_IOS
  { "low_power_scale", &g_opt_low_power_scale, 1,                  2 },
#else
  { "edram_mode",    &g_opt_edram_mode,     kEdramAuto,            kEdramCount },
#endif
};

static std::filesystem::path g_settings_ini_path;
static std::filesystem::path g_active_config_path;
static std::mutex g_settings_io_mutex;
static bool g_ini_had_target_fps = false;
static bool g_ini_had_edram_mode = false;

static void SaveUserSettings() {
  std::lock_guard<std::mutex> lock(g_settings_io_mutex);
  if (g_settings_ini_path.empty()) return;
  std::error_code ec;
  if (!g_settings_ini_path.parent_path().empty()) {
    std::filesystem::create_directories(g_settings_ini_path.parent_path(), ec);
  }
  std::ofstream out(g_settings_ini_path, std::ios::trunc);
  if (!out.is_open()) return;
  out << "# Peggle 2 F1 Settings Menu (auto-saved)\n";
  for (const auto& e : kSettingEntries) {
    out << e.key << "=" << e.value->load(std::memory_order_relaxed) << "\n";
  }
}

static void LoadUserSettings(const std::filesystem::path& path) {
  bool reset_saved_30hz = false;
  {
    std::lock_guard<std::mutex> lock(g_settings_io_mutex);
    g_settings_ini_path = path;
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = line.substr(0, eq);
      int val = 0;
      try {
        val = std::stoi(line.substr(eq + 1));
      } catch (...) {
        continue;
      }
      if (key == "mouse") {
        g_opt_input_style.store(val == 0 ? kInputControllerOnly : kInputPcMouse,
                                std::memory_order_relaxed);
        continue;
      }
      for (const auto& e : kSettingEntries) {
        if (key == e.key && val >= 0 && val < e.count) {
          if (key == "target_fps") {
            g_ini_had_target_fps = true;
            if (val == kFps30) {
              val = kDefaultTargetFpsIdx;
              reset_saved_30hz = true;
            }
#if REX_PLATFORM_IOS
            if (val == kFps72) {
              val = kFps60;
            } else if (val == kFps144 || val == kFpsUncapped) {
              val = kFps120;
            }
#endif
          }
#if !REX_PLATFORM_IOS
          if (key == "edram_mode") {
            g_ini_had_edram_mode = true;
          }
#endif
          e.value->store(val, std::memory_order_relaxed);
          break;
        }
      }
    }
  }
  if (reset_saved_30hz) {
    SaveUserSettings();
  }
}

static void UpdateTomlKeyLine(const std::filesystem::path& config_path,
                              std::initializer_list<const char*> match_keys,
                              const std::string& new_line, bool insert_at_front) {
  if (config_path.empty()) return;
  std::vector<std::string> lines;
  {
    std::ifstream in(config_path);
    std::string line;
    bool replaced = false;
    while (std::getline(in, line)) {
      std::string trimmed = line;
      while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.erase(trimmed.begin());
      }
      bool matched = false;
      if (trimmed.find('=') != std::string::npos) {
        for (const char* k : match_keys) {
          const size_t klen = std::strlen(k);
          if (trimmed.rfind(k, 0) == 0 &&
              (trimmed.size() == klen || trimmed[klen] == ' ' || trimmed[klen] == '\t' ||
               trimmed[klen] == '=')) {
            matched = true;
            break;
          }
        }
      }
      if (matched) {
        if (!replaced) {
          lines.push_back(new_line);
          replaced = true;
        }
      } else {
        lines.push_back(line);
      }
    }
    if (!replaced) {
      if (insert_at_front) {
        lines.insert(lines.begin(), new_line);
      } else {
        lines.push_back(new_line);
      }
    }
  }
  std::error_code ec;
  if (!config_path.parent_path().empty()) {
    std::filesystem::create_directories(config_path.parent_path(), ec);
  }
  std::ofstream out(config_path, std::ios::trunc);
  for (const auto& l : lines) {
    out << l << "\n";
  }
}

#if !REX_PLATFORM_IOS
static void ApplyEdramModeSetting(int mode, bool reinit_backend) {
  const char* val = "";
  if (mode == kEdramRtv) val = "rtv";
  else if (mode == kEdramRov) val = "rov";
  rex::cvar::SetFlagByName("render_target_path_d3d12", val);
  if (reinit_backend) {
    std::filesystem::path cfg = g_active_config_path;
    if (!cfg.empty()) {
      UpdateTomlKeyLine(cfg, {"render_target_path_d3d12"},
                        std::string("render_target_path_d3d12 = \"") + val + "\"", false);
    }
    if (auto* rt = rex::Runtime::instance()) {
      if (auto* gs = rt->graphics_system()) {
        gs->ReinitializeBackendContext();
      }
    }
  }
}
#endif

static void ResetUserSettings() {
  for (const auto& e : kSettingEntries) {
    e.value->store(e.default_val, std::memory_order_relaxed);
  }
#if !REX_PLATFORM_IOS
  ApplyEdramModeSetting(kEdramAuto, true);
#endif
  SaveUserSettings();
}

// Telemetry Counters
static std::atomic<uint32_t> g_vdswap_calls{0};
static std::atomic<uint32_t> g_f2c8_updates{0};
static std::atomic<float> g_telemetry_rendered_fps{60.0f};
static std::atomic<float> g_telemetry_host_fps{60.0f};
static std::atomic<float> g_telemetry_sim_fps{60.0f};
static std::atomic<float> g_telemetry_host_p99_ms{16.67f};
static std::atomic<float> g_telemetry_host_max_ms{16.67f};
static std::atomic<float> g_telemetry_guest_p99_ms{16.67f};
static std::atomic<float> g_telemetry_guest_max_ms{16.67f};
static std::atomic<int16_t> g_telemetry_stick_lx{0};
static std::atomic<uint16_t> g_telemetry_buttons{0};
std::atomic<float> g_vdswap_elapsed_ms{16.667f};

struct FrameTimeSummary {
  size_t count = 0;
  float p50 = 0.0f;
  float p95 = 0.0f;
  float p99 = 0.0f;
  float max = 0.0f;
  uint32_t over20 = 0;
  uint32_t over33 = 0;
};

class FrameTimeWindow {
 public:
  void Push(float ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back(ms);
  }

  FrameTimeSummary Consume() {
    std::vector<float> local;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      local.swap(samples_);
    }
    FrameTimeSummary s{};
    s.count = local.size();
    if (local.empty()) return s;
    for (float v : local) {
      if (v > 20.0f) ++s.over20;
      if (v > 33.34f) ++s.over33;
    }
    std::sort(local.begin(), local.end());
    auto pct = [&](double p) -> float {
      size_t idx = static_cast<size_t>(p * static_cast<double>(local.size() - 1));
      return local[idx];
    };
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    s.max = local.back();
    return s;
  }

 private:
  std::mutex mutex_;
  std::vector<float> samples_;
};

static FrameTimeWindow g_guest_ft_window;
static FrameTimeWindow g_host_ft_window;
static std::atomic<bool> g_mouse_direct_active{false};
static std::atomic<float> g_mouse_target_angle{0.0f};
static std::atomic<float> g_real_guest_turret_angle{0.0f};
static std::atomic<int64_t> g_last_aim_update_ms{0};
static std::atomic<uint64_t> g_current_frame{0};
static std::atomic<uint64_t> g_guest_gameplay_frame_tick{0};
static std::atomic<uint32_t> g_last_board_obj{0};
static std::atomic<bool> g_touch_dragging{false};
static std::atomic<float> g_hud_cursor_nx{0.5f};
static std::atomic<float> g_hud_cursor_ny{0.5f};
static std::atomic<bool> g_hud_cursor_valid{false};
static std::atomic<int> g_hud_flash_zone{-1}; // 0=power, 1=pause, 2=playfield
static std::atomic<int> g_hud_flash_ticks{0};
static std::atomic<const char*> g_telemetry_hover_id{"none"};

// ---------------------------------------------------------------------------
// Engine-Monitored 2D Collider & Gameplay Zone Helpers
// ---------------------------------------------------------------------------
struct ViewportRect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 1280.0f;
  float h = 720.0f;
};

static inline ViewportRect Compute16x9Viewport(float client_w, float client_h) {
  constexpr float kTargetAspect = 16.0f / 9.0f;
  ViewportRect vp{0.0f, 0.0f, client_w, client_h};
  if (client_w <= 1.0f || client_h <= 1.0f) return vp;
  float client_aspect = client_w / client_h;
  if (client_aspect > kTargetAspect) {
    vp.w = client_h * kTargetAspect;
    vp.x = 0.5f * (client_w - vp.w);
  } else if (client_aspect < kTargetAspect) {
    vp.h = client_w / kTargetAspect;
    vp.y = 0.5f * (client_h - vp.h);
  }
  return vp;
}

// True 1280x720 Balljector pivot (741, 45) + parabolic gravity arc compensation adopted from peggle2-mac.
static float ComputeBallisticAimAngle(float norm_x, float norm_y) {
  constexpr float kGuestW = 1280.0f;
  constexpr float kGuestH = 720.0f;
  constexpr float kGunPivotX = 741.0f;
  constexpr float kGunPivotY = 45.0f;
  constexpr float kAimMuzzle = 80.0f;
  constexpr float kAimCurvature = 0.0011f;

  const float dx0 = norm_x * kGuestW - kGunPivotX;
  const float dy0 = (std::max)(15.0f, norm_y * kGuestH - kGunPivotY);
  const float d0 = std::hypot(dx0, dy0);
  if (d0 < 1.0f) return 0.0f;

  float ux = dx0 / d0;
  float uy = dy0 / d0;
  for (int iter = 0; iter < 3; ++iter) {
    const float dx = (d0 > kAimMuzzle + 10.0f) ? (dx0 - ux * kAimMuzzle) : dx0;
    const float dy = (d0 > kAimMuzzle + 10.0f) ? (dy0 - uy * kAimMuzzle) : dy0;
    const float X = std::fabs(dx);
    const float Y = dy;
    const float a = kAimCurvature * X * X * 0.5f;
    const float disc = X * X - 4.0f * a * (a - Y);
    if (X > 1.0f && a > 1e-6f && disc >= 0.0f) {
      const float sx = std::copysign(1.0f, dx);
      const float sy = (-X + std::sqrt(disc)) / (2.0f * a);
      const float len = std::hypot(sx, sy);
      if (len > 1e-6f) {
        ux = sx / len;
        uy = sy / len;
        continue;
      }
    }
    const float nl = std::hypot(dx, dy);
    if (nl > 1e-6f) {
      ux = dx / nl;
      uy = dy / nl;
    }
  }
  return std::clamp(std::atan2(ux, uy), -1.48f, 1.48f);
}

// Xbox 360 left-stick inner deadzone is 7849; map angular error past deadzone with LT fine-aim.
static void ComputeDeadzoneCompensatedStick(float error_rad, int16_t& out_lx, uint8_t& out_lt) {
  const float abs_err = std::fabs(error_rad);
  if (abs_err <= 0.0035f) {
    out_lx = 0;
    out_lt = 0;
    return;
  }
  const float sign = (error_rad > 0.0f) ? 1.0f : -1.0f;
  if (abs_err <= 0.075f) {
    out_lt = 255;
    const float mag = 8300.0f + (abs_err / 0.075f) * 24400.0f;
    out_lx = static_cast<int16_t>(std::clamp(sign * mag, -32767.0f, 32767.0f));
  } else {
    out_lt = 0;
    const float mag = (std::min)(32767.0f, 14000.0f + (abs_err / 0.35f) * 18767.0f);
    out_lx = static_cast<int16_t>(std::clamp(sign * mag, -32767.0f, 32767.0f));
  }
}

struct GameplayZone {
  const char* id;
  const char* label;
  float x0, y0, x1, y1;
  uint16_t button_mask;
  bool is_aim_zone;
};

static constexpr GameplayZone kGameplayZones[3] = {
  {"hud_power", "Master Power [X]", 0.010f, 0.350f, 0.200f, 0.920f, 0x4000, false},
  {"hud_pause", "Pause [START]",    0.900f, 0.010f, 0.990f, 0.220f, 0x0010, false},
  {"board_aim", "Aim / Fire [A]",   0.205f, 0.010f, 0.985f, 0.985f, 0x1000, true},
};

static int HitTestGameplayZones(float nx, float ny) {
  if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f) return -1;
  for (int i = 0; i < 3; ++i) {
    if (nx >= kGameplayZones[i].x0 && nx <= kGameplayZones[i].x1 &&
        ny >= kGameplayZones[i].y0 && ny <= kGameplayZones[i].y1) {
      return i;
    }
  }
  return -1;
}

static void ForceTerminateProcess(int code) {
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#else
  std::_Exit(code);
#endif
}

static bool IsEnvFlagEnabled(const char* name) {
#ifdef _WIN32
  char buf[32]{};
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n > 0 && n < sizeof(buf) && buf[0] != '0';
#else
  const char* v = std::getenv(name);
  return v && v[0] && v[0] != '0';
#endif
}

static void AppendTcFileLog(const char* path, const char* msg) {
  std::ofstream f(path, std::ios::app);
  if (f.is_open()) {
    f << msg;
    f.flush();
  }
}

static void ArmStartupEnvWatchdogOnce() {
  static std::atomic<bool> s_armed{false};
  const bool tc = IsEnvFlagEnabled("PEGGLE_TEST_COLLIDERS");
  const bool ar = IsEnvFlagEnabled("PEGGLE_AUTO_REPLAY");
  if (!tc && !ar) return;
  if (s_armed.exchange(true, std::memory_order_relaxed)) return;
  if (tc) {
    AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log",
                    "[TC] Startup init: PEGGLE_TEST_COLLIDERS=1 -> OS watchdog armed (18000ms)\n");
  }
  const uint32_t sleep_ms = tc ? 18000u : 55000u;
  std::thread([sleep_ms]() {
    {
      std::ofstream wf("C:/Peggle2Recomp/tc_watchdog.log", std::ios::trunc);
      if (wf.is_open()) {
        wf << "Watchdog started\n";
        wf.flush();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    AppendTcFileLog("C:/Peggle2Recomp/tc_watchdog.log", "Watchdog timeout -> TerminateProcess\n");
    if (IsEnvFlagEnabled("PEGGLE_TEST_COLLIDERS")) {
      AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Watchdog 18000ms timeout -> TerminateProcess(0)\n");
    }
    rex_monitor::FlushReplayHitboxAudit();
    rex_monitor::FlushLog();
    fflush(stdout);
    ForceTerminateProcess(0);
  }).detach();
}

static const int g_env_watchdog_bootstrap = []() {
  ArmStartupEnvWatchdogOnce();
  return 1;
}();

#define TC_ASSERT(cond)                                                            \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      char tc_buf_[320];                                                           \
      std::snprintf(tc_buf_, sizeof(tc_buf_), "[TC] FAIL line %d: %s\n", __LINE__, \
                    #cond);                                                        \
      AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", tc_buf_);                   \
      fprintf(stderr, "%s", tc_buf_);                                              \
      fflush(stderr);                                                              \
      ForceTerminateProcess(1);                                                    \
    }                                                                              \
  } while (0)

static void RunColliderSelfCheck() {
  const bool tc_log = IsEnvFlagEnabled("PEGGLE_TEST_COLLIDERS");
  if (tc_log) {
    AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] RunColliderSelfCheck: begin\n");
  }
  ViewportRect vp169 = Compute16x9Viewport(1920.0f, 1080.0f);
  TC_ASSERT(std::abs(vp169.x - 0.0f) < 0.01f && std::abs(vp169.w - 1920.0f) < 0.01f);
  ViewportRect vp_ultra = Compute16x9Viewport(2560.0f, 1080.0f);
  TC_ASSERT(std::abs(vp_ultra.w - 1920.0f) < 0.01f && std::abs(vp_ultra.x - 320.0f) < 0.01f);

  // Synthetic GPU 2D quad snapshot: 2 stacked rows of menu buttons, highlight starts top-left.
  rex_monitor::ClearTestState();
  RexGpuUiMonitorSnapshotMirror snap{};
  snap.frame_index = 100;
  snap.quad_count = 3;
  snap.quads[0] = {0.20f, 0.20f, 0.40f, 0.32f, 1, 1, 1, 1, 2, 0x01, 0, 80.0f}; // Top-left
  snap.quads[1] = {0.60f, 0.20f, 0.80f, 0.32f, 1, 1, 1, 2, 2, 0x00, 0, 0.0f};  // Top-right
  snap.quads[2] = {0.20f, 0.58f, 0.40f, 0.70f, 1, 1, 1, 3, 2, 0x00, 0, 0.0f};  // Bottom-left
  snap.has_highlight = 1;
  snap.highlight_rect = snap.quads[0];
  rex_monitor::InjectSyntheticSnapshotForTest(snap);

  // Clicking empty dead space with desktop mouse must emit 0; a quick mobile touch tap on
  // unmapped cutscenes / dialogue bubbles (tgt_idx < 0, is_touch_tap=true) must emit A (0x1000).
  auto dead = rex_monitor::StepMenuFocusServo(0.05f, 0.05f, true, true, false, 1000);
  TC_ASSERT(!dead.has_target && dead.buttons_or_mask == 0);
  auto dialogue_tap = rex_monitor::StepMenuFocusServo(0.05f, 0.05f, false, true, true, 1050);
  TC_ASSERT(!dialogue_tap.has_target && dialogue_tap.buttons_or_mask == 0x1000);

  // Hovering top-right while highlight is top-left must emit DPAD_RIGHT (0x0008).
  auto right_step = rex_monitor::StepMenuFocusServo(0.70f, 0.26f, true, false, false, 1200);
  TC_ASSERT(right_step.has_target && !right_step.aligned && right_step.buttons_or_mask == 0x0008);

  // Hovering bottom-left while highlight is top-left must emit DPAD_DOWN (0x0002).
  auto down_step = rex_monitor::StepMenuFocusServo(0.30f, 0.64f, true, false, false, 1400);
  TC_ASSERT(down_step.has_target && !down_step.aligned && down_step.buttons_or_mask == 0x0002);

  // Clicking top-left when highlight is aligned on top-left must emit A (0x1000).
  auto aligned_click = rex_monitor::StepMenuFocusServo(0.30f, 0.26f, true, true, false, 1600);
  TC_ASSERT(aligned_click.has_target && aligned_click.aligned && aligned_click.buttons_or_mask == 0x1000);

  // Gameplay fixed zones: HUD Power [X], Pause [START], Playfield Aim [A].
  TC_ASSERT(HitTestGameplayZones(0.10f, 0.60f) == 0);
  TC_ASSERT(HitTestGameplayZones(0.95f, 0.08f) == 1);
  TC_ASSERT(HitTestGameplayZones(0.50f, 0.50f) == 2);
  TC_ASSERT(HitTestGameplayZones(0.10f, 0.10f) == -1);
  if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 0 (synthetic quads + zones) passed\n");

  rex_monitor::ClearTestState();

  // Synthetic 320x180 Main Menu pixel test: UI Blue buttons + Yellow/Gold highlighted MULTIPLAYER.
  {
    constexpr uint32_t W = 320, H = 180;
    std::vector<uint8_t> frame(size_t(W) * H * 4, 0);
    auto fill_rect = [&](float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b) {
      const uint32_t px0 = uint32_t(x0 * W), py0 = uint32_t(y0 * H);
      const uint32_t px1 = uint32_t(x1 * W), py1 = uint32_t(y1 * H);
      const uint8_t luma = uint8_t((77u * r + 150u * g + 29u * b) >> 8);
      for (uint32_t y = py0; y < py1 && y < H; ++y) {
        for (uint32_t x = px0; x < px1 && x < W; ++x) {
          size_t off = (size_t(y) * W + x) * 4;
          frame[off + 0] = r;
          frame[off + 1] = g;
          frame[off + 2] = b;
          frame[off + 3] = luma;
        }
      }
    };
    fill_rect(0.24f, 0.18f, 0.34f, 0.32f, 180, 215, 245); // Glacier mountain (Landmark A)
    fill_rect(0.50f, 0.74f, 0.65f, 0.82f, 50, 155, 55);   // Green grass patch
    fill_rect(0.02f, 0.89f, 0.15f, 0.97f, 200, 40, 40);   // Red BACK
    fill_rect(0.16f, 0.91f, 0.83f, 0.99f, 20, 95, 110);   // Dark teal bar
    fill_rect(0.84f, 0.89f, 0.98f, 0.97f, 40, 195, 70);   // Green SELECT
    fill_rect(0.405f, 0.210f, 0.730f, 0.340f, 40, 140, 220);  // UI Blue SINGLE PLAYER
    fill_rect(0.440f, 0.365f, 0.760f, 0.485f, 240, 195, 30);  // Yellow/Gold MULTIPLAYER
    fill_rect(0.270f, 0.560f, 0.375f, 0.715f, 40, 140, 220);  // MENU BTN 0
    fill_rect(0.395f, 0.560f, 0.495f, 0.715f, 40, 140, 220);  // MENU BTN 1
    fill_rect(0.525f, 0.560f, 0.630f, 0.715f, 40, 140, 220);  // MENU BTN 2
    fill_rect(0.655f, 0.560f, 0.760f, 0.715f, 40, 140, 220);  // MENU BTN 3
    fill_rect(0.785f, 0.560f, 0.890f, 0.715f, 40, 140, 220);  // MENU BTN 4 (Exit)
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 1);
    rex_monitor::SetScreenForTest(SCREEN_MAIN_MENU);

    auto up_step = rex_monitor::StepMenuFocusServo(0.55f, 0.27f, true, false, false, 2000);
    TC_ASSERT(up_step.has_target && up_step.has_highlight && !up_step.aligned &&
              up_step.buttons_or_mask == 0x0001);  // DPAD_UP to SINGLE PLAYER

    RexScreenWatcherStatus st{};
    std::vector<RexCandidateButton> cands;
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU && cands.size() == 9 && st.has_cursor_rgb &&
              st.cursor_r == 40 && st.cursor_g == 140 && st.cursor_b == 220);

    // Clicking empty background (Target == None) must emit 0 and never fire A.
    auto miss_mp = rex_monitor::StepMenuFocusServo(0.60f, 0.50f, true, true, false, 2050);
    TC_ASSERT(!miss_mp.has_target && miss_mp.buttons_or_mask == 0);
    rex_monitor::StepMenuFocusServo(0.60f, 0.50f, true, false, false, 2450);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU);

    // Confirm hovering MENU BTN 4 (Exit door) stays on MAIN_MENU
    auto exit_step = rex_monitor::StepMenuFocusServo(0.83f, 0.64f, true, false, false, 2500);
    TC_ASSERT(exit_step.has_target && std::strcmp(exit_step.target_name, "MENU BTN 4") == 0);

    // Strict Row-2 Horizontal Rule + Store (MENU BTN 2) lower-half gold sample under teal banner
    fill_rect(0.440f, 0.365f, 0.760f, 0.485f, 40, 140, 220);  // MULTIPLAYER back to blue
    fill_rect(0.525f, 0.560f, 0.630f, 0.630f, 25, 110, 125);  // Teal banner
    fill_rect(0.525f, 0.635f, 0.630f, 0.715f, 240, 195, 30);  // Gold lower border on MENU BTN 2
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 2);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU && st.hl_row == 2 && st.hl_col == 2);
    auto row2_right1 = rex_monitor::StepMenuFocusServo(0.83f, 0.64f, true, false, false, 2600);
    TC_ASSERT(row2_right1.has_target && row2_right1.buttons_or_mask == 0x0008);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 3);
    auto row2_right2 = rex_monitor::StepMenuFocusServo(0.83f, 0.64f, true, false, false, 2800);
    TC_ASSERT(row2_right2.buttons_or_mask == 0x0008);  // Never falls back to DPAD_UP / DPAD_DOWN
    rex_monitor::ClearTestState();
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 0b (main menu pixels) passed\n");
  }

  // Click-locked state machine + Fix 1..5 regression tests.
  {
    constexpr uint32_t W = 320, H = 180;
    std::vector<uint8_t> frame(size_t(W) * H * 4, 0);
    auto clear_frame = [&](uint8_t r, uint8_t g, uint8_t b) {
      const uint8_t luma = uint8_t((77u * r + 150u * g + 29u * b) >> 8);
      for (size_t i = 0; i < size_t(W) * H; ++i) {
        frame[i * 4 + 0] = r;
        frame[i * 4 + 1] = g;
        frame[i * 4 + 2] = b;
        frame[i * 4 + 3] = luma;
      }
    };
    auto fill_rect = [&](float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b) {
      const uint32_t px0 = uint32_t(x0 * W), py0 = uint32_t(y0 * H);
      const uint32_t px1 = uint32_t(x1 * W), py1 = uint32_t(y1 * H);
      const uint8_t luma = uint8_t((77u * r + 150u * g + 29u * b) >> 8);
      for (uint32_t y = py0; y < py1 && y < H; ++y) {
        for (uint32_t x = px0; x < px1 && x < W; ++x) {
          size_t off = (size_t(y) * W + x) * 4;
          frame[off + 0] = r;
          frame[off + 1] = g;
          frame[off + 2] = b;
          frame[off + 3] = luma;
        }
      }
    };
    auto add_bottom_bar = [&]() {
      fill_rect(0.02f, 0.90f, 0.17f, 0.98f, 200, 40, 40);
      fill_rect(0.18f, 0.91f, 0.82f, 0.99f, 20, 95, 110);
      fill_rect(0.83f, 0.90f, 0.98f, 0.98f, 40, 195, 70);
    };
    auto draw_main_menu = [&](int hl_item) {
      clear_frame(95, 165, 225);
      add_bottom_bar();
      fill_rect(0.24f, 0.18f, 0.34f, 0.32f, 180, 215, 245);
      fill_rect(0.405f, 0.210f, 0.730f, 0.340f, hl_item == 0 ? 240 : 40, hl_item == 0 ? 195 : 140, hl_item == 0 ? 30 : 220);
      fill_rect(0.440f, 0.365f, 0.760f, 0.485f, hl_item == 1 ? 240 : 40, hl_item == 1 ? 195 : 140, hl_item == 1 ? 30 : 220);
      fill_rect(0.270f, 0.560f, 0.375f, 0.715f, 40, 140, 220);
      fill_rect(0.395f, 0.560f, 0.495f, 0.715f, 40, 140, 220);
      fill_rect(0.525f, 0.560f, 0.630f, 0.630f, 25, 110, 125);
      fill_rect(0.525f, 0.635f, 0.630f, 0.715f, hl_item == 2 ? 240 : 40, hl_item == 2 ? 195 : 140, hl_item == 2 ? 30 : 220);
      fill_rect(0.655f, 0.560f, 0.760f, 0.715f, hl_item == 3 ? 240 : 40, hl_item == 3 ? 195 : 140, hl_item == 3 ? 30 : 220);
      fill_rect(0.785f, 0.560f, 0.890f, 0.715f, 40, 140, 220);
    };
    auto draw_help_options = [&](int hl_row) {
      clear_frame(45, 70, 95);
      add_bottom_bar();
      fill_rect(0.32f, 0.15f, 0.59f, 0.89f, 35, 120, 200);
      for (int r = 0; r < 4; ++r) {
        const float y0 = 0.31f + float(r) * 0.11f;
        const bool g = (r == hl_row);
        fill_rect(0.35f, y0, 0.57f, y0 + 0.08f, g ? 240 : 40, g ? 195 : 140, g ? 30 : 220);
      }
    };
    auto draw_mp_menu = [&](int hl_row) {
      clear_frame(95, 165, 225);
      add_bottom_bar();
      static constexpr float kMpY[4] = {0.19f, 0.36f, 0.53f, 0.70f};
      for (int r = 0; r < 4; ++r) {
        const bool g = (r == hl_row);
        fill_rect(r == 0 ? 0.42f : 0.44f, kMpY[r], r == 0 ? 0.73f : 0.71f, kMpY[r] + 0.13f,
                  g ? 240 : 40, g ? 195 : 140, g ? 30 : 220);
      }
    };

    RexScreenWatcherStatus st{};
    std::vector<RexCandidateButton> cands;

    // 1. Starts on SCREEN_TITLE with Count=0 during EA white splash & black autosave; empty clicks do nothing.
    rex_monitor::ClearTestState();
    clear_frame(245, 245, 245);  // White EA splash
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 10);
    auto splash_click = rex_monitor::StepMenuFocusServo(0.50f, 0.86f, true, true, false, 500);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_TITLE && cands.empty() && splash_click.buttons_or_mask == 0);

    clear_frame(10, 12, 18);     // Black autosave warning
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 11);
    auto save_click = rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, true, false, 950);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_TITLE && cands.empty() && save_click.buttons_or_mask == 0);

    // PLAY_BTN appears on Title Screen -> clicking sky is ignored; clicking PLAY_BTN -> MAIN_MENU
    clear_frame(90, 160, 220);
    fill_rect(0.15f, 0.05f, 0.85f, 0.35f, 180, 220, 250);
    fill_rect(0.395f, 0.815f, 0.605f, 0.915f, 40, 140, 220);  // Blue PLAY_BTN
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 12);
    auto title_miss = rex_monitor::StepMenuFocusServo(0.50f, 0.30f, true, true, false, 1000);
    TC_ASSERT(!title_miss.has_target && title_miss.buttons_or_mask == 0);
    auto title_click = rex_monitor::StepMenuFocusServo(0.50f, 0.86f, true, true, false, 1050);
    TC_ASSERT(title_click.has_target && std::strcmp(title_click.target_name, "PLAY_BTN") == 0 &&
              title_click.buttons_or_mask == 0x1000);
    draw_main_menu(2);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 13);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 1500);  // >= 450ms -> MAIN MENU
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU && cands.size() == 9);

    // Blue OK (A) Modal detection (dimmed footer bar + UI Blue OK button) & strict OK_BTN hitbox gating
    auto hover_store = rex_monitor::StepMenuFocusServo(0.57f, 0.65f, true, false, false, 1600);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU && hover_store.has_target &&
              std::strcmp(hover_store.target_name, "MENU BTN 2") == 0);

    auto store_click = rex_monitor::StepMenuFocusServo(0.57f, 0.65f, true, true, false, 1700);
    TC_ASSERT(store_click.buttons_or_mask == 0x1000);
    clear_frame(20, 20, 30);  // Dimmed footer (< 65 luma on both bottom corners)
    fill_rect(0.28f, 0.22f, 0.72f, 0.76f, 35, 130, 220);       // Modal blue body
    fill_rect(0.415f, 0.840f, 0.585f, 0.925f, 40, 140, 220);   // UI Blue OK (A) button
    fill_rect(0.535f, 0.850f, 0.578f, 0.915f, 40, 200, 75);    // Bright green (A) badge
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 14);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 2150);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MODAL_OK_POPUP && cands.size() == 1);
    auto ok_outside = rex_monitor::StepMenuFocusServo(0.10f, 0.15f, true, true, false, 2170);
    TC_ASSERT(!ok_outside.has_target && ok_outside.buttons_or_mask == 0);
    auto ok_inside = rex_monitor::StepMenuFocusServo(0.50f, 0.88f, true, true, false, 2200);
    TC_ASSERT(ok_inside.has_target && ok_inside.buttons_or_mask == 0x1000);
    draw_main_menu(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 15);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 2650);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU);
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 1 (title + modal ok) passed\n");

    // 2. HELP & OPTIONS + SETTINGS MENU (Count=7) round-trip
    auto opt_click = rex_monitor::StepMenuFocusServo(0.70f, 0.64f, true, true, false, 2700);
    TC_ASSERT(opt_click.buttons_or_mask == 0x1000);
    draw_help_options(0);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 16);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 3150);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_HELP_OPTIONS && cands.size() == 6);

    auto htp_click = rex_monitor::StepMenuFocusServo(0.46f, 0.35f, true, true, false, 3200);
    TC_ASSERT(htp_click.buttons_or_mask == 0x1000);
    clear_frame(45, 70, 95);
    add_bottom_bar();
    fill_rect(0.28f, 0.18f, 0.72f, 0.80f, 35, 120, 200);  // Info panel (no buttons/Accept)
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 160);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 3650);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_INFO_SUBMENU && cands.size() == 2);

    rex_monitor::NotifyMenuActionInput(0x2000, 3700);
    draw_help_options(1);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 17);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 4150);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_HELP_OPTIONS);

    auto set_open = rex_monitor::StepMenuFocusServo(0.46f, 0.46f, true, true, false, 4200);
    TC_ASSERT(set_open.buttons_or_mask == 0x1000);
    clear_frame(35, 50, 70);
    add_bottom_bar();
    fill_rect(0.22f, 0.12f, 0.78f, 0.86f, 35, 120, 205);       // Wide octagonal blue Settings frame
    fill_rect(0.380f, 0.355f, 0.630f, 0.435f, 240, 195, 30);    // SET_COLORBLIND highlighted
    fill_rect(0.285f, 0.468f, 0.745f, 0.542f, 40, 140, 220);    // SET_SOUND_VOL
    fill_rect(0.285f, 0.576f, 0.745f, 0.650f, 40, 140, 220);    // SET_MUSIC_VOL
    fill_rect(0.285f, 0.680f, 0.745f, 0.755f, 40, 140, 220);    // SET_SCREEN_SIZE
    fill_rect(0.415f, 0.770f, 0.585f, 0.848f, 40, 140, 220);    // SET_ACCEPT
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 18);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 4650);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_SETTINGS_MENU && cands.size() == 7);

    auto cb_click = rex_monitor::StepMenuFocusServo(0.50f, 0.39f, true, true, false, 4700);
    TC_ASSERT(cb_click.has_target && std::strcmp(cb_click.target_name, "SET_COLORBLIND") == 0 &&
              cb_click.buttons_or_mask == 0x1000);
    fill_rect(0.380f, 0.355f, 0.630f, 0.435f, 40, 140, 220);
    fill_rect(0.285f, 0.468f, 0.745f, 0.542f, 240, 195, 30);    // Highlight SET_SOUND_VOL
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 180);
    auto set_left = rex_monitor::StepMenuFocusServo(0.54f, 0.50f, true, true, false, 4800);
    TC_ASSERT(set_left.has_target && set_left.aligned && set_left.buttons_or_mask == 0x0004);
    auto set_right = rex_monitor::StepMenuFocusServo(0.68f, 0.50f, true, true, false, 4900);
    TC_ASSERT(set_right.has_target && set_right.aligned && set_right.buttons_or_mask == 0x0008);
    fill_rect(0.285f, 0.468f, 0.745f, 0.542f, 40, 140, 220);
    fill_rect(0.415f, 0.770f, 0.585f, 0.848f, 240, 195, 30);    // Highlight SET_ACCEPT
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 181);
    auto set_accept = rex_monitor::StepMenuFocusServo(0.50f, 0.81f, true, true, false, 5000);
    TC_ASSERT(set_accept.has_target && std::strcmp(set_accept.target_name, "SET_ACCEPT") == 0 &&
              set_accept.buttons_or_mask == 0x1000);
    draw_help_options(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 19);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 5450);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_HELP_OPTIONS);

    auto done_click = rex_monitor::StepMenuFocusServo(0.46f, 0.68f, true, true, false, 5500);
    TC_ASSERT(done_click.buttons_or_mask == 0x1000);
    draw_main_menu(0);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 20);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 5950);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MAIN_MENU);
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 2 (help + settings) passed\n");

    // 3. ADVENTURE (PEGGLE INSTITUTE vs MOUNTAIN HIGHS alignment + tightened hover zones)
    auto sp_click = rex_monitor::StepMenuFocusServo(0.55f, 0.27f, true, true, false, 6000);
    TC_ASSERT(sp_click.buttons_or_mask == 0x1000);

    // PEGGLE INSTITUTE: ADV_LEVELS [0.165..0.280], ADV_TRIALS [0.288..0.405], no ADV_PREV_WORLD, ADV_NEXT_WORLD >= 0.58
    clear_frame(110, 160, 130);
    add_bottom_bar();
    fill_rect(0.02f, 0.02f, 0.18f, 0.10f, 175, 100, 45);   // Wooden ADVENTURE sign
    fill_rect(0.41f, 0.46f, 0.45f, 0.68f, 40, 48, 60);     // Dark card border must NOT flip world 0 -> 1
    fill_rect(0.165f, 0.445f, 0.280f, 0.705f, 240, 195, 30);
    fill_rect(0.288f, 0.445f, 0.405f, 0.705f, 40, 140, 220);
    fill_rect(0.580f, 0.340f, 0.960f, 0.740f, 40, 125, 200);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 21);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 6450);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_ADVENTURE_CAROUSEL && cands.size() == 5 &&
              std::strcmp(cands[0].name, "ADV_LEVELS") == 0 && cands[0].x0 < 0.17f);
    // Hovering Bjorn (x=0.10) or dead zone before 0.58 (x=0.50) must NOT trigger scroll
    auto bjorn_hover = rex_monitor::StepMenuFocusServo(0.10f, 0.55f, true, false, false, 6460);
    auto mid_hover = rex_monitor::StepMenuFocusServo(0.50f, 0.55f, true, false, false, 6470);
    TC_ASSERT(!bjorn_hover.has_target && bjorn_hover.buttons_or_mask == 0 &&
              !mid_hover.has_target && mid_hover.buttons_or_mask == 0);

    // Hover ADV_NEXT_WORLD -> 2 steps on entry, cooldown < 0.65s, repeats at >= 0.65s
    auto nw_1 = rex_monitor::StepMenuFocusServo(0.75f, 0.55f, true, false, false, 6500);
    clear_frame(165, 205, 185);
    add_bottom_bar();
    fill_rect(0.02f, 0.02f, 0.18f, 0.10f, 175, 100, 45);
    fill_rect(0.165f, 0.445f, 0.278f, 0.705f, 40, 140, 220);
    fill_rect(0.285f, 0.445f, 0.402f, 0.705f, 240, 195, 30);
    fill_rect(0.580f, 0.340f, 0.960f, 0.740f, 40, 125, 200);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 200);
    auto nw_2 = rex_monitor::StepMenuFocusServo(0.75f, 0.55f, true, false, false, 6600);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 201);
    auto nw_wait = rex_monitor::StepMenuFocusServo(0.75f, 0.55f, true, false, false, 7000);
    auto nw_rep = rex_monitor::StepMenuFocusServo(0.75f, 0.55f, true, false, false, 7260);
    TC_ASSERT(nw_1.buttons_or_mask == 0x0008 && nw_2.buttons_or_mask == 0x0008 &&
              nw_wait.buttons_or_mask == 0 && nw_rep.buttons_or_mask == 0x0008);

    // CELESTIAL REALM (no right card in [0.60..0.92, 0.45..0.70]): Count=5 without ADV_NEXT_WORLD
    clear_frame(110, 160, 130);
    add_bottom_bar();
    fill_rect(0.02f, 0.02f, 0.18f, 0.10f, 175, 100, 45);
    fill_rect(0.228f, 0.445f, 0.342f, 0.705f, 240, 195, 30);
    fill_rect(0.348f, 0.445f, 0.462f, 0.705f, 40, 140, 220);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 202);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_ADVENTURE_CAROUSEL && cands.size() == 5 &&
              std::strcmp(cands[0].name, "ADV_PREV_WORLD") == 0);
    auto cel_next_miss = rex_monitor::StepMenuFocusServo(0.75f, 0.55f, true, true, false, 7320);
    TC_ASSERT(!cel_next_miss.has_target && cel_next_miss.buttons_or_mask == 0);
    auto cel_prev = rex_monitor::StepMenuFocusServo(0.10f, 0.55f, true, false, false, 7420);
    TC_ASSERT(cel_prev.has_target && std::strcmp(cel_prev.target_name, "ADV_PREV_WORLD") == 0 &&
              cel_prev.buttons_or_mask == 0x0004);

    // Slide completes to MOUNTAIN HIGHS ([0.228..0.342] & [0.348..0.462]) -> Count=6 + deadzone gap [0.155..0.228]
    fill_rect(0.580f, 0.340f, 0.960f, 0.740f, 40, 125, 200);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 22);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_ADVENTURE_CAROUSEL && cands.size() == 6 &&
              std::strcmp(cands[0].name, "ADV_PREV_WORLD") == 0);
    auto gap_hover = rex_monitor::StepMenuFocusServo(0.18f, 0.55f, true, false, false, 7520);
    TC_ASSERT(!gap_hover.has_target && gap_hover.buttons_or_mask == 0);
    auto pw_click = rex_monitor::StepMenuFocusServo(0.10f, 0.55f, true, true, false, 7630);
    TC_ASSERT(pw_click.has_target && std::strcmp(pw_click.target_name, "ADV_PREV_WORLD") == 0 &&
              pw_click.buttons_or_mask == 0x0004);

    // Click MOUNTAIN HIGHS ADV_LEVELS -> LEVEL_SELECT
    auto adv_go = rex_monitor::StepMenuFocusServo(0.28f, 0.55f, true, true, false, 7720);
    TC_ASSERT(adv_go.has_target && std::strcmp(adv_go.target_name, "ADV_LEVELS") == 0 &&
              adv_go.buttons_or_mask == 0x1000);
    clear_frame(95, 165, 225);
    add_bottom_bar();
    fill_rect(0.20f, 0.14f, 0.80f, 0.84f, 35, 125, 200);
    static constexpr struct { float x0, y0, x1, y1; } kHexRects[10] = {
      {0.212f, 0.148f, 0.280f, 0.264f}, {0.248f, 0.274f, 0.326f, 0.390f},
      {0.320f, 0.195f, 0.394f, 0.310f}, {0.362f, 0.308f, 0.436f, 0.422f},
      {0.438f, 0.248f, 0.512f, 0.362f}, {0.514f, 0.190f, 0.588f, 0.304f},
      {0.548f, 0.306f, 0.624f, 0.420f}, {0.626f, 0.246f, 0.700f, 0.360f},
      {0.702f, 0.178f, 0.774f, 0.292f}, {0.744f, 0.282f, 0.818f, 0.396f},
    };
    for (int h = 0; h < 10; ++h) {
      const auto& r = kHexRects[h];
      if (h == 4) fill_rect(r.x0, r.y0, r.x1, r.y1, 245, 200, 30);      // Bright yellow HEX_5
      else if (h == 1) fill_rect(r.x0, r.y0, r.x1, r.y1, 45, 50, 58);   // Locked grey padlock HEX_2
      else fill_rect(r.x0, r.y0, r.x1, r.y1, 32, 95, 130);              // Unlocked dark teal pegboard
    }
    fill_rect(0.225f, 0.165f, 0.265f, 0.245f, 240, 245, 250); // White waterfall inside HEX_1
    fill_rect(0.235f, 0.425f, 0.765f, 0.765f, 25, 80, 115);   // PLAY_PREVIEW
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 23);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 8180);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_LEVEL_SELECT && cands.size() == 13);
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 3 (adventure carousel) passed\n");

    // 4. LEVEL_SELECT: strict grey padlock blocks HEX_2; dark teal unlocked HEX_2..4 step on 0.1s hover despite HEX_1 waterfall
    TC_ASSERT(st.hl_col == 0);
    auto hex_locked = rex_monitor::StepMenuFocusServo(0.304f, 0.310f, true, false, false, 8200);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.hl_col == 0 && hex_locked.buttons_or_mask == 0);

    // Unlock HEX_2 with dark teal (avg_r=32 < 90, avg_b - avg_r=98 > 25) and hover-step to HEX_4 every 0.1s
    fill_rect(kHexRects[1].x0, kHexRects[1].y0, kHexRects[1].x1, kHexRects[1].y1, 32, 95, 130);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 24);
    auto h_step1 = rex_monitor::StepMenuFocusServo(0.399f, 0.365f, true, false, false, 8300);
    auto h_step2 = rex_monitor::StepMenuFocusServo(0.399f, 0.365f, true, false, false, 8400);
    auto h_step3 = rex_monitor::StepMenuFocusServo(0.399f, 0.365f, true, false, false, 8500);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(h_step1.buttons_or_mask == 0x0008 && h_step2.buttons_or_mask == 0x0008 &&
              h_step3.buttons_or_mask == 0x0008 && st.hl_col == 3);
    auto preview_click = rex_monitor::StepMenuFocusServo(0.50f, 0.60f, true, true, false, 8520);
    TC_ASSERT(preview_click.has_target && preview_click.buttons_or_mask == 0x1000);

    clear_frame(60, 100, 130);
    add_bottom_bar();
    fill_rect(0.262f, 0.905f, 0.422f, 0.978f, 40, 140, 220);  // CHANGE_COSTUME (Y)
    fill_rect(0.33f, 0.33f, 0.89f, 0.82f, 55, 135, 205);      // Flat mid-blue panel
    fill_rect(0.352f, 0.365f, 0.428f, 0.572f, 245, 240, 230); // Bright Bjorn portrait inside Row 0 col 0
    fill_rect(0.58f, 0.61f, 0.64f, 0.77f, 245, 245, 245);     // Bright `?` art inside MASTER_RANDOM
    fill_rect(0.572f, 0.334f, 0.648f, 0.352f, 245, 245, 245); // Expanded outer top strip on MASTER_2
    fill_rect(0.572f, 0.582f, 0.648f, 0.598f, 245, 245, 245); // Expanded outer bottom strip on MASTER_2
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 26);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 9000);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MASTER_SELECT && cands.size() == 9);
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 4 (level select hex) passed\n");

    // 4b. Unbeaten Level -> Intro / Jeffrey Speech Bubble -> SCREEN_GAMEPLAY (never MASTER_SELECT!)
    {
      clear_frame(95, 165, 225);
      add_bottom_bar();
      fill_rect(0.235f, 0.425f, 0.765f, 0.765f, 25, 80, 115);
      rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 2600);
      rex_monitor::SetScreenForTest(SCREEN_LEVEL_SELECT);
      auto unb_click = rex_monitor::StepMenuFocusServo(0.50f, 0.60f, true, true, false, 9010);
      TC_ASSERT(unb_click.buttons_or_mask == 0x1000);
      // Level intro + Jeffrey speech bubble (no bottom bar, green/blue board + (A) button)
      clear_frame(55, 130, 75);
      fill_rect(0.70f, 0.59f, 0.75f, 0.67f, 40, 200, 70);
      rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 2601);
      rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 9470);
      rex_monitor::GetScreenWatcherState(st, cands);
      TC_ASSERT(st.screen_id == SCREEN_GAMEPLAY && cands.empty());
      auto bubble_a = rex_monitor::StepMenuFocusServo(0.72f, 0.63f, true, true, false, 9480);
      TC_ASSERT(bubble_a.buttons_or_mask == 0x1000);

      // Even if state starts on MASTER_SELECT, missing bottom bar auto-heals to SCREEN_GAMEPLAY
      rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 2602);
      rex_monitor::SetScreenForTest(SCREEN_MASTER_SELECT);
      rex_monitor::GetScreenWatcherState(st, cands);
      TC_ASSERT(st.screen_id == SCREEN_GAMEPLAY && cands.empty());

      // Restore MASTER_SELECT frame for Section 5
      clear_frame(60, 100, 130);
      add_bottom_bar();
      fill_rect(0.262f, 0.905f, 0.422f, 0.978f, 40, 140, 220);
      fill_rect(0.33f, 0.33f, 0.89f, 0.82f, 55, 135, 205);
      fill_rect(0.352f, 0.365f, 0.428f, 0.572f, 245, 240, 230);
      fill_rect(0.58f, 0.61f, 0.64f, 0.77f, 245, 245, 245);
      fill_rect(0.572f, 0.334f, 0.648f, 0.352f, 245, 245, 245);
      fill_rect(0.572f, 0.582f, 0.648f, 0.598f, 245, 245, 245);
      rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 2603);
      rex_monitor::SetScreenForTest(SCREEN_MASTER_SELECT);
    }

    // 5. MASTER_SELECT: bright Bjorn interior & `?` inside MASTER_RANDOM must NOT steal HL from Jeffrey/Berg
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MASTER_SELECT && st.hl_row == 0 && st.hl_col == 2);

    // Step left from Berg (2) -> Jeffrey (1); Bjorn interior must NOT reset HL to 0
    auto ms_to_jeffrey = rex_monitor::StepMenuFocusServo(0.50f, 0.47f, true, false, false, 9500);
    TC_ASSERT(ms_to_jeffrey.buttons_or_mask == 0x0004);
    fill_rect(0.572f, 0.334f, 0.648f, 0.352f, 55, 135, 205);
    fill_rect(0.572f, 0.582f, 0.648f, 0.598f, 55, 135, 205);
    fill_rect(0.462f, 0.334f, 0.538f, 0.352f, 245, 245, 245); // Expanded outer top strip on MASTER_1
    fill_rect(0.462f, 0.582f, 0.538f, 0.598f, 245, 245, 245); // Expanded outer bottom strip on MASTER_1
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 260);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.hl_row == 0 && st.hl_col == 1);

    // Step onto MASTER_RANDOM and exit cleanly upward
    fill_rect(0.462f, 0.334f, 0.538f, 0.352f, 55, 135, 205);
    fill_rect(0.462f, 0.582f, 0.538f, 0.598f, 55, 135, 205);
    fill_rect(0.572f, 0.798f, 0.648f, 0.814f, 245, 245, 245); // Expanded downward on MASTER_RANDOM
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 27);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.hl_row == 1 && st.hl_col == 2);
    auto ms_exit_up = rex_monitor::StepMenuFocusServo(0.39f, 0.47f, true, false, false, 9650);
    TC_ASSERT(ms_exit_up.buttons_or_mask == 0x0001);

    // Y Costume opens Blue OK (A) modal -> OK_BTN returns to MASTER_SELECT -> MASTER_2 -> GAMEPLAY
    fill_rect(0.572f, 0.798f, 0.648f, 0.814f, 55, 135, 205);
    fill_rect(0.572f, 0.334f, 0.648f, 0.352f, 245, 245, 245);
    fill_rect(0.572f, 0.582f, 0.648f, 0.598f, 245, 245, 245);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 28);
    auto y_click = rex_monitor::StepMenuFocusServo(0.34f, 0.93f, true, true, false, 9700);
    TC_ASSERT(y_click.has_target && y_click.buttons_or_mask == 0x8000);
    clear_frame(15, 20, 30);
    fill_rect(0.30f, 0.24f, 0.70f, 0.75f, 35, 130, 220);      // Modal blue body
    fill_rect(0.415f, 0.840f, 0.585f, 0.925f, 40, 140, 220);  // Blue OK (A)
    fill_rect(0.535f, 0.850f, 0.578f, 0.915f, 40, 200, 75);   // Bright green (A) badge
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 29);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 10200);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MODAL_OK_POPUP);
    rex_monitor::StepMenuFocusServo(0.50f, 0.88f, true, true, false, 10300);
    clear_frame(60, 100, 130);
    add_bottom_bar();
    fill_rect(0.262f, 0.905f, 0.422f, 0.978f, 40, 140, 220);
    fill_rect(0.33f, 0.33f, 0.89f, 0.82f, 55, 135, 205);
    fill_rect(0.572f, 0.334f, 0.648f, 0.352f, 245, 245, 245);
    fill_rect(0.572f, 0.582f, 0.648f, 0.598f, 245, 245, 245);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 30);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 10800);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MASTER_SELECT);

    auto m2_click = rex_monitor::StepMenuFocusServo(0.61f, 0.47f, true, true, false, 10900);
    TC_ASSERT(m2_click.has_target && m2_click.buttons_or_mask == 0x1000);
    clear_frame(55, 130, 75);  // Active gameplay board (no bottom bar)
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 300);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 11400);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_GAMEPLAY && cands.empty());
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 5 (master select) passed\n");

    // 6. GAMEPLAY -> PAUSE MENU -> OPTIONS -> PAUSE MENU -> RESUME -> GAMEPLAY
    clear_frame(40, 90, 50);
    fill_rect(0.01f, 0.20f, 0.13f, 0.80f, 30, 120, 140);
    fill_rect(0.89f, 0.20f, 0.99f, 0.80f, 30, 120, 140);
    fill_rect(0.31f, 0.15f, 0.60f, 0.75f, 35, 120, 200);
    fill_rect(0.35f, 0.18f, 0.57f, 0.26f, 40, 140, 220);
    fill_rect(0.35f, 0.28f, 0.57f, 0.36f, 40, 140, 220);
    fill_rect(0.35f, 0.38f, 0.57f, 0.46f, 240, 195, 30);
    fill_rect(0.35f, 0.48f, 0.57f, 0.56f, 40, 140, 220);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 31);
    rex_monitor::NotifyMenuActionInput(0x0010, 11500);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 12000);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_PAUSE_MENU && cands.size() == 4 && st.hl_row == 2);

    auto p_opt = rex_monitor::StepMenuFocusServo(0.46f, 0.42f, true, true, false, 12100);
    TC_ASSERT(p_opt.buttons_or_mask == 0x1000);
    draw_help_options(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 32);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 12600);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_HELP_OPTIONS);

    auto p_done = rex_monitor::StepMenuFocusServo(0.46f, 0.68f, true, true, false, 12700);
    TC_ASSERT(p_done.buttons_or_mask == 0x1000);
    clear_frame(40, 90, 50);
    fill_rect(0.31f, 0.15f, 0.60f, 0.75f, 35, 120, 200);
    fill_rect(0.35f, 0.18f, 0.57f, 0.26f, 240, 195, 30);
    fill_rect(0.35f, 0.28f, 0.57f, 0.36f, 40, 140, 220);
    fill_rect(0.35f, 0.38f, 0.57f, 0.46f, 40, 140, 220);
    fill_rect(0.35f, 0.48f, 0.57f, 0.56f, 40, 140, 220);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 33);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 13200);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_PAUSE_MENU && cands.size() == 4);

    auto p_res = rex_monitor::StepMenuFocusServo(0.46f, 0.22f, true, true, false, 13300);
    TC_ASSERT(p_res.buttons_or_mask == 0x1000);
    clear_frame(55, 130, 75);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 330);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 13800);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_GAMEPLAY && cands.empty());
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 6 (pause menu) passed\n");

    // 7. MULTIPLAYER -> MP_MODE_SELECT + LOCAL LOBBY (Count=13 + RB) + MODAL YES/NO + Delayed Modal + Title Heal
    draw_mp_menu(0);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 340);
    rex_monitor::SetScreenForTest(SCREEN_MULTIPLAYER_MENU);
    auto qm_click = rex_monitor::StepMenuFocusServo(0.55f, 0.25f, true, true, false, 13850);
    TC_ASSERT(qm_click.has_target && std::strcmp(qm_click.target_name, "MP_QUICK_MATCH") == 0 &&
              qm_click.buttons_or_mask == 0x1000);
    clear_frame(95, 165, 225);
    add_bottom_bar();
    fill_rect(0.395f, 0.275f, 0.715f, 0.435f, 240, 195, 30);   // MP_MODE_PEG_PARTY
    fill_rect(0.410f, 0.455f, 0.695f, 0.610f, 40, 140, 220);   // MP_MODE_DUEL
    fill_rect(0.450f, 0.700f, 0.680f, 0.820f, 55, 155, 65);    // Open green valley
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 341);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 14350);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MP_MODE_SELECT && cands.size() == 4);
    auto qm_back = rex_monitor::StepMenuFocusServo(0.09f, 0.93f, true, true, false, 14400);
    TC_ASSERT(qm_back.buttons_or_mask == 0x2000);

    draw_mp_menu(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 342);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 14900);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MULTIPLAYER_MENU);

    auto ld_click = rex_monitor::StepMenuFocusServo(0.55f, 0.76f, true, true, false, 15000);
    TC_ASSERT(ld_click.has_target && std::strcmp(ld_click.target_name, "MP_LOCAL_DUEL") == 0 &&
              ld_click.buttons_or_mask == 0x1000);
    auto draw_local_lobby = [&]() {
      clear_frame(65, 110, 145);
      add_bottom_bar();
      fill_rect(0.262f, 0.905f, 0.422f, 0.978f, 40, 140, 220);  // CHANGE_COSTUME (Y)
      fill_rect(0.860f, 0.150f, 0.980f, 0.750f, 35, 200, 55);   // Green P2 pillar
      fill_rect(0.408f, 0.045f, 0.498f, 0.265f, 240, 195, 30);  // LOBBY_MASTER_2 highlighted
      fill_rect(0.270f, 0.365f, 0.515f, 0.415f, 40, 140, 220);  // LOBBY_MODE
      fill_rect(0.270f, 0.430f, 0.515f, 0.480f, 40, 140, 220);  // LOBBY_LEVEL
      fill_rect(0.270f, 0.495f, 0.515f, 0.545f, 40, 140, 220);  // LOBBY_SHOT_CLOCK
      fill_rect(0.405f, 0.565f, 0.585f, 0.625f, 40, 140, 220);  // LOBBY_SETTINGS_RB
    };
    draw_local_lobby();
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 343);
    rex_monitor::StepMenuFocusServo(0.15f, 0.50f, true, false, false, 15500);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MP_LOCAL_LOBBY && cands.size() == 13 &&
              std::strcmp(cands[0].name, "LOBBY_MASTER_0 (Bjorn)") == 0 &&
              std::strcmp(cands[9].name, "LOBBY_SETTINGS_RB") == 0 &&
              std::strcmp(cands[11].name, "CHANGE_COSTUME (Y)") == 0);
    // Hovering LOBBY_LEVEL (row 2, col 0) from LOBBY_MASTER_2 (row 0, col 2) steps DPAD_DOWN first
    auto ld_down = rex_monitor::StepMenuFocusServo(0.39f, 0.455f, true, false, false, 15600);
    TC_ASSERT(ld_down.has_target && std::strcmp(ld_down.target_name, "LOBBY_LEVEL") == 0 &&
              ld_down.buttons_or_mask == 0x0002);
    auto ld_rb = rex_monitor::StepMenuFocusServo(0.49f, 0.59f, true, true, false, 15700);
    TC_ASSERT(ld_rb.has_target && std::strcmp(ld_rb.target_name, "LOBBY_SETTINGS_RB") == 0 &&
              ld_rb.buttons_or_mask == 0x0200);
    rex_monitor::StepMenuFocusServo(0.15f, 0.50f, true, false, false, 16200);

    auto ld_back = rex_monitor::StepMenuFocusServo(0.09f, 0.93f, true, true, false, 16250);
    TC_ASSERT(ld_back.buttons_or_mask == 0x2000);
    // Pressing BACK in Local Duel opens "Are you sure you want to leave?" (MODAL_CANCEL_B / MODAL_OK_A).
    auto draw_modal_yesno = [&]() {
      clear_frame(15, 20, 30);
      fill_rect(0.32f, 0.28f, 0.68f, 0.72f, 35, 130, 220);
      fill_rect(0.250f, 0.840f, 0.430f, 0.925f, 40, 140, 220);
      fill_rect(0.262f, 0.850f, 0.302f, 0.908f, 200, 35, 35);   // Red (B) badge
      fill_rect(0.570f, 0.840f, 0.750f, 0.925f, 40, 140, 220);
      fill_rect(0.692f, 0.850f, 0.734f, 0.908f, 40, 200, 75);   // Green (A) badge
    };
    draw_modal_yesno();
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 345);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 16750);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MODAL_YES_NO && cands.size() == 2 &&
              std::strcmp(cands[0].name, "MODAL_CANCEL_B") == 0 &&
              std::strcmp(cands[1].name, "MODAL_OK_A") == 0);
    auto ld_cancel = rex_monitor::StepMenuFocusServo(0.340f, 0.880f, true, true, false, 16780);
    TC_ASSERT(ld_cancel.buttons_or_mask == 0x2000 &&
              std::strcmp(ld_cancel.target_name, "MODAL_CANCEL_B") == 0);
    draw_local_lobby();
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 346);
    rex_monitor::StepMenuFocusServo(0.15f, 0.50f, true, false, false, 17250);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MP_LOCAL_LOBBY);

    auto ld_back2 = rex_monitor::StepMenuFocusServo(0.09f, 0.93f, true, true, false, 17300);
    TC_ASSERT(ld_back2.buttons_or_mask == 0x2000);
    draw_modal_yesno();
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 347);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 17800);
    auto ld_ok = rex_monitor::StepMenuFocusServo(0.650f, 0.878f, true, true, false, 17830);
    TC_ASSERT(ld_ok.buttons_or_mask == 0x1000 && std::strcmp(ld_ok.target_name, "MODAL_OK_A") == 0);
    draw_mp_menu(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 348);
    rex_monitor::StepMenuFocusServo(0.15f, 0.50f, true, false, false, 18300);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MULTIPLAYER_MENU);

    // Rule B Trigger 2: 2 consecutive 0.1s ticks of structural flag change re-classifies without a click
    clear_frame(55, 130, 75);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 3480);
    rex_monitor::StepMenuFocusServo(0.15f, 0.50f, true, false, false, 18400);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MULTIPLAYER_MENU);  // 1 tick does not flip yet
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 3481);
    rex_monitor::StepMenuFocusServo(0.15f, 0.50f, true, false, false, 18500);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_GAMEPLAY && cands.empty());  // 2 consecutive ticks -> GAMEPLAY
    draw_mp_menu(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 3482);
    rex_monitor::SetScreenForTest(SCREEN_MULTIPLAYER_MENU);

    // Delayed Xbox Live MODAL OK popup (no recent click) auto-enters and auto-exits when footer brightens
    clear_frame(15, 20, 30);
    fill_rect(0.32f, 0.28f, 0.68f, 0.72f, 35, 130, 220);
    fill_rect(0.415f, 0.840f, 0.585f, 0.925f, 40, 140, 220);
    fill_rect(0.535f, 0.850f, 0.578f, 0.915f, 40, 200, 75);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 35);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MODAL_OK_POPUP && cands.size() == 1);
    draw_mp_menu(3);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 36);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_MULTIPLAYER_MENU);

    // Full-screen white fade blocks clicks; 2 consecutive Title ticks heal desynced state back to SCREEN_TITLE
    clear_frame(240, 240, 240);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 37);
    auto fade_click = rex_monitor::StepMenuFocusServo(0.50f, 0.93f, true, true, false, 18600);
    TC_ASSERT(fade_click.buttons_or_mask == 0);
    clear_frame(90, 160, 220);
    fill_rect(0.10f, 0.15f, 0.30f, 0.45f, 180, 220, 250);  // Bright sky
    fill_rect(0.18f, 0.92f, 0.38f, 0.98f, 195, 150, 210);  // Left pink/lavender cloud
    fill_rect(0.62f, 0.92f, 0.82f, 0.98f, 195, 150, 210);  // Right pink/lavender cloud
    fill_rect(0.395f, 0.815f, 0.605f, 0.915f, 40, 140, 220);  // Blue PLAY_BTN
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 38);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 18700);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 39);
    rex_monitor::StepMenuFocusServo(0.50f, 0.50f, true, false, false, 18850);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(st.screen_id == SCREEN_TITLE && cands.size() == 1);
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 7 (multiplayer + fade + title heal) passed\n");

    // 8. Pause Menu 0.1s vertical hover step, Visual Balljector guide dots, Deadzone stick, 30Hz dt
    clear_frame(40, 90, 50);
    fill_rect(0.01f, 0.20f, 0.13f, 0.80f, 30, 120, 140);
    fill_rect(0.89f, 0.20f, 0.99f, 0.80f, 30, 120, 140);
    fill_rect(0.31f, 0.15f, 0.60f, 0.75f, 35, 120, 200);
    fill_rect(0.35f, 0.18f, 0.57f, 0.26f, 240, 195, 30);
    fill_rect(0.35f, 0.28f, 0.57f, 0.36f, 40, 140, 220);
    fill_rect(0.35f, 0.38f, 0.57f, 0.46f, 40, 140, 220);
    fill_rect(0.35f, 0.48f, 0.57f, 0.56f, 40, 140, 220);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 40);
    rex_monitor::SetScreenForTest(SCREEN_PAUSE_MENU);
    auto pm_dn1 = rex_monitor::StepMenuFocusServo(0.46f, 0.42f, true, false, false, 19000);
    // Game moves gold highlight to Row 1 (PAUSE_RESTART), then Row 2 (PAUSE_OPTIONS)
    fill_rect(0.35f, 0.18f, 0.57f, 0.26f, 40, 140, 220);
    fill_rect(0.35f, 0.28f, 0.57f, 0.36f, 240, 195, 30);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 41);
    auto pm_dn2 = rex_monitor::StepMenuFocusServo(0.46f, 0.42f, true, false, false, 19100);
    fill_rect(0.35f, 0.28f, 0.57f, 0.36f, 40, 140, 220);
    fill_rect(0.35f, 0.38f, 0.57f, 0.46f, 240, 195, 30);
    rex_monitor::InjectSyntheticFrameForTest(frame.data(), W, H, 42);
    rex_monitor::GetScreenWatcherState(st, cands);
    TC_ASSERT(pm_dn1.buttons_or_mask == 0x0002 && pm_dn2.buttons_or_mask == 0x0002 &&
              st.hl_row == 2);
    auto pm_up1 = rex_monitor::StepMenuFocusServo(0.46f, 0.22f, true, false, false, 19200);
    TC_ASSERT(pm_up1.buttons_or_mask == 0x0001);

    // Visual Balljector dot centroid on cyan sky background (175, 239, 255)
    clear_frame(175, 239, 255);
    float sky_angle = 0.0f;
    uint32_t sky_dots = 0;
    TC_ASSERT(!rex_monitor::MeasureVisualTurretAngleFromPixels(frame.data(), W, H, sky_angle,
                                                               &sky_dots) &&
              sky_dots == 0);
    const float test_angle = 0.45f;
    for (float rad = 60.0f; rad <= 140.0f; rad += 12.0f) {
      const float gx = 741.0f + std::sin(test_angle) * rad;
      const float gy = 45.0f + std::cos(test_angle) * rad;
      const float nu = gx / 1280.0f, nv = gy / 720.0f;
      fill_rect(nu - 0.005f, nv - 0.008f, nu + 0.005f, nv + 0.008f, 206, 137, 226);
    }
    float meas_angle = 0.0f;
    uint32_t meas_dots = 0;
    TC_ASSERT(rex_monitor::MeasureVisualTurretAngleFromPixels(frame.data(), W, H, meas_angle,
                                                              &meas_dots) &&
              meas_dots >= 4 && std::fabs(meas_angle - test_angle) < 0.06f);

    int16_t dz_lx = 0;
    uint8_t dz_lt = 0;
    ComputeDeadzoneCompensatedStick(0.002f, dz_lx, dz_lt);
    TC_ASSERT(dz_lx == 0 && dz_lt == 0);
    ComputeDeadzoneCompensatedStick(0.020f, dz_lx, dz_lt);
    TC_ASSERT(dz_lt == 255 && dz_lx > 7849);
    ComputeDeadzoneCompensatedStick(-0.300f, dz_lx, dz_lt);
    TC_ASSERT(dz_lt == 0 && dz_lx < -25000);
    TC_ASSERT(kFpsPresets[kFps30].dt_sec <= 0.017f);
    TC_ASSERT(kFpsPresets[kFps40].hz == 40);
    TC_ASSERT(kFpsPresets[kFps40].dt_sec <= 0.017f);
    TC_ASSERT(kFpsPresets[kFps60].dt_sec <= 0.017f);
    TC_ASSERT(kFpsPresets[kFps120].dt_sec <= 0.017f);
    TC_ASSERT(kFpsPresets[kFps144].dt_sec <= 0.017f);
    TC_ASSERT(kFps120 == 3 && kFps144 == 4 && kFps40 == 6);
    TC_ASSERT(kLimiterMatchTargetHz == 0 && kLimiterUncapped == 1);
    TC_ASSERT(StepFpsPresetIndex(kFps40, 1) == kFps60);
    TC_ASSERT(StepFpsPresetIndex(kFps60, -1) == kFps40);
#if REX_PLATFORM_IOS
    TC_ASSERT(StepFpsPresetIndex(kFps60, 1) == kFps120);
    TC_ASSERT(TargetFpsIndexFromHz(144) == kFps120);
#else
    TC_ASSERT(StepFpsPresetIndex(kFps120, 1) == kFps144);
    TC_ASSERT(TargetFpsIndexFromHz(120) == kFps120);
    TC_ASSERT(TargetFpsIndexFromHz(144) == kFps144);
#endif
    TC_ASSERT(TargetFpsIndexFromHz(40) == kFps40);
    TC_ASSERT(TargetFpsIndexFromHz(60) == kFps60);
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 8 (pause hover + visual turret + deadzone) passed\n");
    rex_monitor::ClearTestState();
  }

  // ---------------------------------------------------------------------------
  // 9) iOS APFS Case-Insensitive VFS & Boot Telemetry Self-Check
  // ---------------------------------------------------------------------------
  {
    rex::ClearCaseInsensitivePathMap();
    rex::RegisterCaseInsensitivePath("Default.xex", "/var/containers/Bundle/peggle2.app/assets/Default.xex");
    rex::RegisterCaseInsensitivePath("assets/pak_stream/mus_peggle2.xsb",
                                     "/var/containers/Bundle/peggle2.app/assets/pak_stream/mus_peggle2.xsb");
    TC_ASSERT(rex::LookupCaseInsensitivePath("game:\\default.xex") ==
              "/var/containers/Bundle/peggle2.app/assets/Default.xex");
    TC_ASSERT(rex::LookupCaseInsensitivePath("d:\\Pak_Stream\\MUS_Peggle2.xsb") ==
              "/var/containers/Bundle/peggle2.app/assets/pak_stream/mus_peggle2.xsb");
    TC_ASSERT(rex::CleanGuestRelativePath("\\??\\save:\\Profile\\User0.dat") == "profile/user0.dat");
    TC_ASSERT(rex::CleanGuestRelativePath("cache:\\shader_01.bin") == "shader_01.bin");
    rex::ClearCaseInsensitivePathMap();
    if (tc_log) AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log", "[TC] Section 9 (VFS case map & path cleaning) passed\n");
  }

#undef TC_ASSERT

  if (tc_log) {
    AppendTcFileLog(
        "C:/Peggle2Recomp/tc_audit.log",
        "[TC] [COLLIDER_TEST] All engine-monitored 2D quad, highlight servo, and gameplay zone checks passed.\n");
  }
  printf("[COLLIDER_TEST] All engine-monitored 2D quad, highlight servo, and gameplay zone checks passed.\n");
  fflush(stdout);
}

static std::filesystem::path GetExeDirectory() {
#ifdef _WIN32
  char exe_buf[MAX_PATH];
  if (GetModuleFileNameA(NULL, exe_buf, MAX_PATH) > 0) {
    return std::filesystem::path(exe_buf).parent_path();
  }
#elif REX_PLATFORM_IOS
  std::string res = rex_ios_get_bundle_resource_dir();
  if (!res.empty()) {
    return std::filesystem::path(res);
  }
#endif
  return std::filesystem::current_path();
}

// ---------------------------------------------------------------------------
// Deterministic Input Record / Replay (tick = player 0 poll count)
// ---------------------------------------------------------------------------
struct InputSample {
  uint64_t tick = 0;
  uint16_t buttons = 0;
  uint8_t lt = 0;
  uint8_t rt = 0;
  int16_t lx = 0;
  int16_t ly = 0;
  int16_t rx = 0;
  int16_t ry = 0;
  uint8_t direct_active = 0;
  float direct_angle = 0.0f;
};

static bool InputStateEquals(const InputSample& a, const InputSample& b) {
  if (a.buttons != b.buttons || a.lt != b.lt || a.rt != b.rt ||
      a.lx != b.lx || a.ly != b.ly || a.rx != b.rx || a.ry != b.ry ||
      a.direct_active != b.direct_active) {
    return false;
  }
  if (a.direct_active != 0 && std::abs(a.direct_angle - b.direct_angle) > 1e-6f) {
    return false;
  }
  return true;
}

static std::mutex g_input_rec_mutex;
static std::atomic<bool> g_rec_active{false};
static std::atomic<uint64_t> g_rec_tick{0};
static std::atomic<bool> g_play_active{false};
static std::atomic<uint64_t> g_play_tick{0};
static std::atomic<uint64_t> g_play_end_tick{0};

static std::ofstream g_rec_stream;
static std::filesystem::path g_rec_path;
static bool g_rec_has_prev = false;
static InputSample g_rec_prev{};
static bool g_rec_dirty = false;

struct ReplayMenuEvent {
  uint64_t tick = 0;
  float nx = -1.0f;
  float ny = -1.0f;
  uint8_t click = 0;
  uint8_t hover = 0;
};

struct ReplaySnapEvent {
  uint64_t tick = 0;
  std::string path;
};

static std::vector<InputSample> g_play_events;
static size_t g_play_cursor = 0;
static InputSample g_play_current{};
static std::filesystem::path g_play_path;
static std::vector<ReplayMenuEvent> g_play_menu_events;
static size_t g_play_menu_cursor = 0;
static bool g_play_menu_active = false;
static float g_play_menu_x = 0.5f;
static float g_play_menu_y = 0.5f;
static bool g_play_menu_hover = false;
static bool g_play_menu_click_queued = false;
static std::vector<ReplaySnapEvent> g_play_snap_events;
static size_t g_play_snap_cursor = 0;
static uint64_t g_play_exit_tick = 0;
static uint16_t g_play_prev_action_mask = 0;
static uint16_t g_play_dpad_mask = 0;
static int g_play_dpad_ticks = 0;
static uint16_t g_play_btn_mask = 0;
static int g_play_btn_ticks = 0;

static std::atomic<bool> g_test_replay_mode{false};
static std::atomic<bool> g_test_replay_started{false};
static std::atomic<uint64_t> g_test_replay_boot_ms{0};

static bool HasCmdFlag(const char* flag) {
#ifdef _WIN32
  const char* cmd = GetCommandLineA();
  return cmd && flag && std::strstr(cmd, flag) != nullptr;
#else
  (void)flag;
  return false;
#endif
}

static bool IsTestReplayMode() {
  return HasCmdFlag("--test-replay") || IsEnvFlagEnabled("PEGGLE_AUTO_REPLAY");
}

static void TriggerTestReplayExit(const char* reason, int code = 0) {
  printf("[TEST-REPLAY] Exiting (%s)\n", reason ? reason : "done");
  fflush(stdout);
  rex_monitor::FlushReplayHitboxAudit();
  rex_monitor::FlushLog();
  ForceTerminateProcess(code);
}

static std::filesystem::path ResolveDefaultRecordPath() {
  std::string cvar_rec = REXCVAR_GET(record_input);
  if (!cvar_rec.empty()) return std::filesystem::path(cvar_rec);
  std::error_code ec;
  if (std::filesystem::is_directory("C:/Peggle2Recomp/captures", ec)) {
    return std::filesystem::path("C:/Peggle2Recomp/captures/last_recording.rec");
  }
  return GetExeDirectory() / "last_recording.rec";
}

static std::filesystem::path ResolveDefaultReplayPath() {
  std::string cvar_play = REXCVAR_GET(replay_input);
  if (!cvar_play.empty()) return std::filesystem::path(cvar_play);
  std::filesystem::path def = ResolveDefaultRecordPath();
  std::error_code ec;
  if (std::filesystem::exists(def, ec)) return def;
  const char* fallbacks[] = {
      "C:/Peggle2Recomp/captures/test_replay_audit.rec",
      "C:/Peggle2Recomp/captures/m3_drive.rec",
  };
  for (const char* fb : fallbacks) {
    std::filesystem::path p(fb);
    if (std::filesystem::exists(p, ec)) return p;
  }
  return def;
}

static void StopInputRecordingLocked() {
  if (!g_rec_active.load(std::memory_order_relaxed)) return;
  uint64_t total_ticks = g_rec_tick.load(std::memory_order_relaxed);
  if (g_rec_stream.is_open()) {
    g_rec_stream << "# end_tick " << total_ticks << "\n";
    g_rec_stream.flush();
    g_rec_stream.close();
  }
  g_rec_active.store(false, std::memory_order_relaxed);
  g_rec_has_prev = false;
  g_rec_dirty = false;
  printf("[INPUT] Recording saved (%llu ticks) -> %s\n",
         static_cast<unsigned long long>(total_ticks), g_rec_path.string().c_str());
  fflush(stdout);
}

static void StopInputReplayLocked(const char* reason) {
  if (!g_play_active.load(std::memory_order_relaxed)) return;
  uint64_t cur_tick = g_play_tick.load(std::memory_order_relaxed);
  g_play_active.store(false, std::memory_order_relaxed);
  g_mouse_direct_active.store(false, std::memory_order_relaxed);
  g_play_events.clear();
  g_play_cursor = 0;
  g_play_current = InputSample{};
  g_play_menu_events.clear();
  g_play_menu_cursor = 0;
  g_play_menu_active = false;
  g_play_menu_hover = false;
  g_play_menu_click_queued = false;
  g_play_snap_events.clear();
  g_play_snap_cursor = 0;
  g_play_exit_tick = 0;
  g_play_prev_action_mask = 0;
  g_play_dpad_mask = 0;
  g_play_dpad_ticks = 0;
  g_play_btn_mask = 0;
  g_play_btn_ticks = 0;
  printf("[INPUT] Replay %s at tick %llu (%s)\n", reason,
         static_cast<unsigned long long>(cur_tick), g_play_path.string().c_str());
  fflush(stdout);
}

static bool StartInputRecording(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(g_input_rec_mutex);
  StopInputReplayLocked("stopped for recording");
  StopInputRecordingLocked();

  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  g_rec_stream.open(path, std::ios::out | std::ios::trunc);
  if (!g_rec_stream.is_open()) {
    printf("[INPUT] ERROR: Failed to open recording file: %s\n", path.string().c_str());
    fflush(stdout);
    return false;
  }
  g_rec_path = path;
  g_rec_stream << "# peggle2 input recording v1\n"
               << "# tick buttons lt rt lx ly rx ry direct_active direct_angle\n";
  g_rec_stream.flush();
  g_rec_tick.store(0, std::memory_order_relaxed);
  g_rec_has_prev = false;
  g_rec_prev = InputSample{};
  g_rec_dirty = false;
  g_rec_active.store(true, std::memory_order_relaxed);
  printf("[INPUT] Recording started -> %s\n", g_rec_path.string().c_str());
  fflush(stdout);
  return true;
}

static void StopInputRecording() {
  std::lock_guard<std::mutex> lock(g_input_rec_mutex);
  StopInputRecordingLocked();
}

static bool StartInputReplay(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(g_input_rec_mutex);
  StopInputRecordingLocked();
  StopInputReplayLocked("restarted");

  std::ifstream in(path);
  if (!in.is_open()) {
    printf("[INPUT] ERROR: Failed to open replay file: %s\n", path.string().c_str());
    fflush(stdout);
    return false;
  }

  std::vector<InputSample> parsed;
  std::vector<ReplayMenuEvent> parsed_menu;
  std::vector<ReplaySnapEvent> parsed_snap;
  uint64_t parsed_exit_tick = 0;
  bool has_end_tick = false;
  uint64_t parsed_end_tick = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string first;
    if (!(iss >> first)) continue;
    if (first[0] == '#') {
      if (first == "#") {
        std::string tag;
        if (!(iss >> tag)) continue;
        if (tag == "end_tick") {
          uint64_t val = 0;
          if (iss >> val) {
            has_end_tick = true;
            parsed_end_tick = val;
          }
        } else if (tag == "menu") {
          ReplayMenuEvent me{};
          int clk = 0, hov = 0;
          if (iss >> me.tick >> me.nx >> me.ny >> clk >> hov) {
            me.click = clk ? 1 : 0;
            me.hover = hov ? 1 : 0;
            parsed_menu.push_back(me);
            parsed_end_tick = (std::max)(parsed_end_tick, me.tick + 1);
            has_end_tick = true;
          }
        } else if (tag == "snap") {
          ReplaySnapEvent se{};
          if (iss >> se.tick >> se.path) {
            parsed_snap.push_back(std::move(se));
            parsed_end_tick = (std::max)(parsed_end_tick, parsed_snap.back().tick + 1);
            has_end_tick = true;
          }
        } else if (tag == "exit") {
          uint64_t val = 0;
          if (iss >> val) {
            parsed_exit_tick = val;
            parsed_end_tick = (std::max)(parsed_end_tick, val + 1);
            has_end_tick = true;
          }
        }
      }
      continue;
    }

    InputSample s{};
    try {
      s.tick = std::stoull(first, nullptr, 0);
      std::string btn_tok;
      int lt_i = 0, rt_i = 0, lx_i = 0, ly_i = 0, rx_i = 0, ry_i = 0;
      if (!(iss >> btn_tok >> lt_i >> rt_i >> lx_i >> ly_i >> rx_i >> ry_i)) continue;
      s.buttons = static_cast<uint16_t>(std::stoul(btn_tok, nullptr, 0));
      s.lt = static_cast<uint8_t>(std::clamp(lt_i, 0, 255));
      s.rt = static_cast<uint8_t>(std::clamp(rt_i, 0, 255));
      s.lx = static_cast<int16_t>(std::clamp(lx_i, -32768, 32767));
      s.ly = static_cast<int16_t>(std::clamp(ly_i, -32768, 32767));
      s.rx = static_cast<int16_t>(std::clamp(rx_i, -32768, 32767));
      s.ry = static_cast<int16_t>(std::clamp(ry_i, -32768, 32767));
      int da_i = 0;
      float ang_f = 0.0f;
      if (iss >> da_i) {
        s.direct_active = (da_i != 0) ? 1 : 0;
        if (iss >> ang_f) {
          s.direct_angle = std::isfinite(ang_f) ? ang_f : 0.0f;
        }
      }
      parsed.push_back(s);
    } catch (...) {
      continue;
    }
  }

  if (parsed.empty() && parsed_menu.empty() && parsed_snap.empty() &&
      (!has_end_tick || parsed_end_tick == 0)) {
    printf("[INPUT] WARNING: Replay file has no events: %s\n", path.string().c_str());
    fflush(stdout);
    return false;
  }

  std::stable_sort(parsed.begin(), parsed.end(),
                   [](const InputSample& a, const InputSample& b) { return a.tick < b.tick; });
  std::stable_sort(parsed_menu.begin(), parsed_menu.end(),
                   [](const ReplayMenuEvent& a, const ReplayMenuEvent& b) { return a.tick < b.tick; });
  std::stable_sort(parsed_snap.begin(), parsed_snap.end(),
                   [](const ReplaySnapEvent& a, const ReplaySnapEvent& b) { return a.tick < b.tick; });
  uint64_t end_tick = parsed.empty() ? 0 : (parsed.back().tick + 1);
  if (has_end_tick) {
    end_tick = (std::max)(end_tick, parsed_end_tick);
  }

  g_play_path = path;
  g_play_events = std::move(parsed);
  g_play_menu_events = std::move(parsed_menu);
  g_play_snap_events = std::move(parsed_snap);
  g_play_exit_tick = parsed_exit_tick;
  g_play_cursor = 0;
  g_play_menu_cursor = 0;
  g_play_snap_cursor = 0;
  g_play_menu_active = false;
  g_play_menu_hover = false;
  g_play_menu_click_queued = false;
  g_play_prev_action_mask = 0;
  g_play_dpad_mask = 0;
  g_play_dpad_ticks = 0;
  g_play_btn_mask = 0;
  g_play_btn_ticks = 0;
  g_play_current = InputSample{};
  g_play_tick.store(0, std::memory_order_relaxed);
  g_play_end_tick.store(end_tick, std::memory_order_relaxed);
  g_play_active.store(true, std::memory_order_relaxed);
  printf("[INPUT] Replay started (%zu events, %zu menu, %zu snap, %llu ticks) <- %s\n",
         g_play_events.size(), g_play_menu_events.size(), g_play_snap_events.size(),
         static_cast<unsigned long long>(end_tick), g_play_path.string().c_str());
  fflush(stdout);
  return true;
}

static void StopInputReplay() {
  std::lock_guard<std::mutex> lock(g_input_rec_mutex);
  StopInputReplayLocked("stopped");
}

static bool TryStepInputReplay(InputSample& out, uint64_t now_ms) {
  bool menu_active = false;
  float mx = 0.5f, my = 0.5f;
  bool mhover = false, mclick = false;
  std::vector<std::string> due_snaps;
  bool do_exit = false;
  uint64_t cur_tick = 0;
  bool replay_finished = false;
  {
    std::lock_guard<std::mutex> lock(g_input_rec_mutex);
    if (!g_play_active.load(std::memory_order_relaxed)) return false;

    uint64_t tick = g_play_tick.load(std::memory_order_relaxed);
    uint64_t end_tick = g_play_end_tick.load(std::memory_order_relaxed);
    cur_tick = tick;
    if (tick >= end_tick) {
      StopInputReplayLocked("finished");
      out = InputSample{};
      replay_finished = true;
    } else {
      while (g_play_cursor < g_play_events.size() && g_play_events[g_play_cursor].tick <= tick) {
        g_play_current = g_play_events[g_play_cursor++];
      }
      while (g_play_menu_cursor < g_play_menu_events.size() &&
             g_play_menu_events[g_play_menu_cursor].tick <= tick) {
        const auto& me = g_play_menu_events[g_play_menu_cursor++];
        if (me.nx < 0.0f || me.ny < 0.0f) {
          g_play_menu_active = false;
        } else {
          g_play_menu_active = true;
          g_play_menu_x = me.nx;
          g_play_menu_y = me.ny;
          g_play_menu_hover = (me.hover != 0);
          if (me.click) g_play_menu_click_queued = true;
        }
      }
      while (g_play_snap_cursor < g_play_snap_events.size() &&
             g_play_snap_events[g_play_snap_cursor].tick <= tick) {
        due_snaps.push_back(g_play_snap_events[g_play_snap_cursor++].path);
      }
      if (g_play_exit_tick > 0 && tick >= g_play_exit_tick) {
        do_exit = true;
      }

      out = g_play_current;
      menu_active = g_play_menu_active;
      mx = g_play_menu_x;
      my = g_play_menu_y;
      mhover = g_play_menu_hover;
      mclick = g_play_menu_click_queued;
      g_play_menu_click_queued = false;
      g_play_tick.store(tick + 1, std::memory_order_relaxed);
    }
  }
  if (replay_finished) {
    if (g_test_replay_mode.load(std::memory_order_relaxed)) {
      TriggerTestReplayExit("replay finished", 0);
    }
    return true;  // Emit one neutral frame so no buttons remain latched on exit.
  }

  for (const auto& spath : due_snaps) {
    RexScreenWatcherStatus st{};
    size_t cnt = 0;
    const bool ok = rex_monitor::SaveScreenWatcherPng(spath.c_str(), &st, &cnt);
    printf("[SNAP] tick=%llu ok=%d screen=%s cand=%zu hl=%s(%d,%d) tgt=%s motion=%.1f%% -> %s\n",
           static_cast<unsigned long long>(cur_tick), ok ? 1 : 0,
           st.screen_name[0] ? st.screen_name : "UNKNOWN", cnt,
           st.has_highlight ? st.hl_name : "none", int(st.hl_row), int(st.hl_col),
           st.has_target ? st.target_name : "None", st.motion_pct, spath.c_str());
    fflush(stdout);
  }

  if (menu_active && !(rex_monitor::IsInGameplay() &&
                       rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY)) {
    g_hud_cursor_nx.store(mx, std::memory_order_relaxed);
    g_hud_cursor_ny.store(my, std::memory_order_relaxed);
    g_hud_cursor_valid.store(true, std::memory_order_relaxed);
    RexMonitorServoOutput srv =
        rex_monitor::StepMenuFocusServo(mx, my, mhover, mclick, false, now_ms);
    g_telemetry_hover_id.store(srv.has_target ? (srv.aligned ? "AlignedBtn" : "MenuBtn") : "None",
                               std::memory_order_relaxed);
    if (srv.buttons_or_mask & 0x000F) {
      g_play_dpad_mask = srv.buttons_or_mask & 0x000F;
      g_play_dpad_ticks = 4;
    }
    if (srv.buttons_or_mask & (0x1000 | 0x2000 | 0x8000 | 0x0200)) {
      g_play_dpad_ticks = 0;
      g_play_btn_mask = srv.buttons_or_mask & (0x1000 | 0x2000 | 0x8000 | 0x0200);
      g_play_btn_ticks = 3;
    }
  } else {
    g_hud_cursor_valid.store(false, std::memory_order_relaxed);
    const uint16_t act = out.buttons & 0x3010;
    const uint16_t edge = act & static_cast<uint16_t>(~g_play_prev_action_mask);
    g_play_prev_action_mask = act;
    if (edge != 0) {
      rex_monitor::NotifyMenuActionInput(edge, now_ms);
    }
  }

  if (g_play_dpad_ticks > 0) {
    --g_play_dpad_ticks;
    out.buttons |= g_play_dpad_mask;
  }
  if (g_play_btn_ticks > 0) {
    --g_play_btn_ticks;
    out.buttons |= g_play_btn_mask;
  }

  if (do_exit) {
    printf("[INPUT] Replay # exit cleanly at tick %llu\n",
           static_cast<unsigned long long>(cur_tick));
    fflush(stdout);
    rex_monitor::FlushReplayHitboxAudit();
    rex_monitor::FlushLog();
    ForceTerminateProcess(0);
  }
  return true;
}

static void RecordInputSample(const InputSample& s) {
  std::lock_guard<std::mutex> lock(g_input_rec_mutex);
  if (!g_rec_active.load(std::memory_order_relaxed) || !g_rec_stream.is_open()) return;

  uint64_t tick = g_rec_tick.fetch_add(1, std::memory_order_relaxed);
  if (!g_rec_has_prev || !InputStateEquals(g_rec_prev, s)) {
    char line[160];
    std::snprintf(line, sizeof(line), "%llu 0x%04X %u %u %d %d %d %d %u %.6f\n",
                  static_cast<unsigned long long>(tick),
                  static_cast<unsigned int>(s.buttons),
                  static_cast<unsigned int>(s.lt),
                  static_cast<unsigned int>(s.rt),
                  static_cast<int>(s.lx),
                  static_cast<int>(s.ly),
                  static_cast<int>(s.rx),
                  static_cast<int>(s.ry),
                  static_cast<unsigned int>(s.direct_active),
                  s.direct_active ? s.direct_angle : 0.0f);
    g_rec_stream << line;
    g_rec_prev = s;
    g_rec_has_prev = true;
    g_rec_dirty = true;
  }
  if (g_rec_dirty && (tick % 60 == 0)) {
    g_rec_stream.flush();
    g_rec_dirty = false;
  }
}

// Global guest simulation step dt (seconds), statically initialized to 1/60s at compile time
extern "C" double g_active_guest_dt = 1.0 / 60.0;
extern "C" double g_guest_step_dt = 1.0 / 60.0;

// Deterministic clean dt helper for HUD telemetry and guest frame-wait pacing
extern "C" float GetDeterministicPresetDtMs() {
  return static_cast<float>(g_active_guest_dt * 1000.0);
}

extern "C" float GetDeterministicPresetDtSec() {
  return static_cast<float>(g_active_guest_dt);
}

extern "C" uint32_t GetActiveTargetFps() {
  int fps_idx = g_target_fps_index.load(std::memory_order_relaxed);
  if (fps_idx < 0 || fps_idx >= kFpsCount) fps_idx = kDefaultTargetFpsIdx;
  return kFpsPresets[fps_idx].hz;
}

// ---------------------------------------------------------------------------
// D3D Presentation Interval Hook (0x826199B0)
// ---------------------------------------------------------------------------
extern "C" REX_FUNC(sub_826199B0) {
  REX_FUNC_PROLOGUE();
  int vsync = g_opt_vsync.load(std::memory_order_relaxed);
  // Interval 0 = IMMEDIATE (Host Pacer controls cadence without DXGI 60Hz clamp)
  // Interval 1 = ONE (Swapchain VSync)
  uint32_t interval = (vsync == kVSyncOn) ? 1 : 0;
  REX_STORE_U32(ctx.r3.u32 + 14000, interval);
}

// ---------------------------------------------------------------------------
// Presentation Swap Wrapper (0x8261FE50) calling VdSwap
// ---------------------------------------------------------------------------
DECLARE_REX_FUNC(sub_8261FE50);
extern "C" REX_FUNC(sub_8261FE50) {
  REX_FUNC_PROLOGUE();

#ifdef _WIN32
  // Edge-detected keyboard polling for F1 and menu navigation
  auto poll_key_edge = [](int vk, bool& prev_state) -> bool {
    SHORT state = GetAsyncKeyState(vk);
    bool down = (state & 0x8000) != 0;
    bool pressed = ((state & 0x0001) != 0) || (down && !prev_state);
    prev_state = down;
    return pressed;
  };

  static bool prev_f1 = false;
  static bool prev_f2 = false;
  static bool prev_f3 = false;
  static bool prev_f6 = false;
  static bool prev_f7 = false;
  static bool prev_up = false;
  static bool prev_down = false;
  static bool prev_left = false;
  static bool prev_right = false;
  static bool prev_enter = false;
  static bool prev_esc = false;

  if (poll_key_edge(VK_F1, prev_f1)) {
    bool cur = g_menu_open.load(std::memory_order_relaxed);
    g_menu_open.store(!cur, std::memory_order_relaxed);
  }

  HWND fg_swap = GetForegroundWindow();
  DWORD fg_pid = 0;
  if (fg_swap) GetWindowThreadProcessId(fg_swap, &fg_pid);
  bool swap_game_active = (fg_pid == GetCurrentProcessId());

  if (poll_key_edge(VK_F2, prev_f2) && swap_game_active) {
    int cur = g_opt_mouse_aim.load(std::memory_order_relaxed);
    g_opt_mouse_aim.store((cur + 1) % kAimModeCount, std::memory_order_relaxed);
    SaveUserSettings();
  }
  if (poll_key_edge(VK_F3, prev_f3) && swap_game_active) {
    int cur = g_opt_menu_mode.load(std::memory_order_relaxed);
    g_opt_menu_mode.store((cur + 1) % kMenuModeCount, std::memory_order_relaxed);
    SaveUserSettings();
  }

  if (poll_key_edge(VK_F6, prev_f6) && swap_game_active) {
    if (g_rec_active.load(std::memory_order_relaxed)) {
      StopInputRecording();
    } else {
      StartInputRecording(ResolveDefaultRecordPath());
    }
  }
  if (poll_key_edge(VK_F7, prev_f7) && swap_game_active) {
    if (g_play_active.load(std::memory_order_relaxed)) {
      StopInputReplay();
    } else {
      StartInputReplay(ResolveDefaultReplayPath());
    }
  }

  if (g_menu_open.load(std::memory_order_relaxed)) {
    if (poll_key_edge(VK_ESCAPE, prev_esc)) {
      g_menu_open.store(false, std::memory_order_relaxed);
    }
    if (poll_key_edge(VK_UP, prev_up)) {
      g_selected_menu_item = (g_selected_menu_item + kMenuItemCount - 1) % kMenuItemCount;
    }
    if (poll_key_edge(VK_DOWN, prev_down)) {
      g_selected_menu_item = (g_selected_menu_item + 1) % kMenuItemCount;
    }

    auto cycle_val = [](std::atomic<int>& opt, int delta, int max_val) {
      int cur = opt.load(std::memory_order_relaxed);
      int next = (cur + delta + max_val) % max_val;
      opt.store(next, std::memory_order_relaxed);
    };

    bool left = poll_key_edge(VK_LEFT, prev_left);
    bool right = poll_key_edge(VK_RIGHT, prev_right) || poll_key_edge(VK_RETURN, prev_enter);

    if (left || right) {
      int dir = right ? 1 : -1;
      if (g_selected_menu_item == 0) {
        int cur = g_target_fps_index.load(std::memory_order_relaxed);
        g_target_fps_index.store(StepFpsPresetIndex(cur, dir), std::memory_order_relaxed);
        SaveUserSettings();
      } else if (g_selected_menu_item >= 0 && g_selected_menu_item < kMenuItemCount) {
        const auto& e = kSettingEntries[g_selected_menu_item];
        int cur = e.value->load(std::memory_order_relaxed);
        int next = (cur + dir + e.count) % e.count;
        e.value->store(next, std::memory_order_relaxed);
#if !REX_PLATFORM_IOS
        if (g_selected_menu_item == 14) {
          ApplyEdramModeSetting(next, true);
        }
#endif
        SaveUserSettings();
      }
    }
  }

  static bool s_win_timer_init = false;
  if (!s_win_timer_init) {
    timeBeginPeriod(1);
    s_win_timer_init = true;
  }
#endif

  // Synchronize presentation interval and SDK vsync/refresh settings
  int vsync = g_opt_vsync.load(std::memory_order_relaxed);
  int fps_idx = g_target_fps_index.load(std::memory_order_relaxed);
  if (fps_idx < 0 || fps_idx >= kFpsCount) fps_idx = kDefaultTargetFpsIdx;
  int limiter_idx = g_opt_limiter.load(std::memory_order_relaxed);
  bool is_uncapped = (fps_idx == kFpsUncapped) || (limiter_idx == kLimiterUncapped);
  rex::ui::Presenter::SetUncappedPresent(is_uncapped);

  uint32_t target_hz = kFpsPresets[fps_idx].hz;
  uint32_t interval = (vsync == kVSyncOn) ? 1 : 0;
  REX_STORE_U32(ctx.r3.u32 + 14000, interval);

  // Guest simulation logic dt is strictly locked to 1/60s (0.016666668f)
  g_active_guest_dt = 1.0 / 60.0;
  g_guest_step_dt = 1.0 / 60.0;

  static int last_synced_vsync = -1;
  static int last_synced_fps = -1;
  static int last_synced_limiter = -1;
  static int last_synced_res = -1;
  static int last_synced_tex = -1;
  static int last_synced_audio = -1;

  if (last_synced_vsync != vsync || last_synced_fps != fps_idx || last_synced_limiter != limiter_idx) {
    rex::cvar::SetFlagByName("vsync", (vsync == kVSyncOn) ? "true" : "false");
    rex::cvar::SetFlagByName("VSyncInterval", (vsync == kVSyncOn) ? "1" : "0");
    rex::cvar::SetFlagByName("TargetFPS", std::to_string(target_hz));
    rex::cvar::SetFlagByName("target_fps", std::to_string(target_hz));
    rex::cvar::SetFlagByName("display_refresh_rate", std::to_string(target_hz));
    // Keep Xbox 360 guest display/VBlank cadence locked to 60 Hz across all host refresh presets.
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");
#if REX_PLATFORM_IOS
    rex_ios_set_target_fps(target_hz);
#endif
    last_synced_vsync = vsync;
    last_synced_fps = fps_idx;
    last_synced_limiter = limiter_idx;
  }

  int res_idx = g_opt_res.load(std::memory_order_relaxed);
  int cur_tex_idx = g_opt_tex.load(std::memory_order_relaxed);
  if (last_synced_res != res_idx) {
    float scale = 1.0f;
    switch (res_idx) {
      case kRes180p:  scale = 0.25f; break;
      case kRes360p:  scale = 0.50f; break;
      case kRes720p:  scale = 1.00f; break;
      case kRes1080p: scale = 1.50f; break;
      case kRes1440p: scale = 2.00f; break;
      case kRes4K:    scale = 3.00f; break;
    }
    rex::ui::Presenter::SetResolutionScale(scale);
    rex::ui::Presenter::RefreshGuestOutputPaintConfig();
    last_synced_res = res_idx;
  }

  int tex_idx = g_opt_tex.load(std::memory_order_relaxed);
#if REX_PLATFORM_IOS
  rex_ios_set_low_power_auto_scale_enabled(
      g_opt_low_power_scale.load(std::memory_order_relaxed) != 0);
  const bool lp_075 = rex_ios_is_low_power_075x_active();
  static bool s_last_lp_075 = false;
  if (lp_075 != s_last_lp_075) {
    last_synced_tex = -1;
    s_last_lp_075 = lp_075;
  }
  const int effective_tex_idx = lp_075 ? 0 : tex_idx;
#else
  const int effective_tex_idx = tex_idx;
#endif
  if (last_synced_tex != effective_tex_idx) {
    rex::cvar::SetFlagByName("texture_upscale_mode", std::to_string(effective_tex_idx));
    if (effective_tex_idx == 0) {
      rex::cvar::SetFlagByName("present_effect", "bilinear");
      rex::cvar::SetFlagByName("present_cas_additional_sharpness", "0.0");
    } else if (effective_tex_idx == 1) {
      rex::cvar::SetFlagByName("present_effect", "cas");
      rex::cvar::SetFlagByName("present_cas_additional_sharpness", "0.85");
    } else {
      rex::cvar::SetFlagByName("present_effect", "point");
      rex::cvar::SetFlagByName("present_cas_additional_sharpness", "1.0");
    }
    rex::ui::Presenter::RefreshGuestOutputPaintConfig();
    last_synced_tex = effective_tex_idx;
  }

  int audio_idx = g_opt_audio.load(std::memory_order_relaxed);
  if (last_synced_audio != audio_idx) {
    rex::cvar::SetFlagByName("audio_safe_sync", (audio_idx == kAudioSafe) ? "true" : "false");
    last_synced_audio = audio_idx;
  }

  static bool s_has_prev_vdswap = false;
  static auto s_prev_vdswap = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (s_has_prev_vdswap) {
    float elapsed_ms = std::chrono::duration<float, std::milli>(now - s_prev_vdswap).count();
    g_vdswap_elapsed_ms.store(elapsed_ms, std::memory_order_relaxed);
    g_guest_ft_window.Push(elapsed_ms);
  }
  s_prev_vdswap = now;
  s_has_prev_vdswap = true;

  g_vdswap_calls.fetch_add(1, std::memory_order_relaxed);
  rex::SetGuestLastPC(0x8261FE50u);
  rex::SetGuestLastLR(static_cast<uint32_t>(ctx.lr));
  rex::SetGuestThreadState(1);
  rex::RecordGuestTick();
  __imp__sub_8261FE50(ctx, base);

  // Fail-safe monotonic 60.0 Hz guest swap ceiling in case an underlying swap returns early.
  {
    constexpr auto kMinSwapInterval = std::chrono::nanoseconds(16666667);
    static bool s_swap_guard_init = false;
    static std::chrono::steady_clock::time_point s_next_swap_guard{};
    auto post_now = std::chrono::steady_clock::now();
    if (!s_swap_guard_init || s_next_swap_guard > post_now + kMinSwapInterval) {
      s_next_swap_guard = post_now + kMinSwapInterval;
      s_swap_guard_init = true;
    } else if (post_now > s_next_swap_guard + std::chrono::milliseconds(4)) {
      s_next_swap_guard = post_now + kMinSwapInterval;
    } else {
      while (post_now < s_next_swap_guard) {
        auto rem = std::chrono::duration_cast<std::chrono::nanoseconds>(s_next_swap_guard - post_now).count();
        if (rem > 2200000LL) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
          std::this_thread::yield();
        }
        post_now = std::chrono::steady_clock::now();
      }
      s_next_swap_guard += kMinSwapInterval;
    }
  }
}

// ---------------------------------------------------------------------------
// Simulation Update Count Hook (0x8242F2C8)
// ---------------------------------------------------------------------------
DECLARE_REX_FUNC(sub_8242F2C8);
extern "C" REX_FUNC(sub_8242F2C8) {
  REX_FUNC_PROLOGUE();
  g_current_frame.fetch_add(1, std::memory_order_relaxed);
  // Peggle 2 calls sub_8242F2C8 twice per 60 Hz frame (r4=0 pre-step at 0x8242F9D4, then main update at 0x8242FA0C).
  if (static_cast<uint32_t>(ctx.lr) != 0x8242F9D4u) {
    g_f2c8_updates.fetch_add(1, std::memory_order_relaxed);
  }
  rex::SetGuestLastPC(0x8242F2C8u);
  rex::SetGuestLastLR(static_cast<uint32_t>(ctx.lr));
  rex::SetGuestThreadState(1);
  rex::RecordGuestTick();
  __imp__sub_8242F2C8(ctx, base);
}

// ---------------------------------------------------------------------------
// RakNet Startup & Socket Error Telemetry Hooks
// ---------------------------------------------------------------------------
#define PEGGLE_WRAP_NET_FAIL(fn_name, fail_cond)                             \
  DECLARE_REX_FUNC(fn_name);                                                 \
  extern "C" REX_FUNC(fn_name) {                                             \
    REX_FUNC_PROLOGUE();                                                     \
    const uint32_t caller_lr = static_cast<uint32_t>(ctx.lr);                \
    __imp__##fn_name(ctx, base);                                             \
    const int32_t rc = ctx.r3.s32;                                           \
    if (fail_cond) {                                                         \
      char msg[96];                                                          \
      std::snprintf(msg, sizeof(msg), "NET: %s ret=%d lr=0x%08x", #fn_name,  \
                    rc, caller_lr);                                          \
      rex::RecordKernelFail(msg);                                            \
    }                                                                        \
  }

PEGGLE_WRAP_NET_FAIL(sub_822ECF80, rc != 0)
PEGGLE_WRAP_NET_FAIL(sub_822FC338, rc != 0)
PEGGLE_WRAP_NET_FAIL(sub_822FC130, rc != 0)
PEGGLE_WRAP_NET_FAIL(sub_822FC718, rc < 0)
#undef PEGGLE_WRAP_NET_FAIL

// ---------------------------------------------------------------------------
// Win32 Icon & "Locate Your ISO / Game Data" Helpers
// ---------------------------------------------------------------------------
#ifdef _WIN32
static HWND g_app_hwnd = nullptr;

static HWND EnsureAppWindowIcon() {
  if (!g_app_hwnd || !IsWindow(g_app_hwnd)) {
    struct FindCtx { DWORD pid; HWND found; } ctx{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
      auto* c = reinterpret_cast<FindCtx*>(lp);
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid == c->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        RECT r{};
        if (GetWindowRect(hwnd, &r) && (r.right - r.left) > 200 && (r.bottom - r.top) > 200) {
          c->found = hwnd;
          return FALSE;
        }
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    g_app_hwnd = ctx.found;
    if (g_app_hwnd) {
      HINSTANCE hinst = GetModuleHandleW(nullptr);
      HANDLE big_ico = LoadImageW(hinst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
      HANDLE sm_ico  = LoadImageW(hinst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
      if (big_ico) SendMessageW(g_app_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_ico));
      if (sm_ico)  SendMessageW(g_app_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm_ico));
    }
  }
  return g_app_hwnd;
}
#else
static void EnsureAppWindowIcon() {}
#endif

struct LocateGameState {
  std::atomic<bool> active{false};
  bool mid_session = false;
  rex::PathConfig defaults;
  std::function<void(rex::PathConfig)> resume;
  char path_buf[1024]{};
  bool needs_validate = true;
  bool valid = false;
  std::filesystem::path resolved_path;
  std::string status;
};

static LocateGameState g_locate_state;
static std::string g_cli_positional_game;
static std::filesystem::path g_active_game_source;

static bool EqualsCaseInsensitive(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

static bool HasExtractedGameFiles(const std::filesystem::path& dir) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return false;
  if (std::filesystem::exists(dir / "Default.xex", ec) ||
      std::filesystem::exists(dir / "default.xex", ec) ||
      std::filesystem::exists(dir / "main.rsb", ec)) {
    return true;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string fname = entry.path().filename().string();
    if (EqualsCaseInsensitive(fname, "default.xex") ||
        EqualsCaseInsensitive(fname, "main.rsb")) {
      return true;
    }
  }
  return false;
}

static bool IsXboxIsoFile(const std::filesystem::path& p) {
  std::error_code ec;
  if (p.empty() || !std::filesystem::is_regular_file(p, ec) ||
      !EqualsCaseInsensitive(p.extension().string(), ".iso")) {
    return false;
  }
  std::ifstream f(p, std::ios::binary);
  if (!f.is_open()) return false;
  static constexpr uint64_t kGdfxOffsets[] = {
      0x00000000ULL, 0x0000FB20ULL, 0x00020600ULL, 0x02080000ULL, 0x0FD90000ULL,
  };
  static constexpr char kGdfxMagic[] = "MICROSOFT*XBOX*MEDIA";
  char buf[20]{};
  for (uint64_t off : kGdfxOffsets) {
    f.clear();
    f.seekg(static_cast<std::streamoff>(off + 32ULL * 2048ULL), std::ios::beg);
    if (f.read(buf, sizeof(buf)) && std::memcmp(buf, kGdfxMagic, sizeof(buf)) == 0) {
      return true;
    }
  }
  return false;
}

static bool IsStfsContainerFile(const std::filesystem::path& p) {
  std::error_code ec;
  if (p.empty() || !std::filesystem::is_regular_file(p, ec)) return false;
  std::ifstream f(p, std::ios::binary);
  char magic[4]{};
  if (!f.read(magic, 4)) return false;
  return std::memcmp(magic, "LIVE", 4) == 0 ||
         std::memcmp(magic, "PIRS", 4) == 0 ||
         std::memcmp(magic, "CON ", 4) == 0;
}

static std::filesystem::path NormalizeGameSourceCandidate(std::filesystem::path p) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(p, ec) &&
      EqualsCaseInsensitive(p.filename().string(), "default.xex")) {
    return p.parent_path();
  }
  return p;
}

static bool IsValidGameDataPath(const std::filesystem::path& p) {
  if (p.empty()) return false;
  if (HasExtractedGameFiles(p)) return true;
  if (IsXboxIsoFile(p)) return true;
  return IsStfsContainerFile(p);
}

static std::filesystem::path FindValidGameInFolder(const std::filesystem::path& dir) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return {};
  if (HasExtractedGameFiles(dir)) return dir;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& fp : files) {
    if (IsXboxIsoFile(fp) || IsStfsContainerFile(fp)) return fp;
  }
  return {};
}

static void ValidateLocatePath(LocateGameState& st) {
  std::string raw(st.path_buf);
  while (!raw.empty() && (raw.front() == ' ' || raw.front() == '"' || raw.front() == '\t')) raw.erase(raw.begin());
  while (!raw.empty() && (raw.back() == ' ' || raw.back() == '"' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();

  st.valid = false;
  st.resolved_path.clear();
  if (raw.empty()) {
    st.status = "Select your Peggle 2 extracted folder, .iso, XBLA container, or Default.xex.";
    return;
  }

  std::error_code ec;
  std::filesystem::path p(raw);
  if (std::filesystem::is_regular_file(p, ec)) {
    std::string fname = p.filename().string();
    std::string ext = p.extension().string();
    if (EqualsCaseInsensitive(fname, "default.xex")) {
      p = p.parent_path();
    } else if (EqualsCaseInsensitive(ext, ".iso")) {
      if (IsXboxIsoFile(p)) {
        st.valid = true;
        st.resolved_path = std::filesystem::absolute(p, ec);
        st.status = "Valid Xbox 360 ISO image ready.";
      } else {
        st.status = "File does not contain a valid Xbox 360 GDFX header.";
      }
      return;
    } else if (IsStfsContainerFile(p)) {
      st.valid = true;
      st.resolved_path = std::filesystem::absolute(p, ec);
      st.status = "Valid Xbox 360 XBLA STFS container ready.";
      return;
    } else {
      st.status = "Unsupported file. Pick a .iso, XBLA container, Default.xex, or game folder.";
      return;
    }
  }

  if (HasExtractedGameFiles(p)) {
    st.valid = true;
    st.resolved_path = std::filesystem::absolute(p, ec);
    st.status = "Valid extracted Peggle 2 game folder.";
  } else if (std::filesystem::is_directory(p, ec)) {
    st.status = "Folder does not contain Default.xex or main.rsb.";
  } else {
    st.status = "Path does not exist.";
  }
}

#ifdef _WIN32
static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
  return out;
}

static bool RunWin32FileOpenDialog(HWND owner, bool pick_folder, const wchar_t* title,
                                   std::string& out_path) {
  HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dlg = nullptr;
  bool picked = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                    (pick_folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!pick_folder) {
      static constexpr COMDLG_FILTERSPEC kSpecs[] = {
          {L"Peggle 2 Game Files (*.iso; Default.xex; XBLA)", L"*.iso;default.xex;Default.xex;*.*"},
          {L"All Files (*.*)", L"*.*"},
      };
      dlg->SetFileTypes(2, kSpecs);
      dlg->SetFileTypeIndex(1);
    }
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR wpath = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
          out_path = WideToUtf8(wpath);
          picked = !out_path.empty();
          CoTaskMemFree(wpath);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(hr_init)) CoUninitialize();
  return picked;
}

static bool BrowseForGameFile(HWND owner, std::string& out_path) {
  return RunWin32FileOpenDialog(owner, false,
                                L"Select Peggle 2 .ISO, XBLA Container, or Default.xex",
                                out_path);
}

static bool BrowseForGameFolder(HWND owner, std::string& out_path) {
  return RunWin32FileOpenDialog(owner, true, L"Select Extracted Peggle 2 Game Folder", out_path);
}

static void RestartWindowsAppWithToml() {
  wchar_t exe_path[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) return;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(exe_path, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::exit(0);
  }
}
#endif

static void SaveGameDataRootToToml(const std::filesystem::path& config_path,
                                   const std::filesystem::path& game_path) {
  if (config_path.empty()) return;
  std::string normalized = game_path.generic_string();
#if REX_PLATFORM_IOS
  const char* primary_key = "game_data_root";
#else
  const char* primary_key = "game_path";
  rex::cvar::SetFlagByName("game_path", normalized);
#endif
  UpdateTomlKeyLine(config_path, {"game_path", "game_data_root"},
                    std::string(primary_key) + " = \"" + normalized + "\"", true);
}

// ---------------------------------------------------------------------------
// In-Game ReXGlue Settings Overlay & Telemetry HUD (ImGui)
// ---------------------------------------------------------------------------
static const char* AimModeName(int mode) {
  static const char* const kNames[kAimModeCount] = {
      "Guest Turret Lock (1:1)",
      "Closed-Loop Stick Servo",
      "1:1 Mouse Delta",
      "Mobile Touch Slingshot",
  };
  return kNames[std::clamp(mode, 0, kAimModeCount - 1)];
}

static const char* MenuModeName(int mode) {
  static const char* const kNames[kMenuModeCount] = {
      "Click-to-Target",
      "Hover + Click (Direct)",
      "Hover Auto-Step (90ms)",
      "Wheel/Flick + Click Anywhere",
  };
  return kNames[std::clamp(mode, 0, kMenuModeCount - 1)];
}

static std::filesystem::path ResolveGameDataFolder(const std::filesystem::path& requested);

class ReXGlueSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ReXGlueSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer)
      : rex::ui::ImGuiDialog(imgui_drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    EnsureAppWindowIcon();

    if (g_locate_state.active.load(std::memory_order_acquire)) {
      DrawLocateGameDialog(io);
      return;
    }

    static bool s_has_prev_host = false;
    static auto s_prev_host = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (s_has_prev_host) {
      float dt_ms = std::chrono::duration<float, std::milli>(now - s_prev_host).count();
      g_host_ft_window.Push(dt_ms);
    }
    s_prev_host = now;
    s_has_prev_host = true;

    if (g_opt_ui_colliders.load(std::memory_order_relaxed) == kCollidersOn) {
      DrawUiColliderOverlay(io);
    }
    if (g_opt_hud.load(std::memory_order_relaxed) != kHudHidden) {
      const rex::RexBootTelemetrySnapshot boot_tel = rex::GetBootTelemetrySnapshot();
      const char* crash_sym = UpdateCrashDiagnostics(boot_tel);
      DrawTelemetryHud(io, boot_tel, crash_sym);
    }
    DrawMobileHudToggle(io);
    if (g_menu_open.load(std::memory_order_relaxed)) {
      DrawSettingsMenu(io);
    }
  }

 private:
  void DrawUiColliderOverlay(ImGuiIO& io) {
    RexGpuUiMonitorSnapshotMirror gpu{};
    std::vector<RexGuestWidgetBox> guest_widgets;
    RexMonitorServoOutput servo{};
    rex_monitor::GetSnapshotForOverlay(gpu, guest_widgets, servo);

    RexScreenWatcherStatus sw{};
    std::vector<RexCandidateButton> candidates;
    rex_monitor::GetScreenWatcherState(sw, candidates);

    float nx = g_hud_cursor_nx.load(std::memory_order_relaxed);
    float ny = g_hud_cursor_ny.load(std::memory_order_relaxed);
    bool cursor_valid = g_hud_cursor_valid.load(std::memory_order_relaxed);
    bool in_gp = rex_monitor::IsInGameplay() &&
                 (rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY);

    ViewportRect vp = Compute16x9Viewport(io.DisplaySize.x, io.DisplaySize.y);
    const bool menu_is_open = g_menu_open.load(std::memory_order_relaxed);
    ImDrawList* dl = menu_is_open ? ImGui::GetBackgroundDrawList()
                                  : ImGui::GetForegroundDrawList();
    if (!dl) return;

    int aim_mode = g_opt_mouse_aim.load(std::memory_order_relaxed);
    int menu_mode = g_opt_menu_mode.load(std::memory_order_relaxed);
    int input_style = g_opt_input_style.load(std::memory_order_relaxed);
    char hdr[320];
    snprintf(hdr, sizeof(hdr),
             "[ScreenWatcher 0.1s: %s | Cand:%zu | HL:%s (%.0f%%) | Motion:%.0f%%%s | Target:%s]",
             in_gp ? "GAMEPLAY" : (sw.screen_name[0] ? sw.screen_name : "UNKNOWN"),
             candidates.size(), sw.has_highlight ? sw.hl_name : "none",
             sw.has_highlight ? (sw.hl_gold_frac * 100.0f) : 0.0f,
             sw.motion_pct, sw.in_transition ? " TRANSITION" : "",
             sw.has_target ? sw.target_name : "None");
    ImVec2 hdr_sz = ImGui::CalcTextSize(hdr);
    float hx = vp.x + 0.5f * (vp.w - hdr_sz.x) - 10.0f;
    float hy = vp.y + 8.0f;
    dl->AddRectFilled(ImVec2(hx, hy), ImVec2(hx + hdr_sz.x + 20.0f, hy + hdr_sz.y + 8.0f),
                      IM_COL32(10, 14, 22, 200), 4.0f);
    dl->AddRect(ImVec2(hx, hy), ImVec2(hx + hdr_sz.x + 20.0f, hy + hdr_sz.y + 8.0f),
                IM_COL32(0, 230, 220, 210), 4.0f);
    dl->AddText(ImVec2(hx + 10.0f, hy + 4.0f), IM_COL32(120, 255, 240, 255), hdr);

    // 1. Cyan wireframes: All active screen hitboxes on the current 0.1s classified screen.
    for (size_t i = 0; i < candidates.size(); ++i) {
      const auto& c = candidates[i];
      ImVec2 p0(vp.x + c.x0 * vp.w, vp.y + c.y0 * vp.h);
      ImVec2 p1(vp.x + c.x1 * vp.w, vp.y + c.y1 * vp.h);
      dl->AddRectFilled(p0, p1, IM_COL32(0, 210, 235, 22), 2.0f);
      dl->AddRect(p0, p1, IM_COL32(0, 235, 245, 220), 2.0f, 0, 1.8f);
      char clbl[56];
      snprintf(clbl, sizeof(clbl), "%s (Y:%.0f%% B:%.0f%%)", c.name[0] ? c.name : "BTN",
               c.gold_frac * 100.0f, c.blue_frac * 100.0f);
      dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 2.0f), IM_COL32(0, 245, 255, 240), clbl);
    }

    // 2. Layer 1 Guest Widget bounds (when active).
    for (const auto& w : guest_widgets) {
      if (!w.has_bounds) continue;
      ImVec2 p0(vp.x + w.x0 * vp.w, vp.y + w.y0 * vp.h);
      ImVec2 p1(vp.x + w.x1 * vp.w, vp.y + w.y1 * vp.h);
      dl->AddRect(p0, p1, IM_COL32(100, 230, 170, 210), 2.0f, 0, 1.5f);
      char wlbl[64];
      snprintf(wlbl, sizeof(wlbl), "%s @%08X", w.label, w.guest_ptr);
      dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 14.0f), IM_COL32(110, 240, 180, 230), wlbl);
    }

    // 3. Gameplay-only HUD & Playfield zones when turret is ticking.
    if (in_gp) {
      int flash_z = g_hud_flash_zone.load(std::memory_order_relaxed);
      int flash_t = g_hud_flash_ticks.load(std::memory_order_relaxed);
      for (int i = 0; i < 3; ++i) {
        const auto& z = kGameplayZones[i];
        ImVec2 p0(vp.x + z.x0 * vp.w, vp.y + z.y0 * vp.h);
        ImVec2 p1(vp.x + z.x1 * vp.w, vp.y + z.y1 * vp.h);
        bool flashing = (flash_t > 0 && flash_z == i);
        ImU32 line_col = flashing ? IM_COL32(255, 70, 70, 255)
                                  : (z.is_aim_zone ? IM_COL32(80, 240, 130, 180)
                                                   : IM_COL32(0, 240, 230, 220));
        dl->AddRect(p0, p1, line_col, 3.0f, 0, flashing ? 3.0f : 1.75f);
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 5.0f), line_col, z.label);
      }
    }

    // 4. Bright Green wireframe: HoveredButton under mouse cursor.
    if (sw.has_target) {
      ImVec2 g0(vp.x + sw.target_x0 * vp.w, vp.y + sw.target_y0 * vp.h);
      ImVec2 g1(vp.x + sw.target_x1 * vp.w, vp.y + sw.target_y1 * vp.h);
      dl->AddRectFilled(g0, g1, IM_COL32(40, 255, 80, 38), 3.0f);
      dl->AddRect(g0, g1, IM_COL32(40, 255, 80, 255), 3.0f, 0, 2.8f);
      char tlbl[48];
      snprintf(tlbl, sizeof(tlbl), "TARGET: %s", sw.target_name[0] ? sw.target_name : "BTN");
      dl->AddText(ImVec2(g0.x + 4.0f, g0.y + 16.0f), IM_COL32(80, 255, 110, 255), tlbl);
    }

    // 5. Bright Yellow wireframe: ActiveHighlightBox + Yellow -> Green steering line.
    if (sw.has_highlight) {
      ImVec2 h0(vp.x + sw.hl_x0 * vp.w, vp.y + sw.hl_y0 * vp.h);
      ImVec2 h1(vp.x + sw.hl_x1 * vp.w, vp.y + sw.hl_y1 * vp.h);
      dl->AddRectFilled(h0, h1, IM_COL32(255, 240, 20, 42), 3.0f);
      dl->AddRect(h0, h1, IM_COL32(255, 240, 20, 255), 3.0f, 0, 3.2f);
      char hlbl[48];
      snprintf(hlbl, sizeof(hlbl), "SELECTED: %s", sw.hl_name[0] ? sw.hl_name : "BTN");
      dl->AddText(ImVec2(h0.x + 4.0f, h1.y - 16.0f), IM_COL32(255, 245, 50, 255), hlbl);

      if (sw.has_target) {
        ImVec2 h_mid(vp.x + sw.hl_u * vp.w, vp.y + sw.hl_v * vp.h);
        ImVec2 t_mid(vp.x + sw.target_u * vp.w, vp.y + sw.target_v * vp.h);
        dl->AddLine(h_mid, t_mid, IM_COL32(255, 240, 40, 255), 2.8f);
        dl->AddCircleFilled(h_mid, 4.5f, IM_COL32(255, 240, 40, 255), 12);
        dl->AddCircleFilled(t_mid, 5.0f, IM_COL32(40, 255, 80, 255), 12);
      }
    }

    if (cursor_valid) {
      float cx = vp.x + nx * vp.w;
      float cy = vp.y + ny * vp.h;
      if ((aim_mode == kAimTouchSlingshot || input_style == kInputMobileTouch) &&
          g_touch_dragging.load(std::memory_order_relaxed)) {
        dl->AddCircleFilled(ImVec2(cx, cy), 18.0f, IM_COL32(0, 255, 255, 65), 24);
        dl->AddCircle(ImVec2(cx, cy), 22.0f, IM_COL32(0, 255, 255, 255), 24, 2.8f);
        dl->AddCircleFilled(ImVec2(cx, cy), 5.0f, IM_COL32(200, 255, 255, 255), 16);
      } else {
        ImU32 ch_col = IM_COL32(255, 255, 90, 235);
        dl->AddCircle(ImVec2(cx, cy), 8.0f, ch_col, 16, 1.8f);
        dl->AddLine(ImVec2(cx - 13.0f, cy), ImVec2(cx + 13.0f, cy), ch_col, 1.5f);
        dl->AddLine(ImVec2(cx, cy - 13.0f), ImVec2(cx, cy + 13.0f), ch_col, 1.5f);
      }
    }
  }
  void DrawLocateGameDialog(ImGuiIO& io) {
    if (g_locate_state.needs_validate) {
      ValidateLocatePath(g_locate_state);
      g_locate_state.needs_validate = false;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);

    bool do_launch = false;
#if REX_PLATFORM_IOS
    if (rex_ios_is_game_picker_active()) {
      return;
    }
    if (ImGui::Begin("Peggle 2 - Game Files Required", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
      ImGui::TextWrapped(
          "Select an extracted Peggle 2 game folder, a .iso / XBLA file, or copy files into "
          "Files -> On My iPhone -> Peggle 2 -> game.");
      ImGui::Spacing();
      if (ImGui::Button("Select Game Source...", ImVec2(220, 44))) {
        rex_ios_present_game_picker(false, [](const std::string& chosen_path) {
          if (chosen_path.empty()) return;
          rex::PathConfig next_paths = g_locate_state.defaults;
          next_paths.game_data_root = std::filesystem::path(chosen_path);
          SaveGameDataRootToToml(next_paths.config_path, next_paths.game_data_root);
          rex_ios_init_vfs_case_map_for_path(chosen_path);
          auto resume_cb = std::move(g_locate_state.resume);
          g_locate_state.active.store(false, std::memory_order_release);
          if (resume_cb) {
            resume_cb(std::move(next_paths));
          }
        });
      }
      ImGui::SameLine();
      if (ImGui::Button("Retry", ImVec2(140, 44))) {
        std::filesystem::path rescanned = ResolveGameDataFolder({});
        std::string s = rescanned.string();
        snprintf(g_locate_state.path_buf, sizeof(g_locate_state.path_buf), "%s", s.c_str());
        ValidateLocatePath(g_locate_state);
        if (g_locate_state.valid) {
          rex_ios_init_vfs_case_map_for_path(s);
          do_launch = true;
        }
      }
    }
    ImGui::End();
#else
    if (ImGui::Begin("Locate Your Peggle 2 Game Source", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
      ImGui::TextWrapped("Bring your own copy of Peggle 2 (Xbox 360). Choose an extracted folder containing Default.xex, or pick a .iso / XBLA STFS / Default.xex file.");
      ImGui::Spacing();
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::InputText("##locate_path", g_locate_state.path_buf, sizeof(g_locate_state.path_buf))) {
        ValidateLocatePath(g_locate_state);
      }
      ImGui::Spacing();
#ifdef _WIN32
      if (ImGui::Button("Browse Folder...")) {
        std::string picked;
        if (BrowseForGameFolder(EnsureAppWindowIcon(), picked)) {
          snprintf(g_locate_state.path_buf, sizeof(g_locate_state.path_buf), "%s", picked.c_str());
          ValidateLocatePath(g_locate_state);
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Browse ISO / XEX / XBLA...")) {
        std::string picked;
        if (BrowseForGameFile(EnsureAppWindowIcon(), picked)) {
          snprintf(g_locate_state.path_buf, sizeof(g_locate_state.path_buf), "%s", picked.c_str());
          ValidateLocatePath(g_locate_state);
        }
      }
#endif
      ImGui::Spacing();
      ImVec4 status_col = g_locate_state.valid ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f) : ImVec4(1.0f, 0.65f, 0.30f, 1.0f);
      ImGui::TextColored(status_col, "%s", g_locate_state.status.c_str());
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::BeginDisabled(!g_locate_state.valid);
      if (ImGui::Button(g_locate_state.mid_session ? "Save & Restart" : "Launch Game",
                        ImVec2(150, 0))) {
        do_launch = true;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (g_locate_state.mid_session) {
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
          g_locate_state.active.store(false, std::memory_order_release);
          g_locate_state.mid_session = false;
        }
      } else if (ImGui::Button("Quit", ImVec2(100, 0))) {
#ifdef _WIN32
        if (HWND hwnd = EnsureAppWindowIcon()) {
          PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
          std::exit(0);
        }
#else
        std::exit(0);
#endif
      }
    }
    ImGui::End();
#endif

    if (do_launch) {
      rex::PathConfig next_paths = g_locate_state.defaults;
      next_paths.game_data_root = g_locate_state.resolved_path;
      const std::filesystem::path cfg_out =
          !next_paths.config_path.empty() ? next_paths.config_path : g_active_config_path;
      SaveGameDataRootToToml(cfg_out, next_paths.game_data_root);
      if (g_locate_state.mid_session) {
        g_locate_state.active.store(false, std::memory_order_release);
        g_locate_state.mid_session = false;
#ifdef _WIN32
        RestartWindowsAppWithToml();
#endif
      } else {
        g_active_game_source = next_paths.game_data_root;
        auto resume_cb = std::move(g_locate_state.resume);
        g_locate_state.active.store(false, std::memory_order_release);
        if (resume_cb) {
          resume_cb(std::move(next_paths));
        }
      }
    }
  }
  const char* UpdateCrashDiagnostics(const rex::RexBootTelemetrySnapshot& boot_tel) {
#if REX_PLATFORM_IOS
    static bool s_written = false;
    static uint32_t s_last_sig = 0;
    static uint64_t s_last_addr = 0;
    static uint64_t s_last_pc = 0;
    static std::string s_demangled_sym;
    if (boot_tel.fault_signal == 0 && boot_tel.fault_addr == 0) {
      return "-";
    }
    if (!s_written || s_last_sig != boot_tel.fault_signal ||
        s_last_addr != boot_tel.fault_addr || s_last_pc != boot_tel.fault_host_pc) {
      const std::string report = RexIosFormatCrashReport(boot_tel, &s_demangled_sym);
      const std::string docs = rex_ios_get_documents_dir();
      if (!docs.empty()) {
        RexIosWriteCrashReportFile(docs + "/last_crash.txt", report);
      }
      s_last_sig = boot_tel.fault_signal;
      s_last_addr = boot_tel.fault_addr;
      s_last_pc = boot_tel.fault_host_pc;
      s_written = true;
    }
    return s_demangled_sym.empty() ? "-" : s_demangled_sym.c_str();
#else
    return boot_tel.fault_sym[0] ? boot_tel.fault_sym : "-";
#endif
  }

  void DrawTelemetryHud(ImGuiIO& io, const rex::RexBootTelemetrySnapshot& boot_tel,
                        const char* crash_sym) {
    int hud_mode = g_opt_hud.load(std::memory_order_relaxed);
    if (hud_mode == kHudHidden) return;

    ImGui::SetNextWindowPos(ImVec2(14, 14), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (!g_menu_open.load(std::memory_order_relaxed)) {
      flags |= ImGuiWindowFlags_NoInputs;
    }

    ImGui::SetNextWindowBgAlpha(0.70f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f)); // Bright green text
    if (ImGui::Begin("##TelemetryHUD", nullptr, flags)) {
      int fps_idx = g_target_fps_index.load(std::memory_order_relaxed);
      int vsync_idx = g_opt_vsync.load(std::memory_order_relaxed);
      int limiter_idx = g_opt_limiter.load(std::memory_order_relaxed);
      bool is_uncapped = (fps_idx == kFpsUncapped) || (limiter_idx == kLimiterUncapped);

      float ren_fps = g_telemetry_rendered_fps.load(std::memory_order_relaxed);
      float host_fps = g_telemetry_host_fps.load(std::memory_order_relaxed);
      float sim_fps = g_telemetry_sim_fps.load(std::memory_order_relaxed);
      float speed = (sim_fps > 0.0f) ? (sim_fps / 60.0f) : 1.00f;
      float ren_ms = (ren_fps > 0.0f) ? (1000.0f / ren_fps) : 0.0f;
      float host_ms = (host_fps > 0.0f) ? (1000.0f / host_fps) : 0.0f;

      int res_idx = g_opt_res.load(std::memory_order_relaxed);
      uint32_t ren_w = 1280;
      uint32_t ren_h = 720;
      float ren_scale = 1.0f;
      switch (res_idx) {
        case kRes180p:  ren_w = 320;  ren_h = 180;  ren_scale = 0.25f; break;
        case kRes360p:  ren_w = 640;  ren_h = 360;  ren_scale = 0.50f; break;
        case kRes720p:  ren_w = 1280; ren_h = 720;  ren_scale = 1.00f; break;
        case kRes1080p: ren_w = 1920; ren_h = 1080; ren_scale = 1.50f; break;
        case kRes1440p: ren_w = 2560; ren_h = 1440; ren_scale = 2.00f; break;
        case kRes4K:    ren_w = 3840; ren_h = 2160; ren_scale = 3.00f; break;
      }
#if REX_PLATFORM_IOS
      if (rex_ios_is_low_power_075x_active()) {
        ren_w = 960;
        ren_h = 540;
        ren_scale = 0.75f;
      }
#endif

      int aim_mode_hud = g_opt_mouse_aim.load(std::memory_order_relaxed);
      int menu_mode_hud = g_opt_menu_mode.load(std::memory_order_relaxed);
      const char* hud_hov = g_telemetry_hover_id.load(std::memory_order_relaxed);
      bool hud_in_gp = rex_monitor::IsInGameplay() &&
                       (rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY);
      RexGpuUiMonitorSnapshotMirror hud_gpu{};
      rex_monitor::PollGpuSnapshot(hud_gpu);
      RexScreenWatcherStatus hud_sw{};
      std::vector<RexCandidateButton> hud_sw_cands;
      rex_monitor::GetScreenWatcherState(hud_sw, hud_sw_cands);

      char hl_buf[24], tgt_buf[24], rgb_buf[24];
      if (hud_sw.has_highlight) snprintf(hl_buf, sizeof(hl_buf), "%.2f,%.2f", hud_sw.hl_u, hud_sw.hl_v);
      else snprintf(hl_buf, sizeof(hl_buf), "-1.00,-1.00");
      if (hud_sw.has_target) snprintf(tgt_buf, sizeof(tgt_buf), "%.2f,%.2f", hud_sw.target_u, hud_sw.target_v);
      else snprintf(tgt_buf, sizeof(tgt_buf), "-1.00,-1.00");
      if (hud_sw.has_cursor_rgb) snprintf(rgb_buf, sizeof(rgb_buf), "%u,%u,%u", hud_sw.cursor_r, hud_sw.cursor_g, hud_sw.cursor_b);
      else snprintf(rgb_buf, sizeof(rgb_buf), "---,---,---");

      const char* boot_stage_name = rex::GetBootStageName(boot_tel.boot_stage);
      auto draw_fault_lines = [&](bool compact) {
        if (boot_tel.fault_signal == 0 && boot_tel.fault_addr == 0) return;
#if REX_PLATFORM_IOS
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
        ImGui::PushTextWrapPos((std::max)(360.0f, io.DisplaySize.x - 44.0f));
#endif
        if (compact) {
          ImGui::Text("Fault: Sig=%u @ 0x%llX Thr=%s GPC=0x%08X GLR=0x%08X",
                      boot_tel.fault_signal,
                      static_cast<unsigned long long>(boot_tel.fault_addr),
                      boot_tel.fault_thread[0] ? boot_tel.fault_thread : "-",
                      boot_tel.fault_guest_pc,
                      boot_tel.fault_guest_lr);
        } else {
          ImGui::Text("Fault:     Sig=%u FaultAddr=0x%llX Thr=%s GPC=0x%08X GLR=0x%08X",
                      boot_tel.fault_signal,
                      static_cast<unsigned long long>(boot_tel.fault_addr),
                      boot_tel.fault_thread[0] ? boot_tel.fault_thread : "-",
                      boot_tel.fault_guest_pc,
                      boot_tel.fault_guest_lr);
        }
#if REX_PLATFORM_IOS
        ImGui::Text("FaultHost: PC=0x%llX LR=0x%llX Insn=0x%08X",
                    static_cast<unsigned long long>(boot_tel.fault_host_pc),
                    static_cast<unsigned long long>(boot_tel.fault_host_lr),
                    boot_tel.fault_insn);
        ImGui::Text("Sym=%s", crash_sym);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
#else
        ImGui::Text("FaultHost: PC=0x%llX LR=0x%llX Insn=0x%08X Sym=%s",
                    static_cast<unsigned long long>(boot_tel.fault_host_pc),
                    static_cast<unsigned long long>(boot_tel.fault_host_lr),
                    boot_tel.fault_insn,
                    crash_sym);
#endif
      };

      auto draw_perf_line = [&](bool minimal) {
#if REX_PLATFORM_IOS
        (void)minimal;
        const RexIosPerfSnapshot ios_perf = rex_ios_get_perf_snapshot();
        ImGui::Text("Perf: CPU=%.2f Enc=%.2f GPU=%.2f Draws=%u TexUp=%u Scale=%.2fx Therm=%s",
                    ios_perf.cpu_ms, ios_perf.enc_ms, ios_perf.gpu_ms,
                    ios_perf.draws_per_frame, ios_perf.tex_uploads_per_frame,
                    ren_scale, rex_ios_get_thermal_state_name());
#else
        const auto gfb = rex::ui::Presenter::GetGuestFrameBreakdown();
        ImGui::Text(minimal ? "Perf: CPU=%.2f GPU=%.2f Draws=%u EDRAM=%s"
                            : "Perf:     CPU=%.2f GPU=%.2f Draws=%u EDRAM=%s",
                    gfb.active_work_ms, gfb.gpu_hw_ms, gfb.draw_calls_per_frame,
                    rex::ui::Presenter::GetActiveEdramPathName());
#endif
      };

      if (hud_mode == kHudVisible) {
        ImGui::Text("[ReXGlue Telemetry | RT=%s]", rex::GetRuntimeBuildTag());
        ImGui::Text("BootStage: %d:%s (0:Init|1:ArenaOK|2:XexOK|3:VFS(%u files)|4:ThreadRun|5:VdSwap) Base=0x%llX%s%s",
                    boot_tel.boot_stage, boot_stage_name, boot_tel.bundle_assets,
                    static_cast<unsigned long long>(boot_tel.guest_base),
                    boot_tel.boot_error[0] ? " Err=" : "",
                    boot_tel.boot_error[0] ? boot_tel.boot_error : "");
        const char* cpu_state_name = rex::GetGuestThreadStateName(boot_tel.guest_state);
        ImGui::Text("GuestCPU:  Ticks=%llu | PC=0x%08X | LR=0x%08X | State=%s%s | VBlanks=%llu",
                    static_cast<unsigned long long>(boot_tel.guest_ticks),
                    boot_tel.guest_pc,
                    boot_tel.guest_lr,
                    cpu_state_name,
                    boot_tel.gpu_fence_wait ? "+GPUWait" : "",
                    static_cast<unsigned long long>(boot_tel.vblanks));
        draw_fault_lines(false);
        ImGui::Text("GuestGPU:  PM4Packets=%llu | Draws=%llu | Shaders=%llu | VdSwaps=%llu",
                    static_cast<unsigned long long>(boot_tel.pm4_packets),
                    static_cast<unsigned long long>(boot_tel.guest_draws),
                    static_cast<unsigned long long>(boot_tel.shaders_compiled),
                    static_cast<unsigned long long>(boot_tel.vd_swaps));
        draw_perf_line(false);
        ImGui::Text("VFS:       OK=%u | Fail=%u | Last=%s",
                    boot_tel.vfs_opens_ok, boot_tel.vfs_opens_fail,
                    boot_tel.last_fail[0] ? boot_tel.last_fail : "None");
        if (hud_in_gp) {
          ImGui::Text("Aim [F2]: %s | Menu [F3]: %s | Screen: GAMEPLAY (gun=0x%08X %.1f deg)",
                      AimModeName(aim_mode_hud), MenuModeName(menu_mode_hud),
                      rex_monitor::GetLastTurretPtr(), rex_monitor::GetLastTurretAngleDeg());
        } else {
          ImGui::Text("Aim [F2]: %s | Menu [F3]: %s | Screen: %s (Cand=%zu)",
                      AimModeName(aim_mode_hud), MenuModeName(menu_mode_hud),
                      hud_sw.screen_name[0] ? hud_sw.screen_name : "UNKNOWN",
                      hud_sw_cands.size());
        }
        ImGui::Text("ScreenWatcher [0.1s]: Motion=%.1f%% | CursorRGB=(%s) | Highlight=(%s) -> MouseBtn=(%s)",
                    hud_sw.motion_pct, rgb_buf, hl_buf, tgt_buf);
        ImGui::Text("Framerate Mode: %s", kFpsPresets[fps_idx].name);
        ImGui::Text("Host Present:   %.1f FPS (%.2f ms | p99=%.2f max=%.2f)",
                    host_fps, host_ms,
                    g_telemetry_host_p99_ms.load(std::memory_order_relaxed),
                    g_telemetry_host_max_ms.load(std::memory_order_relaxed));
        ImGui::Text("Rendered FPS:   %.1f (%.2f ms)", ren_fps, ren_ms);
        const bool guest_ok = (sim_fps >= 57.0f && sim_fps <= 63.0f);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              guest_ok ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(1.0f, 0.9f, 0.2f, 1.0f));
        ImGui::Text("Guest Logic:    %.1f FPS (%.2fx speed)", sim_fps, speed);
        ImGui::PopStyleColor();
        ImGui::Text("Render Res:     %ux%u (%.2fx)", ren_w, ren_h, ren_scale);
        ImGui::Text("VSync:          %s", vsync_idx == kVSyncOff ? "Off (Host Pacer)" : "On (Swapchain)");
        ImGui::Text("FPS Limiter:    %s", is_uncapped ? "Uncapped Present" : "Match Target Hz");
        int tex_idx = g_opt_tex.load(std::memory_order_relaxed);
        const char* tex_names[kTexCount] = { "Original 720p", "2x Bicubic + Sharpen", "Retro CRT / Ultra Sharpen" };
        ImGui::Text("Texture Filter: %s", tex_names[tex_idx]);
        uint32_t active_v = rex::ui::Presenter::active_audio_voice_count();
        uint32_t sub_f = rex::ui::Presenter::submitted_audio_frame_count();
        ImGui::Text("Audio:          XMA=%u/320 Frames=%u", active_v, sub_f);
        const char* active_target =
            (!hud_in_gp && hud_sw.has_target && hud_sw.target_name[0])
                ? hud_sw.target_name
                : (hud_hov ? hud_hov : "None");
        ImGui::Text("Input:          Stick=%-6d Buttons=0x%04X | Target: %s",
                    (int)g_telemetry_stick_lx.load(std::memory_order_relaxed),
                    (unsigned int)g_telemetry_buttons.load(std::memory_order_relaxed),
                    active_target);
        if (g_rec_active.load(std::memory_order_relaxed)) {
          ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC:            tick %llu",
                             static_cast<unsigned long long>(g_rec_tick.load(std::memory_order_relaxed)));
        } else if (g_play_active.load(std::memory_order_relaxed)) {
          ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.0f, 1.0f), "PLAY:           tick %llu / %llu",
                             static_cast<unsigned long long>(g_play_tick.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_play_end_tick.load(std::memory_order_relaxed)));
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
        ImGui::Text("[F1] Settings | [F6] %s | [F7] %s",
                    g_rec_active.load(std::memory_order_relaxed) ? "Stop Rec" : "Rec",
                    g_play_active.load(std::memory_order_relaxed) ? "Stop Play" : "Play");
        ImGui::PopStyleColor();
      } else if (hud_mode == kHudMinimal) {
        const char* rec_tag = g_rec_active.load(std::memory_order_relaxed) ? " [REC]" :
                              (g_play_active.load(std::memory_order_relaxed) ? " [PLAY]" : "");
        const char* min_cpu_state = rex::GetGuestThreadStateName(boot_tel.guest_state);
        ImGui::Text("BootStage: %d:%s (VFS=%u) RT=%s | GuestCPU: Ticks=%llu PC=0x%08X LR=0x%08X %s%s VBlanks=%llu",
                    boot_tel.boot_stage, boot_stage_name, boot_tel.bundle_assets,
                    rex::GetRuntimeBuildTag(),
                    static_cast<unsigned long long>(boot_tel.guest_ticks),
                    boot_tel.guest_pc,
                    boot_tel.guest_lr,
                    min_cpu_state,
                    boot_tel.gpu_fence_wait ? "+GPUWait" : "",
                    static_cast<unsigned long long>(boot_tel.vblanks));
        draw_fault_lines(true);
        ImGui::Text("GuestGPU: PM4=%llu Draws=%llu Shaders=%llu VdSwaps=%llu | VFS: OK=%u Fail=%u Last=%s",
                    static_cast<unsigned long long>(boot_tel.pm4_packets),
                    static_cast<unsigned long long>(boot_tel.guest_draws),
                    static_cast<unsigned long long>(boot_tel.shaders_compiled),
                    static_cast<unsigned long long>(boot_tel.vd_swaps),
                    boot_tel.vfs_opens_ok, boot_tel.vfs_opens_fail,
                    boot_tel.last_fail[0] ? boot_tel.last_fail : "None");
        ImGui::Text("Aim [F2]: %s | Menu [F3]: %s | %s Q=%u",
                    AimModeName(aim_mode_hud), MenuModeName(menu_mode_hud),
                    hud_in_gp ? "GAMEPLAY" : "MENUS", hud_gpu.quad_count);
        ImGui::Text("Host: %.1f FPS (%.1fms) | Ren: %.1f | Sim: %.1f (%.2fx) | %ux%u | Stick=%d%s | [F1]",
                    host_fps, host_ms, ren_fps, sim_fps, speed,
                    ren_w, ren_h,
                    (int)g_telemetry_stick_lx.load(std::memory_order_relaxed), rec_tag);
        draw_perf_line(true);
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
  }

  void DrawSettingsMenu(ImGuiIO& io) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(590, 465), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);

    bool open = true;
    if (ImGui::Begin("ReXGlue Settings (F1)", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Performance & Simulation Cadence");
      ImGui::Separator();
      ImGui::Spacing();

      auto draw_option = [](int index, const char* label, const char* value,
                             std::function<void(int)> on_change) {
        bool is_selected = (g_selected_menu_item == index);
        if (is_selected) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.25f, 1.0f)); // Bright yellow
        }

        ImGui::Text("%s %-20s", is_selected ? ">>" : "  ", label);
        ImGui::SameLine(230);

        char btn_l[16], btn_r[16];
        snprintf(btn_l, sizeof(btn_l), "<##opt%d", index);
        snprintf(btn_r, sizeof(btn_r), ">##opt%d", index);

        if (ImGui::Button(btn_l, ImVec2(24, 0))) {
          g_selected_menu_item = index;
          on_change(-1);
          SaveUserSettings();
        }
        ImGui::SameLine();
        ImGui::Text(" %-22s ", value);
        ImGui::SameLine();
        if (ImGui::Button(btn_r, ImVec2(24, 0))) {
          g_selected_menu_item = index;
          on_change(1);
          SaveUserSettings();
        }

        if (is_selected) {
          ImGui::PopStyleColor();
        }
      };

      // 1. Target FPS / Hz
      int fps_idx = g_target_fps_index.load(std::memory_order_relaxed);
      draw_option(0, "Target FPS / Hz:", kFpsPresets[fps_idx].name, [](int dir) {
        int cur = g_target_fps_index.load(std::memory_order_relaxed);
        g_target_fps_index.store(StepFpsPresetIndex(cur, dir), std::memory_order_relaxed);
      });

      // 2. VSync
      int vsync_idx = g_opt_vsync.load(std::memory_order_relaxed);
      const char* vsync_names[kVSyncCount] = { "Off (Host Pacer)", "On (Swapchain)" };
      draw_option(1, "VSync:", vsync_names[vsync_idx], [](int dir) {
        int cur = g_opt_vsync.load(std::memory_order_relaxed);
        g_opt_vsync.store((cur + dir + kVSyncCount) % kVSyncCount, std::memory_order_relaxed);
      });

      // 3. FPS Limiter Mode
      int limiter_idx = g_opt_limiter.load(std::memory_order_relaxed);
      const char* limiter_names[kLimiterCount] = { "Match Target Hz", "Uncapped Present" };
      draw_option(2, "FPS Limiter Mode:", limiter_names[limiter_idx], [](int dir) {
        int cur = g_opt_limiter.load(std::memory_order_relaxed);
        g_opt_limiter.store((cur + dir + kLimiterCount) % kLimiterCount, std::memory_order_relaxed);
      });

      // 4. Resolution Scale
      int res_idx = g_opt_res.load(std::memory_order_relaxed);
      const char* res_names[kResCount] = { "0.25x (180p)", "0.5x (360p)", "1.0x (720p)", "1.5x (1080p)", "2.0x (1440p)", "3.0x (4K)" };
      draw_option(3, "Resolution Scale:", res_names[res_idx], [](int dir) {
        int cur = g_opt_res.load(std::memory_order_relaxed);
        g_opt_res.store((cur + dir + kResCount) % kResCount, std::memory_order_relaxed);
      });

      // 5. Texture Filtering / Upscale
      int tex_idx = g_opt_tex.load(std::memory_order_relaxed);
      const char* tex_names[kTexCount] = { "Original 720p", "2x Bicubic + Sharpen", "Retro CRT / Ultra Sharpen" };
      draw_option(4, "Texture Filtering:", tex_names[tex_idx], [](int dir) {
        int cur = g_opt_tex.load(std::memory_order_relaxed);
        g_opt_tex.store((cur + dir + kTexCount) % kTexCount, std::memory_order_relaxed);
      });

      // 6. Telemetry HUD
      int hud_idx = g_opt_hud.load(std::memory_order_relaxed);
      const char* hud_names[kHudCount] = { "Visible", "Minimal", "Hidden" };
      draw_option(5, "Telemetry HUD:", hud_names[hud_idx], [](int dir) {
        int cur = g_opt_hud.load(std::memory_order_relaxed);
        g_opt_hud.store((cur + dir + kHudCount) % kHudCount, std::memory_order_relaxed);
      });

      // 7. Audio Buffer Sync
      int audio_idx = g_opt_audio.load(std::memory_order_relaxed);
      const char* audio_names[kAudioCount] = { "Normal", "Safe" };
      draw_option(6, "Audio Buffer Sync:", audio_names[audio_idx], [](int dir) {
        int cur = g_opt_audio.load(std::memory_order_relaxed);
        g_opt_audio.store((cur + dir + kAudioCount) % kAudioCount, std::memory_order_relaxed);
      });

      // 8. Input Style
      int style_idx = g_opt_input_style.load(std::memory_order_relaxed);
      const char* style_names[kInputStyleCount] = {
        "PC Mouse (Hover + Click)", "Mobile Touch Sim (Drag Aim)", "Controller Only"
      };
      draw_option(7, "Input Style:", style_names[style_idx], [](int dir) {
        int cur = g_opt_input_style.load(std::memory_order_relaxed);
        g_opt_input_style.store((cur + dir + kInputStyleCount) % kInputStyleCount, std::memory_order_relaxed);
      });

      // 9. Show UI Colliders
      int col_idx = g_opt_ui_colliders.load(std::memory_order_relaxed);
      const char* col_names[kCollidersCount] = { "Off", "On (Debug Wireframe)" };
      draw_option(8, "Show UI Colliders:", col_names[col_idx], [](int dir) {
        int cur = g_opt_ui_colliders.load(std::memory_order_relaxed);
        g_opt_ui_colliders.store((cur + dir + kCollidersCount) % kCollidersCount, std::memory_order_relaxed);
      });

      // 10. Gameplay Aim Mode [F2]
      int aim_mode_idx = g_opt_mouse_aim.load(std::memory_order_relaxed);
      draw_option(9, "Gameplay Aim [F2]:", AimModeName(aim_mode_idx), [](int dir) {
        int cur = g_opt_mouse_aim.load(std::memory_order_relaxed);
        g_opt_mouse_aim.store((cur + dir + kAimModeCount) % kAimModeCount, std::memory_order_relaxed);
      });

      // 11. Menu Hitbox Mode [F3]
      int menu_mode_idx = g_opt_menu_mode.load(std::memory_order_relaxed);
      draw_option(10, "Menu Hitbox [F3]:", MenuModeName(menu_mode_idx), [](int dir) {
        int cur = g_opt_menu_mode.load(std::memory_order_relaxed);
        g_opt_menu_mode.store((cur + dir + kMenuModeCount) % kMenuModeCount, std::memory_order_relaxed);
      });

      // 12. Mouse Sensitivity
      int sens_idx = g_opt_sens.load(std::memory_order_relaxed);
      const char* sens_names[kSensCount] = { "Low", "Medium", "High", "Very High" };
      draw_option(11, "Mouse Sensitivity:", sens_names[sens_idx], [](int dir) {
        int cur = g_opt_sens.load(std::memory_order_relaxed);
        g_opt_sens.store((cur + dir + kSensCount) % kSensCount, std::memory_order_relaxed);
      });

      // 13. Mouse Button Swap
      int swap_idx = g_opt_mouse_swap.load(std::memory_order_relaxed);
#ifdef _WIN32
      bool win_swapped_now = (GetSystemMetrics(SM_SWAPBUTTON) != 0);
#else
      bool win_swapped_now = false;
#endif
      char swap_auto_label[64];
      snprintf(swap_auto_label, sizeof(swap_auto_label), "Auto (%s)", win_swapped_now ? "Swapped" : "Normal");
      const char* swap_names[kSwapCount] = { swap_auto_label, "Normal", "Swapped" };
      draw_option(12, "Mouse Button Swap:", swap_names[swap_idx], [](int dir) {
        int cur = g_opt_mouse_swap.load(std::memory_order_relaxed);
        g_opt_mouse_swap.store((cur + dir + kSwapCount) % kSwapCount, std::memory_order_relaxed);
      });

      // 14. On-Screen Touch Buttons (A / B / Start)
      int tb_idx = g_opt_touch_controls.load(std::memory_order_relaxed);
      const char* tb_names[kTouchControlsCount] = { "Show", "Hide" };
      draw_option(13, "Touch Buttons:", tb_names[tb_idx], [](int dir) {
        int cur = g_opt_touch_controls.load(std::memory_order_relaxed);
        g_opt_touch_controls.store((cur + dir + kTouchControlsCount) % kTouchControlsCount,
                                   std::memory_order_relaxed);
      });
#if REX_PLATFORM_IOS
      // 15. Automatic 0.75x Render Resolution in Low Power Mode / Thermal Throttle
      int lp_idx = g_opt_low_power_scale.load(std::memory_order_relaxed);
      const char* lp_names[2] = { "Off (Native 720p)", "On (Auto 0.75x)" };
      draw_option(14, "Low Power 0.75x:", lp_names[lp_idx ? 1 : 0], [](int dir) {
        (void)dir;
        int cur = g_opt_low_power_scale.load(std::memory_order_relaxed);
        g_opt_low_power_scale.store(cur ? 0 : 1, std::memory_order_relaxed);
      });
#else
      // 15. EDRAM Path (Auto defaults to RTV on Integrated GPUs)
      int edram_idx = g_opt_edram_mode.load(std::memory_order_relaxed);
      char edram_auto_label[48];
      snprintf(edram_auto_label, sizeof(edram_auto_label), "Auto (RTV on iGPU [%s])",
               rex::ui::Presenter::GetActiveEdramPathName());
      const char* edram_names[kEdramCount] = { edram_auto_label, "RTV", "ROV" };
      draw_option(14, "EDRAM Mode:", edram_names[edram_idx], [](int dir) {
        int cur = g_opt_edram_mode.load(std::memory_order_relaxed);
        int next = (cur + dir + kEdramCount) % kEdramCount;
        g_opt_edram_mode.store(next, std::memory_order_relaxed);
        ApplyEdramModeSetting(next, true);
      });
#endif

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      if (ImGui::Button("Reset Defaults")) {
        ResetUserSettings();
      }
#if REX_PLATFORM_IOS
      ImGui::SameLine();
      if (ImGui::Button("Change Game Source")) {
        g_menu_open.store(false, std::memory_order_relaxed);
        rex_ios_present_game_picker(true, [](const std::string& chosen_path) {
          if (chosen_path.empty()) return;
          const std::string docs = rex_ios_get_documents_dir();
          const std::filesystem::path cfg =
              !g_settings_ini_path.empty()
                  ? (g_settings_ini_path.parent_path() / "peggle2.toml")
                  : (docs.empty() ? std::filesystem::path("peggle2.toml")
                                  : (std::filesystem::path(docs) / "peggle2.toml"));
          SaveGameDataRootToToml(cfg, std::filesystem::path(chosen_path));
        });
      }
#elif defined(_WIN32)
      ImGui::SameLine();
      if (ImGui::Button("Change Game Source")) {
        g_menu_open.store(false, std::memory_order_relaxed);
        g_locate_state.mid_session = true;
        g_locate_state.defaults.config_path =
            !g_active_config_path.empty() ? g_active_config_path : (GetExeDirectory() / "peggle2.toml");
        std::string cur = g_active_game_source.string();
        snprintf(g_locate_state.path_buf, sizeof(g_locate_state.path_buf), "%s", cur.c_str());
        g_locate_state.needs_validate = true;
        g_locate_state.active.store(true, std::memory_order_release);
      }
#endif
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.65f, 0.90f, 0.65f, 1.0f), "(Auto-saved to peggle2_settings.ini)");
      ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                         "Nav: [Up/Down] Select  |  [Left/Right/Enter] Adjust  |  [F1/Esc] Close");
    }
    ImGui::End();

    if (!open) {
      g_menu_open.store(false, std::memory_order_relaxed);
    }
  }

  void DrawMobileHudToggle(ImGuiIO& io) {
    const bool is_mobile =
#if REX_PLATFORM_IOS
        true;
#else
        (g_opt_input_style.load(std::memory_order_relaxed) == kInputMobileTouch);
#endif
    ViewportRect vp = Compute16x9Viewport(io.DisplaySize.x, io.DisplaySize.y);
    if (!is_mobile) {
      g_ios_corner_held_mask.store(0, std::memory_order_relaxed);
      g_ios_hud_toggle_rect.active.store(false, std::memory_order_relaxed);
      g_ios_corner_strip_rect.active.store(false, std::memory_order_relaxed);
      return;
    }

    // Compact mobile HUD / Settings toggle button in top-left corner.
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGuiWindowFlags btn_flags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##MobileHudToggle", nullptr, btn_flags)) {
      if (ImGui::Button(g_menu_open.load(std::memory_order_relaxed) ? "Close Menu" : "Menu")) {
        g_menu_open.store(!g_menu_open.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
      }
      ImGui::SameLine();
      if (ImGui::Button("HUD")) {
        int cur = g_opt_hud.load(std::memory_order_relaxed);
        g_opt_hud.store((cur + 1) % kHudCount, std::memory_order_relaxed);
      }
      ImVec2 p = ImGui::GetWindowPos();
      ImVec2 s = ImGui::GetWindowSize();
      UpdateMobileExcludeRect(g_ios_hud_toggle_rect, true, p.x, p.y, p.x + s.x, p.y + s.y,
                              vp.x, vp.y, vp.w, vp.h);
    }
    ImGui::End();

    // Compact semi-transparent bottom-right A / B / Start touch strip (toggled in Settings).
    const bool show_strip =
        !g_menu_open.load(std::memory_order_relaxed) &&
        (g_opt_touch_controls.load(std::memory_order_relaxed) == kTouchControlsShow);
    if (!show_strip) {
      g_ios_corner_held_mask.store(0, std::memory_order_relaxed);
      g_ios_corner_strip_rect.active.store(false, std::memory_order_relaxed);
      return;
    }

    const bool in_gameplay = rex_monitor::IsInGameplay() &&
                             (rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 14.0f, io.DisplaySize.y - 14.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.28f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    uint16_t held_mask = 0;
    if (ImGui::Begin("##MobileCornerStrip", nullptr, btn_flags)) {
      auto corner_btn = [&](const char* lbl, uint16_t gp_mask, const ImVec4& col) {
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(col.x, col.y, col.z, std::min(1.0f, col.w + 0.25f)));
        if (ImGui::Button(lbl, ImVec2(0.0f, 40.0f))) {
          g_ios_corner_latch_mask.fetch_or(gp_mask, std::memory_order_acq_rel);
        }
        if (ImGui::IsItemActive()) {
          held_mask |= gp_mask;
        }
        ImGui::PopStyleColor(3);
      };
      corner_btn(in_gameplay ? " FIRE (A) " : "   A   ", 0x1000, ImVec4(0.16f, 0.65f, 0.30f, 0.48f));
      ImGui::SameLine();
      corner_btn("   B   ", 0x2000, ImVec4(0.72f, 0.22f, 0.22f, 0.45f));
      ImGui::SameLine();
      corner_btn(" Start ", 0x0010, ImVec4(0.22f, 0.36f, 0.68f, 0.45f));
      ImVec2 p = ImGui::GetWindowPos();
      ImVec2 s = ImGui::GetWindowSize();
      UpdateMobileExcludeRect(g_ios_corner_strip_rect, true, p.x, p.y, p.x + s.x, p.y + s.y,
                              vp.x, vp.y, vp.w, vp.h);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    g_ios_corner_held_mask.store(held_mask, std::memory_order_relaxed);
  }
};

void Peggle2App::OnPreSetup(rex::RuntimeConfig& config) {
  rex_monitor::Init();
#if REX_PLATFORM_IOS
  config.gpu_plugin.clear();
  config.graphics = rex_ios_create_graphics_system();
  config.audio_factory = [](rex::runtime::FunctionDispatcher* fd) {
    return rex_ios_create_audio_system(fd);
  };
  config.input_factory = [](bool) { return rex_ios_create_input_system(); };
#else
  config.gpu_plugin = "xenos";
#endif
  std::string replay_path = REXCVAR_GET(replay_input);
  std::string record_path = REXCVAR_GET(record_input);
  if (g_test_replay_mode.load(std::memory_order_relaxed)) {
    // Wait until Title Screen (PLAY_BTN) renders before starting replay.
  } else if (!replay_path.empty()) {
    StartInputReplay(replay_path);
  } else if (!record_path.empty()) {
    StartInputRecording(record_path);
  }
}

bool Peggle2App::OnWindowCloseRequested() {
  StopInputReplay();
  StopInputRecording();
  rex_monitor::Shutdown();
  return true;
}

void Peggle2App::OnShutdown() {
  StopInputReplay();
  StopInputRecording();
  rex_monitor::Shutdown();
}

void Peggle2App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  if (drawer) {
    new ReXGlueSettingsDialog(drawer);
  }
}

static std::filesystem::path ResolveGameDataFolder(const std::filesystem::path& requested) {
  auto check_candidate = [](const std::filesystem::path& raw,
                            std::filesystem::path& out) -> bool {
    if (raw.empty()) return false;
    std::error_code ec;
    const std::filesystem::path norm = NormalizeGameSourceCandidate(raw);
    if (IsValidGameDataPath(norm)) {
      out = std::filesystem::absolute(norm, ec);
      return true;
    }
    if (auto found = FindValidGameInFolder(norm); !found.empty()) {
      out = std::filesystem::absolute(found, ec);
      return true;
    }
    return false;
  };

#if REX_PLATFORM_IOS
  // iOS resolution order: (a) Documents/game/, (b) Peggle2GameBookmark, (c) bundle assets/.
  if (const std::string ios_src = rex_ios_resolve_startup_game_source(); !ios_src.empty()) {
    std::filesystem::path p(ios_src);
    if (IsValidGameDataPath(p)) return p;
  }
  if (IsValidGameDataPath(requested)) return std::filesystem::absolute(requested);
  return {};
#else
  std::filesystem::path resolved;
  const std::filesystem::path cwd = std::filesystem::current_path();
  const std::filesystem::path exe_dir = GetExeDirectory();

  // (a) Explicit CLI argument (--game, drag-and-drop positional, or runtime override)
  if (check_candidate(requested, resolved)) return resolved;
  if (const std::string cli_game = REXCVAR_GET(game); !cli_game.empty()) {
    if (check_candidate(cli_game, resolved)) return resolved;
    if (check_candidate(cwd / cli_game, resolved)) return resolved;
    if (check_candidate(exe_dir / cli_game, resolved)) return resolved;
  }
  if (!g_cli_positional_game.empty()) {
    if (check_candidate(g_cli_positional_game, resolved)) return resolved;
    if (check_candidate(cwd / g_cli_positional_game, resolved)) return resolved;
    if (check_candidate(exe_dir / g_cli_positional_game, resolved)) return resolved;
  }

  // (b) Saved game_path (or legacy game_data_root) in peggle2.toml
  for (const std::string& toml_val : {REXCVAR_GET(game_path), REXCVAR_GET(game_data_root)}) {
    if (toml_val.empty()) continue;
    if (check_candidate(toml_val, resolved)) return resolved;
    if (check_candidate(cwd / toml_val, resolved)) return resolved;
    if (check_candidate(exe_dir / toml_val, resolved)) return resolved;
  }

  // (c) Local ./game/ folder next to peggle2.exe (plus dev workspace fallbacks)
  if (check_candidate(exe_dir / "game", resolved)) return resolved;
  if (check_candidate(cwd / "game", resolved)) return resolved;
  if (check_candidate(exe_dir, resolved)) return resolved;
  if (check_candidate(cwd, resolved)) return resolved;
  if (check_candidate(exe_dir / ".." / ".." / ".." / "game", resolved)) {
    return std::filesystem::weakly_canonical(resolved);
  }
  if (check_candidate("C:/Peggle2Recomp/game", resolved)) return resolved;

  return {};
#endif
}

static std::filesystem::path ResolveConfigPath(const std::filesystem::path& requested) {
  std::error_code ec;
  if (!requested.empty() && std::filesystem::is_regular_file(requested, ec)) {
    return requested;
  }

#if REX_PLATFORM_IOS
  const std::string docs_str = rex_ios_get_documents_dir();
  const std::string bundle_str = rex_ios_get_bundle_resource_dir();
  if (!docs_str.empty()) {
    std::filesystem::path docs_toml = std::filesystem::path(docs_str) / "peggle2.toml";
    if (!std::filesystem::is_regular_file(docs_toml, ec) && !bundle_str.empty()) {
      std::filesystem::path bundle_toml = std::filesystem::path(bundle_str) / "peggle2.toml";
      if (std::filesystem::is_regular_file(bundle_toml, ec)) {
        std::filesystem::create_directories(docs_toml.parent_path(), ec);
        std::filesystem::copy_file(bundle_toml, docs_toml,
                                   std::filesystem::copy_options::skip_existing, ec);
      }
    }
    if (std::filesystem::is_regular_file(docs_toml, ec)) {
      return docs_toml;
    }
  }
  if (!bundle_str.empty()) {
    std::filesystem::path bundle_toml = std::filesystem::path(bundle_str) / "peggle2.toml";
    if (std::filesystem::is_regular_file(bundle_toml, ec)) {
      return bundle_toml;
    }
  }
#endif

  auto cwd = std::filesystem::current_path();
  if (std::filesystem::is_regular_file(cwd / "peggle2.toml", ec)) {
    return cwd / "peggle2.toml";
  }

  auto exe_dir = GetExeDirectory();
  if (std::filesystem::is_regular_file(exe_dir / "peggle2.toml", ec)) {
    return exe_dir / "peggle2.toml";
  }

  if (std::filesystem::is_regular_file("C:/Peggle2Recomp/peggle2/out/build/win-amd64-release/peggle2.toml", ec)) {
    return "C:/Peggle2Recomp/peggle2/out/build/win-amd64-release/peggle2.toml";
  }

  return requested;
}

void Peggle2App::OnConfigurePaths(rex::PathConfig& paths) {
  ArmStartupEnvWatchdogOnce();
  if (auto drag_arg = GetArgument("game"); drag_arg && !drag_arg->empty()) {
    g_cli_positional_game = *drag_arg;
  }
  if (IsEnvFlagEnabled("PEGGLE_TEST_COLLIDERS") || REXCVAR_GET(test_colliders) ||
      HasCmdFlag("--test_colliders")) {
    const bool tc_log = IsEnvFlagEnabled("PEGGLE_TEST_COLLIDERS");
    if (tc_log) {
      AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log",
                      "[TC] OnConfigurePaths entered -> running RunColliderSelfCheck\n");
    }
    RunColliderSelfCheck();
    if (tc_log) {
      AppendTcFileLog("C:/Peggle2Recomp/tc_audit.log",
                      "[TC] OnConfigurePaths complete -> TerminateProcess(0)\n");
    }
    fflush(stdout);
    ForceTerminateProcess(0);
  }
#if REX_PLATFORM_IOS
  if (auto docs_str = rex_ios_get_documents_dir(); !docs_str.empty()) {
    std::filesystem::path docs(docs_str);
    paths.user_data_root = docs / "UserData";
    paths.cache_root = docs / "cache";
    if (paths.config_path.empty() || paths.config_path.parent_path() == GetExeDirectory()) {
      paths.config_path = docs / "peggle2.toml";
    }
  }
#endif
  paths.config_path = ResolveConfigPath(paths.config_path);
  if (paths.config_path.empty()) {
    paths.config_path = GetExeDirectory() / "peggle2.toml";
  }
  g_active_config_path = paths.config_path;
  if (std::filesystem::exists(paths.config_path)) {
    rex::cvar::LoadConfig(paths.config_path);
  }
#if REX_PLATFORM_IOS
  if (REXCVAR_GET(license_mask) == 0u) {
    rex::cvar::SetFlagByName("license_mask", "1");
  }
#endif
  std::filesystem::path settings_dir = paths.config_path.empty()
      ? GetExeDirectory()
      : paths.config_path.parent_path();
#if REX_PLATFORM_IOS
  if (auto docs_str = rex_ios_get_documents_dir(); !docs_str.empty()) {
    settings_dir = std::filesystem::path(docs_str);
  }
#else
  if (settings_dir.empty()) settings_dir = GetExeDirectory();
#endif
  LoadUserSettings(settings_dir / "peggle2_settings.ini");
  if (rex::cvar::HasNonDefaultValue("target_fps")) {
    if (int idx = TargetFpsIndexFromHz(REXCVAR_GET(target_fps)); idx >= 0) {
      g_target_fps_index.store(idx, std::memory_order_relaxed);
    }
  } else if (rex::cvar::HasNonDefaultValue("TargetFPS")) {
    if (int idx = TargetFpsIndexFromHz(REXCVAR_GET(TargetFPS)); idx >= 0) {
      g_target_fps_index.store(idx, std::memory_order_relaxed);
    }
  }
#if !REX_PLATFORM_IOS
  if (!g_ini_had_edram_mode && rex::cvar::HasNonDefaultValue("render_target_path_d3d12")) {
    const std::string p = REXCVAR_QUERY(std::string, render_target_path_d3d12);
    if (p == "rtv") g_opt_edram_mode.store(kEdramRtv, std::memory_order_relaxed);
    else if (p == "rov") g_opt_edram_mode.store(kEdramRov, std::memory_order_relaxed);
    else g_opt_edram_mode.store(kEdramAuto, std::memory_order_relaxed);
  }
  ApplyEdramModeSetting(g_opt_edram_mode.load(std::memory_order_relaxed), false);
#endif
  if (IsTestReplayMode()) {
    const uint64_t boot_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    g_test_replay_boot_ms.store(boot_ms, std::memory_order_relaxed);
    g_test_replay_started.store(false, std::memory_order_relaxed);
    g_test_replay_mode.store(true, std::memory_order_relaxed);
    g_opt_ui_colliders.store(kCollidersOn, std::memory_order_relaxed);
#if REX_PLATFORM_IOS
    std::string audit_path = rex_ios_get_documents_dir();
    audit_path = audit_path.empty() ? std::string("replay_hitbox_audit.log")
                                    : (audit_path + "/replay_hitbox_audit.log");
    rex_monitor::EnableReplayHitboxAudit(true, audit_path.c_str());
#else
    rex_monitor::EnableReplayHitboxAudit(true, "replay_hitbox_audit.log");
#endif
    printf("[TEST-REPLAY] Enabled auto-replay mode (colliders=On, timeout=55s)\n");
    fflush(stdout);
  }
  if (g_target_fps_index.load(std::memory_order_relaxed) == kFps30) {
    g_target_fps_index.store(kDefaultTargetFpsIdx, std::memory_order_relaxed);
  }
#if REX_PLATFORM_IOS
  if (int cur_idx = g_target_fps_index.load(std::memory_order_relaxed);
      cur_idx == kFps72 || cur_idx == kFps144 || cur_idx == kFpsUncapped) {
    g_target_fps_index.store(cur_idx == kFps72 ? kFps60 : kFps120, std::memory_order_relaxed);
  }
#endif
  const uint32_t startup_hz = GetActiveTargetFps();
  const int startup_vsync = g_opt_vsync.load(std::memory_order_relaxed);
  rex::cvar::SetFlagByName("vsync", startup_vsync == kVSyncOn ? "true" : "false");
  rex::cvar::SetFlagByName("VSyncInterval", startup_vsync == kVSyncOn ? "1" : "0");
  rex::cvar::SetFlagByName("TargetFPS", std::to_string(startup_hz));
  rex::cvar::SetFlagByName("target_fps", std::to_string(startup_hz));
  rex::cvar::SetFlagByName("display_refresh_rate", std::to_string(startup_hz));
  rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");
  rex::cvar::SetFlagByName("guest_time_scalar", "1.0");
#if REX_PLATFORM_IOS
  rex_ios_set_target_fps(startup_hz);
#endif
  paths.game_data_root = ResolveGameDataFolder(paths.game_data_root);
}

std::optional<rex::PathConfig> Peggle2App::OnFinalizePaths(const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  rex::PathConfig resolved = defaults;
  resolved.config_path = ResolveConfigPath(resolved.config_path);
  if (resolved.config_path.empty()) {
#if REX_PLATFORM_IOS
    std::string docs = rex_ios_get_documents_dir();
    resolved.config_path = (!docs.empty() ? std::filesystem::path(docs) : GetExeDirectory()) / "peggle2.toml";
#else
    resolved.config_path = GetExeDirectory() / "peggle2.toml";
#endif
  }
  g_active_config_path = resolved.config_path;
  resolved.game_data_root = ResolveGameDataFolder(resolved.game_data_root);

  if (REXCVAR_GET(locate_game) || !IsValidGameDataPath(resolved.game_data_root)) {
    g_locate_state.mid_session = false;
    g_locate_state.defaults = resolved;
    g_locate_state.resume = std::move(resume);
    std::string initial = resolved.game_data_root.string();
    snprintf(g_locate_state.path_buf, sizeof(g_locate_state.path_buf), "%s", initial.c_str());
    g_locate_state.needs_validate = true;
    g_locate_state.active.store(true, std::memory_order_release);
#if REX_PLATFORM_IOS
    rex_ios_present_game_picker(false, [](const std::string& chosen_path) {
      if (chosen_path.empty()) return;
      rex::PathConfig next_paths = g_locate_state.defaults;
      next_paths.game_data_root = std::filesystem::path(chosen_path);
      SaveGameDataRootToToml(next_paths.config_path, next_paths.game_data_root);
      rex_ios_init_vfs_case_map_for_path(chosen_path);
      auto resume_cb = std::move(g_locate_state.resume);
      g_locate_state.active.store(false, std::memory_order_release);
      if (resume_cb) {
        resume_cb(std::move(next_paths));
      }
    });
#endif
    return std::nullopt;
  }
  g_active_game_source = resolved.game_data_root;
#if REX_PLATFORM_IOS
  rex_ios_init_vfs_case_map_for_path(resolved.game_data_root.string());
#endif
  if (rex::GetBootTelemetrySnapshot().bundle_assets == 0) {
    std::error_code ec;
    if (std::filesystem::is_directory(resolved.game_data_root, ec)) {
      uint32_t asset_count = 0;
      for (const auto& entry :
           std::filesystem::recursive_directory_iterator(resolved.game_data_root, ec)) {
        if (entry.is_regular_file(ec)) ++asset_count;
      }
      rex::SetBundleAssetCount(asset_count);
    } else if (std::filesystem::is_regular_file(resolved.game_data_root, ec)) {
      rex::SetBundleAssetCount(1);
    }
  }
  return resolved;
}

// ---------------------------------------------------------------------------
// Background Telemetry Reporter & Hotkey Watcher
// ---------------------------------------------------------------------------
static std::thread g_cadence_reporter([] {
  static uint64_t last_rendered_count = 0;
  static uint64_t last_host_count = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (rex::IsAppBackgroundPaused()) continue;
    if (g_test_replay_mode.load(std::memory_order_relaxed)) {
      const uint64_t boot_ms = g_test_replay_boot_ms.load(std::memory_order_relaxed);
      const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
      if (boot_ms != 0 && now_ms > boot_ms && (now_ms - boot_ms) >= 55000ull) {
        TriggerTestReplayExit("55s safety timeout", 0);
      }
    }
    uint64_t cur_rendered = rex::ui::Presenter::rendered_frame_count();
    uint32_t ren = static_cast<uint32_t>(cur_rendered - last_rendered_count);
    last_rendered_count = cur_rendered;

    uint64_t cur_host = rex::ui::Presenter::host_present_count();
    uint32_t host = static_cast<uint32_t>(cur_host - last_host_count);
    last_host_count = cur_host;

    uint32_t f2 = g_f2c8_updates.exchange(0);
    uint32_t vds = g_vdswap_calls.exchange(0, std::memory_order_relaxed);
    uint32_t guest_fps_val = (vds > 0) ? vds : f2;
    g_telemetry_rendered_fps.store((float)ren, std::memory_order_relaxed);
    g_telemetry_host_fps.store((float)host, std::memory_order_relaxed);
    g_telemetry_sim_fps.store((float)guest_fps_val, std::memory_order_relaxed);

    FrameTimeSummary gft = g_guest_ft_window.Consume();
    FrameTimeSummary hft = g_host_ft_window.Consume();
    if (hft.count > 0) {
      g_telemetry_host_p99_ms.store(hft.p99, std::memory_order_relaxed);
      g_telemetry_host_max_ms.store(hft.max, std::memory_order_relaxed);
    }
    if (gft.count > 0) {
      g_telemetry_guest_p99_ms.store(gft.p99, std::memory_order_relaxed);
      g_telemetry_guest_max_ms.store(gft.max, std::memory_order_relaxed);
    }

    rex::ui::ProfilingHudMetrics pm{};
    const bool has_pm = rex::ui::Presenter::GetActiveProfilingHudMetrics(pm);
    int fps_idx = g_target_fps_index.load(std::memory_order_relaxed);
    float speed = (guest_fps_val > 0) ? ((float)guest_fps_val / 60.0f) : 1.00f;
    printf("[Framerate Mode: %s | RT=%s] CADENCE 1s: Host Present=%u, Rendered FPS=%u, VdSwaps=%u, Sim=%u (%.2fx speed) | "
           "HostFT(p50=%.2f p95=%.2f p99=%.2f max=%.2f >20ms=%u >33ms=%u) | "
           "GuestFT(p50=%.2f p95=%.2f p99=%.2f max=%.2f >20ms=%u) | "
           "Perf(cpu=%.2f pres=%.2f)\n",
           kFpsPresets[fps_idx].name, rex::GetRuntimeBuildTag(), host, ren, vds, f2, speed,
           hft.p50, hft.p95, hft.p99, hft.max, hft.over20, hft.over33,
           gft.p50, gft.p95, gft.p99, gft.max, gft.over20,
           has_pm ? pm.avg_cpu_ms : -1.0f, has_pm ? pm.avg_present_ms : -1.0f);
    fflush(stdout);
  }
});

// ---------------------------------------------------------------------------
// XAudio2 Chamberlin SVF Biquad Stability Hook (0x82332838)
// Prevents NaN/Inf delay state latchup on boulder multi-peg hits (which pause menu clears).
// ---------------------------------------------------------------------------
DECLARE_REX_FUNC(sub_82332838);
extern "C" REX_FUNC(sub_82332838) {
  REX_FUNC_PROLOGUE();
  uint32_t svf_obj = ctx.r3.u32;
  uint32_t out_desc = ctx.r7.u32;
  uint32_t channels = svf_obj ? REX_LOAD_U32(svf_obj + 96) : 0;

  auto sanitize_delay_pair = [&](uint32_t obj, uint32_t ch_count) {
    if (!obj || ch_count == 0 || ch_count > 8) return;
    uint32_t lp_ptr = REX_LOAD_U32(obj + 84);
    uint32_t bp_ptr = REX_LOAD_U32(obj + 88);
    bool corrupted = false;
    for (uint32_t ptr : { lp_ptr, bp_ptr }) {
      if (!ptr) continue;
      for (uint32_t c = 0; c < ch_count; ++c) {
        float s = std::bit_cast<float>(REX_LOAD_U32(ptr + c * 4));
        if (!std::isfinite(s) || std::abs(s) > 32.0f) {
          corrupted = true;
          break;
        }
      }
      if (corrupted) break;
    }
    if (corrupted) {
      for (uint32_t ptr : { lp_ptr, bp_ptr }) {
        if (!ptr) continue;
        for (uint32_t c = 0; c < ch_count; ++c) {
          REX_STORE_U32(ptr + c * 4, 0);
        }
      }
    }
  };

  if (svf_obj) {
    uint32_t filter_type = REX_LOAD_U32(svf_obj + 160);
    float one_over_q = std::bit_cast<float>(REX_LOAD_U32(svf_obj + 164));
    float frequency = std::bit_cast<float>(REX_LOAD_U32(svf_obj + 168));
    one_over_q = std::isfinite(one_over_q) ? std::clamp(one_over_q, 0.01f, 1.5f) : 1.0f;
    frequency = std::isfinite(frequency) ? std::clamp(frequency, 0.001f, 1.0f) : 1.0f;
    // Keep Chamberlin SVF stable (f * (f + 1/Q) < 2) when active filter is engaged.
    if (filter_type != 0 && !(frequency == 1.0f && one_over_q == 1.0f)) {
      float max_stable_f = 0.5f * (std::sqrt(one_over_q * one_over_q + 7.92f) - one_over_q);
      frequency = std::min(frequency, max_stable_f);
    }
    REX_STORE_U32(svf_obj + 164, std::bit_cast<uint32_t>(one_over_q));
    REX_STORE_U32(svf_obj + 168, std::bit_cast<uint32_t>(frequency));
    sanitize_delay_pair(svf_obj, channels);
  }

  __imp__sub_82332838(ctx, base);

  if (svf_obj) {
    sanitize_delay_pair(svf_obj, channels);
  }
  if (out_desc && channels > 0 && channels <= 8) {
    uint32_t out_buf = REX_LOAD_U32(out_desc + 0);
    uint32_t out_valid = REX_LOAD_U32(out_desc + 4);
    uint32_t out_frames = REX_LOAD_U32(out_desc + 8);
    if (out_buf && out_valid && out_frames > 0 && out_frames <= 1024) {
      uint32_t total = out_frames * channels;
      for (uint32_t i = 0; i < total; ++i) {
        float s = std::bit_cast<float>(REX_LOAD_U32(out_buf + i * 4));
        if (!std::isfinite(s)) {
          REX_STORE_U32(out_buf + i * 4, 0);
        } else if (std::abs(s) > 1.0f) {
          REX_STORE_U32(out_buf + i * 4, std::bit_cast<uint32_t>(std::tanhf(s)));
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Layer 1 Guest Hooks: Turret (0x8223BD10 / 0x821A7398) + Input + UI Focus
// ---------------------------------------------------------------------------
DECLARE_REX_FUNC(sub_8223BD10);
DECLARE_REX_FUNC(sub_821A7398);
extern "C" REX_FUNC(sub_8223BD10) {
  REX_FUNC_PROLOGUE();
  uint32_t board_obj = ctx.r3.u32;
  g_last_board_obj.store(board_obj, std::memory_order_relaxed);
  g_guest_gameplay_frame_tick.store(
      g_current_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
  __imp__sub_8223BD10(ctx, base);
  if (!board_obj) return;
  uint32_t gun_ptr = REX_LOAD_U32(board_obj + 204);
  if (!gun_ptr) return;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  g_last_aim_update_ms.store(now_ms, std::memory_order_relaxed);

  // Track Balljector gun state (gun+528 BE: 0=idle/intro/in-flight, 2=loading, 3=ready, 4=firing) adopted from peggle2-mac.
  static uint32_t s_prev_gun_ptr = 0;
  static int64_t s_prev_aim_ms = 0;
  static bool s_gun_fired = false;
  if (gun_ptr != s_prev_gun_ptr || (now_ms - s_prev_aim_ms) > 250) {
    s_gun_fired = false;
    s_prev_gun_ptr = gun_ptr;
  }
  s_prev_aim_ms = now_ms;
  const uint32_t gun_state = REX_LOAD_U32(gun_ptr + 528);
  if (gun_state == 4) {
    s_gun_fired = true;
  }
  const bool aimable = (gun_state == 2 || gun_state == 3 || (gun_state == 0 && s_gun_fired));

  float prev_x = std::bit_cast<float>(REX_LOAD_U32(gun_ptr + 536));
  float prev_y = std::bit_cast<float>(REX_LOAD_U32(gun_ptr + 540));
  float cur_angle = std::atan2(prev_x, prev_y);
  bool locked = g_mouse_direct_active.load(std::memory_order_relaxed) && aimable;

  if (locked) {
    float angle = g_mouse_target_angle.load(std::memory_order_relaxed);
    float target_x = std::sin(angle);
    float target_y = std::cos(angle);
    uint32_t saved_r1 = ctx.r1.u32;
    uint32_t vec_ptr = saved_r1 - 16;
    ctx.r1.u32 = saved_r1 - 64;
    REX_STORE_U32(vec_ptr + 0, std::bit_cast<uint32_t>(target_x));
    REX_STORE_U32(vec_ptr + 4, std::bit_cast<uint32_t>(target_y));
    ctx.r3.u64 = gun_ptr;
    ctx.r4.u64 = vec_ptr;
    __imp__sub_821A7398(ctx, base);
    ctx.r1.u32 = saved_r1;
    if (std::abs(target_x - prev_x) > 1e-4f || std::abs(target_y - prev_y) > 1e-4f) {
      REX_STORE_U8(board_obj + 9, 1);
    }
  }

  float cur_x = std::bit_cast<float>(REX_LOAD_U32(gun_ptr + 536));
  float cur_y = std::bit_cast<float>(REX_LOAD_U32(gun_ptr + 540));
  float final_angle = std::atan2(cur_x, cur_y);
  g_real_guest_turret_angle.store(final_angle, std::memory_order_relaxed);
  rex_monitor::NotifyGameplayTurretTick(gun_ptr, cur_angle, final_angle, locked);
}

extern "C" REX_FUNC(sub_821A7398) {
  REX_FUNC_PROLOGUE();
  __imp__sub_821A7398(ctx, base);
  uint32_t gun_ptr = ctx.r3.u32;
  if (gun_ptr >= 0x40000000u && gun_ptr < 0x7FFF0000u) {
    float cx = std::bit_cast<float>(REX_LOAD_U32(gun_ptr + 536));
    float cy = std::bit_cast<float>(REX_LOAD_U32(gun_ptr + 540));
    rex_monitor::NotifyGameplayTurretTick(gun_ptr, std::atan2(cx, cy), std::atan2(cx, cy), false);
  }
}

DECLARE_REX_FUNC(sub_8245F948);
extern "C" REX_FUNC(sub_8245F948) {
  REX_FUNC_PROLOGUE();
  uint32_t mgr = ctx.r3.u32;
  uint32_t r4 = ctx.r4.u32;
  __imp__sub_8245F948(ctx, base);
  if (mgr >= 0x40000000u && mgr < 0x7FFF0000u) {
    uint16_t btns = REX_LOAD_U16(mgr + 556);
    static std::unordered_map<uint32_t, uint16_t> s_prev_btns_by_mgr;
    uint16_t& prev_btns = s_prev_btns_by_mgr[mgr];
    if (btns != prev_btns) {
      rex_monitor::RecordGuestInputCall("sub_8245F948_GamepadMgr", 0x8245F948, mgr, r4,
                                        ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                        ctx.r9.u32, ctx.r10.u32, ctx.f1.f64, ctx.f2.f64,
                                        ctx.f3.f64, ctx.f4.f64, base, mgr + 552, btns,
                                        true);
      prev_btns = btns;
    }
  }
}

DECLARE_REX_FUNC(sub_82490FA8);
extern "C" REX_FUNC(sub_82490FA8) {
  REX_FUNC_PROLOGUE();
  rex_monitor::RecordGuestInputCall("sub_82490FA8_BtnDown", 0x82490FA8, ctx.r3.u32, ctx.r4.u32,
                                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                    ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                    base, ctx.r3.u32, uint16_t(ctx.r4.u32), true);
  __imp__sub_82490FA8(ctx, base);
}

DECLARE_REX_FUNC(sub_82491000);
extern "C" REX_FUNC(sub_82491000) {
  REX_FUNC_PROLOGUE();
  rex_monitor::RecordGuestInputCall("sub_82491000_BtnUp", 0x82491000, ctx.r3.u32, ctx.r4.u32,
                                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                    ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                    base, ctx.r3.u32, uint16_t(ctx.r4.u32), false);
  __imp__sub_82491000(ctx, base);
}

DECLARE_REX_FUNC(sub_82491058);
extern "C" REX_FUNC(sub_82491058) {
  REX_FUNC_PROLOGUE();
  if (std::abs(ctx.f1.f64) > 0.1) {
    rex_monitor::RecordGuestInputCall("sub_82491058_Trigger", 0x82491058, ctx.r3.u32, ctx.r4.u32,
                                      ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                      ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                      base, ctx.r3.u32, 0, false);
  }
  __imp__sub_82491058(ctx, base);
}

DECLARE_REX_FUNC(sub_824910D0);
extern "C" REX_FUNC(sub_824910D0) {
  REX_FUNC_PROLOGUE();
  if (std::abs(ctx.f1.f64) > 0.15) {
    rex_monitor::RecordGuestInputCall("sub_824910D0_StickAxis", 0x824910D0, ctx.r3.u32, ctx.r4.u32,
                                      ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                      ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                      base, ctx.r3.u32, 0, false);
  }
  __imp__sub_824910D0(ctx, base);
}

DECLARE_REX_FUNC(sub_821C2720);
extern "C" REX_FUNC(sub_821C2720) {
  REX_FUNC_PROLOGUE();
  uint32_t obj = ctx.r3.u32;
  uint32_t vt = (obj >= 0x40000000u && obj < 0x7FFF0000u) ? REX_LOAD_U32(obj) : 0;
  rex_monitor::ReportGuestWidget(obj, vt, 0, 0, 0, 0, false, true, "InFocusEnter");
  rex_monitor::RecordGuestInputCall("sub_821C2720_InFocusEnter", 0x821C2720, obj, ctx.r4.u32,
                                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                    ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                    base, obj, 0, false);
  __imp__sub_821C2720(ctx, base);
}

DECLARE_REX_FUNC(sub_821C24C8);
extern "C" REX_FUNC(sub_821C24C8) {
  REX_FUNC_PROLOGUE();
  uint32_t obj = ctx.r3.u32;
  uint32_t vt = (obj >= 0x40000000u && obj < 0x7FFF0000u) ? REX_LOAD_U32(obj) : 0;
  rex_monitor::ReportGuestWidget(obj, vt, 0, 0, 0, 0, false, false, "InFocusExit");
  rex_monitor::RecordGuestInputCall("sub_821C24C8_InFocusExit", 0x821C24C8, obj, ctx.r4.u32,
                                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                    ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                    base, obj, 0, false);
  __imp__sub_821C24C8(ctx, base);
}

DECLARE_REX_FUNC(sub_821F0AA8);
extern "C" REX_FUNC(sub_821F0AA8) {
  REX_FUNC_PROLOGUE();
  uint32_t obj = ctx.r3.u32;
  uint32_t vt = (obj >= 0x40000000u && obj < 0x7FFF0000u) ? REX_LOAD_U32(obj) : 0;
  rex_monitor::ReportGuestWidget(obj, vt, 0, 0, 0, 0, false, false, "LevelSelectLayout");
  rex_monitor::RecordGuestInputCall("sub_821F0AA8_LevelSelectLayout", 0x821F0AA8, obj, ctx.r4.u32,
                                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                                    ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64,
                                    base, ctx.r4.u32, 0, false);
  __imp__sub_821F0AA8(ctx, base);
}

DECLARE_REX_FUNC(sub_8241C5F8);
extern "C" REX_FUNC(sub_8241C5F8) {
  REX_FUNC_PROLOGUE();
  uint32_t obj = ctx.r3.u32;
  __imp__sub_8241C5F8(ctx, base);
  uint32_t ret = ctx.r3.u32;
  if (ret >= 0x40000000u && ret < 0x7FFF0000u) {
    uint32_t vt = REX_LOAD_U32(ret);
    rex_monitor::ReportGuestWidget(ret, vt, 0, 0, 0, 0, false, false, "UILayoutContainer");
  }
}

// ---------------------------------------------------------------------------
// Gamepad polling & input injection (0x8260D410)
// ---------------------------------------------------------------------------
DECLARE_REX_FUNC(sub_8260D410);
static std::atomic<int> g_wheel_delta{0};

#ifdef _WIN32
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
    auto* p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (p) {
      int delta = GET_WHEEL_DELTA_WPARAM(p->mouseData);
      g_wheel_delta.fetch_add(delta, std::memory_order_relaxed);
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static std::thread g_mouse_hook_thread([] {
  HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (hook) {
    UnhookWindowsHookEx(hook);
  }
});
#endif

extern "C" REX_FUNC(sub_8260D410) {
  REX_FUNC_PROLOGUE();
  uint32_t user_index = ctx.r3.u32;
  uint32_t state_ptr = ctx.r4.u32;
  __imp__sub_8260D410(ctx, base);

  if (state_ptr && user_index == 0) {
    static uint32_t s_virtual_packet = 1000;
    static uint16_t s_prev_written_buttons = 0;
    static uint8_t s_prev_written_lt = 0;
    static uint8_t s_prev_written_rt = 0;
    static int16_t s_prev_written_lx = 0;
    static int16_t s_prev_written_ly = 0;
    static int16_t s_prev_written_rx = 0;
    static int16_t s_prev_written_ry = 0;
    static uint16_t s_prev_any_dpad = 0;

    uint16_t combined_buttons = 0;
    uint8_t combined_lt = 0;
    uint8_t combined_rt = 0;
    int16_t combined_lx = 0;
    int16_t combined_ly = 0;
    int16_t combined_rx = 0;
    int16_t combined_ry = 0;

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool in_gameplay = rex_monitor::IsInGameplay() &&
                       (rex_monitor::GetCurrentScreenId() == SCREEN_GAMEPLAY);
    if (g_hud_flash_ticks.load(std::memory_order_relaxed) > 0) {
      g_hud_flash_ticks.fetch_sub(1, std::memory_order_relaxed);
    }

    if (g_test_replay_mode.load(std::memory_order_relaxed) &&
        !g_test_replay_started.load(std::memory_order_relaxed)) {
      static uint64_t s_title_ready_since_ms = 0;
      RexScreenWatcherStatus sw{};
      std::vector<RexCandidateButton> cands;
      rex_monitor::GetScreenWatcherState(sw, cands);
      if (sw.screen_id == SCREEN_TITLE && !cands.empty() && sw.motion_pct < 15.0f) {
        if (s_title_ready_since_ms == 0) {
          s_title_ready_since_ms = uint64_t(now_ms);
        } else if (uint64_t(now_ms) - s_title_ready_since_ms >= 400ull) {
          if (!g_test_replay_started.exchange(true, std::memory_order_relaxed)) {
            std::filesystem::path rpath = ResolveDefaultReplayPath();
            printf("[TEST-REPLAY] Title Screen detected (cand=%zu) -> starting replay: %s\n",
                   cands.size(), rpath.string().c_str());
            fflush(stdout);
            if (!StartInputReplay(rpath)) {
              TriggerTestReplayExit("replay file load failed", 1);
            }
          }
        }
      } else {
        s_title_ready_since_ms = 0;
      }
    }

    InputSample replay_sample{};
    if (TryStepInputReplay(replay_sample, uint64_t(now_ms))) {
      combined_buttons = replay_sample.buttons;
      combined_lt = replay_sample.lt;
      combined_rt = replay_sample.rt;
      combined_lx = replay_sample.lx;
      combined_ly = replay_sample.ly;
      combined_rx = replay_sample.rx;
      combined_ry = replay_sample.ry;
      g_mouse_direct_active.store(replay_sample.direct_active != 0, std::memory_order_relaxed);
      if (replay_sample.direct_active != 0) {
        g_mouse_target_angle.store(replay_sample.direct_angle, std::memory_order_relaxed);
      }
      g_wheel_delta.store(0, std::memory_order_relaxed);
      g_touch_dragging.store(false, std::memory_order_relaxed);
    } else {
      bool menu_open = g_menu_open.load(std::memory_order_relaxed);
#ifdef _WIN32
      HWND fg = GetForegroundWindow();
      DWORD proc_id = 0;
      if (fg) GetWindowThreadProcessId(fg, &proc_id);
      bool is_game_active = (proc_id == GetCurrentProcessId());
#else
      bool is_game_active = true;
#endif

      uint32_t existing_packet = 0;
      uint16_t existing_buttons = 0;
      uint8_t existing_lt = 0, existing_rt = 0;
      int16_t existing_lx = 0, existing_ly = 0, existing_rx = 0, existing_ry = 0;
      if (ctx.r3.u32 == 0) {
        existing_packet = REX_LOAD_U32(state_ptr);
        existing_buttons = REX_LOAD_U16(state_ptr + 4);
        existing_lt = REX_LOAD_U8(state_ptr + 6);
        existing_rt = REX_LOAD_U8(state_ptr + 7);
        existing_lx = static_cast<int16_t>(REX_LOAD_U16(state_ptr + 8));
        existing_ly = static_cast<int16_t>(REX_LOAD_U16(state_ptr + 10));
        existing_rx = static_cast<int16_t>(REX_LOAD_U16(state_ptr + 12));
        existing_ry = static_cast<int16_t>(REX_LOAD_U16(state_ptr + 14));
      }

      static bool s_controller_active = false;
      static bool s_manual_aim_override = false;
#ifdef _WIN32
      static POINT s_last_screen_cursor = {};
#endif
      static bool s_has_last_screen_cursor = false;
      struct ButtonHoldLatch {
        int ticks = 0;
        int64_t until_ms = 0;
        uint16_t mask = 0;

        void Trigger(uint16_t m, int64_t now_ms, int64_t duration_ms = 120) {
          mask = m;
          ticks = 3;
          const int64_t deadline = now_ms + duration_ms;
          if (deadline > until_ms) until_ms = deadline;
        }

        uint16_t Step(int64_t now_ms, bool active) {
          const bool tick_active = (ticks > 0);
          if (ticks > 0) --ticks;
          const bool time_active = (now_ms < until_ms);
          if (!tick_active && !time_active) {
            mask = 0;
            return 0;
          }
          return active ? mask : 0;
        }
      };
      static ButtonHoldLatch s_primary_latch;
      static ButtonHoldLatch s_secondary_latch;
      static ButtonHoldLatch s_start_latch;
      static ButtonHoldLatch s_mmb_latch;
      static uint16_t s_pulse_mask = 0;
      static int s_pulse_on = 0;
      static int s_menu_accum_x = 0;
      static int s_menu_accum_y = 0;
      static int64_t s_last_flick_ms = 0;
      static int64_t s_hover_stay_since_ms = 0;
      static float s_prev_hover_x = 0.0f, s_prev_hover_y = 0.0f;
      static uint16_t s_gp_nudge_mask = 0;
      static int s_gp_nudge_ticks = 0;
      static bool s_prev_primary_down = false;
      static bool s_prev_secondary_down = false;
      static float s_touch_start_u = 0.5f, s_touch_start_v = 0.5f;
      static bool s_touch_swiped = false;
      static bool s_touch_was_aim = false;
      static bool s_prev_menu_open = false;
      static int s_prev_input_style = -1;
      static uint16_t s_prev_kb_action_mask = 0;
      static uint16_t s_prev_pad_action_mask = 0;
      static bool s_ignore_esc_until_release = false;
      static float s_estimated_turret_angle = 0.0f;
      static uint64_t s_last_vis_seq = 0;
      static uint32_t s_last_est_brd = 0;
      static int64_t s_last_est_ms = 0;

      uint16_t mouse_buttons = 0;
      uint8_t mouse_lt = 0;
      int16_t mouse_stick_x = 0;
      bool direct_cursor_ok = false;
      int wheel = g_wheel_delta.exchange(0, std::memory_order_relaxed);

#ifdef _WIN32
      int swap_mode = g_opt_mouse_swap.load(std::memory_order_relaxed);
      bool swap_active = (swap_mode == kSwapAuto) ? (GetSystemMetrics(SM_SWAPBUTTON) != 0)
                                                  : (swap_mode == kSwapSwapped);
      int vk_primary = swap_active ? VK_RBUTTON : VK_LBUTTON;
      int vk_secondary = swap_active ? VK_LBUTTON : VK_RBUTTON;
#endif

      if (std::abs(existing_lx) > 8000) {
        s_controller_active = true;
        s_manual_aim_override = true;
      }

      if (is_game_active) {
        int input_style = g_opt_input_style.load(std::memory_order_relaxed);
        int aim_mode_now = g_opt_mouse_aim.load(std::memory_order_relaxed);
        int menu_mode_now = g_opt_menu_mode.load(std::memory_order_relaxed);
        bool cursor_enabled = (input_style != kInputControllerOnly);

        if ((s_prev_menu_open && !menu_open) || s_prev_input_style != input_style) {
#ifdef _WIN32
          if (s_prev_menu_open && !menu_open && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            s_ignore_esc_until_release = true;
          }
#endif
          if (cursor_enabled) {
            s_controller_active = false;
            s_manual_aim_override = false;
          }
          s_has_last_screen_cursor = false;
          s_touch_was_aim = false;
          s_menu_accum_x = 0;
          s_menu_accum_y = 0;
          s_prev_input_style = input_style;
        }
        s_prev_menu_open = menu_open;
        if (menu_open) {
          s_prev_kb_action_mask = 0;
          s_prev_pad_action_mask = 0;
        }

        int dx_screen = 0;
        int dy_screen = 0;
        bool had_prev = false;
        bool cursor_moved = false;
        float norm_x = 0.5f, norm_y = 0.5f;
        bool cursor_in_vp = false;
        float cur_angle = 0.0f;

        bool raw_primary_down = false;
        bool raw_secondary_down = false;
        bool primary_pressed = false;
        bool primary_released = false;
        bool secondary_pressed = false;
        bool touch_quick_tap = false;

        const uint16_t corner_latch =
            g_ios_corner_latch_mask.exchange(0, std::memory_order_acq_rel);
        const uint16_t corner_held =
            !menu_open ? g_ios_corner_held_mask.load(std::memory_order_relaxed) : 0;
        if (!menu_open && (corner_latch & 0x1000)) {
          s_primary_latch.Trigger(0x1000, now_ms);
          rex_monitor::NotifyMenuActionInput(0x1000, uint64_t(now_ms));
        }
        if (!menu_open && (corner_latch & 0x2000)) {
          s_secondary_latch.Trigger(0x2000, now_ms);
          rex_monitor::NotifyMenuActionInput(0x2000, uint64_t(now_ms));
        }
        if (!menu_open && (corner_latch & 0x0010)) {
          s_start_latch.Trigger(0x0010, now_ms);
          rex_monitor::NotifyMenuActionInput(0x0010, uint64_t(now_ms));
        }
        mouse_buttons |= corner_held;

#ifdef _WIN32
        POINT cur_pt{};
        if (GetCursorPos(&cur_pt)) {
          dx_screen = s_has_last_screen_cursor ? (cur_pt.x - s_last_screen_cursor.x) : 0;
          dy_screen = s_has_last_screen_cursor ? (cur_pt.y - s_last_screen_cursor.y) : 0;
          cursor_moved = !s_has_last_screen_cursor || (dx_screen != 0 || dy_screen != 0);
          had_prev = s_has_last_screen_cursor;
          s_last_screen_cursor = cur_pt;
          s_has_last_screen_cursor = true;

          if (cursor_moved && !menu_open && cursor_enabled) {
            s_controller_active = false;
            s_manual_aim_override = false;
          }

          RECT rc{};
          if (GetClientRect(fg, &rc) && !menu_open) {
            POINT client_pt = cur_pt;
            ScreenToClient(fg, &client_pt);
            float client_w = static_cast<float>(rc.right - rc.left);
            float client_h = static_cast<float>(rc.bottom - rc.top);
            bool in_window = (client_pt.x >= 0 && client_pt.x <= rc.right &&
                              client_pt.y >= 0 && client_pt.y <= rc.bottom);
            if (in_window && client_w > 100.0f && client_h > 100.0f) {
              ViewportRect vp = Compute16x9Viewport(client_w, client_h);
              float local_x = static_cast<float>(client_pt.x) - vp.x;
              float local_y = static_cast<float>(client_pt.y) - vp.y;
              norm_x = (vp.w > 1.0f) ? (local_x / vp.w) : -1.0f;
              norm_y = (vp.h > 1.0f) ? (local_y / vp.h) : -1.0f;
              cursor_in_vp = (norm_x >= 0.0f && norm_x <= 1.0f && norm_y >= 0.0f && norm_y <= 1.0f);
              cur_angle = ComputeBallisticAimAngle(norm_x, norm_y);
            }
          }
        }

        raw_primary_down = !menu_open && cursor_enabled && !s_controller_active &&
                           ((GetAsyncKeyState(vk_primary) & 0x8000) != 0);
        raw_secondary_down = !menu_open && cursor_enabled && !s_controller_active &&
                             ((GetAsyncKeyState(vk_secondary) & 0x8000) != 0);
        primary_pressed = raw_primary_down && !s_prev_primary_down;
        primary_released = !raw_primary_down && s_prev_primary_down;
        secondary_pressed = raw_secondary_down && !s_prev_secondary_down;
        s_prev_primary_down = raw_primary_down;
        s_prev_secondary_down = raw_secondary_down;
#elif REX_PLATFORM_IOS
        RexIosTouchSnapshot ts = rex_ios_consume_touch_for_guest();
        norm_x = ts.u;
        norm_y = ts.v;
        cursor_in_vp = ts.in_viewport;
        if (cursor_in_vp) {
          cur_angle = ComputeBallisticAimAngle(norm_x, norm_y);
          if ((ts.down || ts.pressed || ts.released) && !menu_open && cursor_enabled) {
            s_controller_active = false;
            s_manual_aim_override = false;
          }
        }
        raw_primary_down = !menu_open && cursor_enabled && !s_controller_active && ts.down;
        primary_pressed = !menu_open && cursor_enabled && !s_controller_active && ts.pressed;
        primary_released = !menu_open && cursor_enabled && !s_controller_active && ts.released;
        touch_quick_tap = !menu_open && cursor_enabled && !s_controller_active && ts.quick_tap;
        s_prev_primary_down = raw_primary_down;
#endif

        bool slingshot_active =
            (aim_mode_now == kAimTouchSlingshot) || (input_style == kInputMobileTouch);
        if (cursor_in_vp && !menu_open) {
          const bool in_hud_toggle =
              PointInMobileExcludeRect(g_ios_hud_toggle_rect, norm_x, norm_y) ||
              (slingshot_active && norm_x < 0.14f && norm_y < 0.12f);
          const bool in_corner_strip =
              PointInMobileExcludeRect(g_ios_corner_strip_rect, norm_x, norm_y);
          if (in_hud_toggle || in_corner_strip) {
            raw_primary_down = false;
            primary_pressed = false;
            primary_released = false;
            touch_quick_tap = false;
            cursor_in_vp = false;
          }
        }

        static int64_t s_last_pointer_activity_ms = 0;
        if (cursor_enabled && !menu_open && !s_controller_active &&
            (cursor_moved || raw_primary_down || primary_pressed || primary_released ||
             raw_secondary_down || wheel != 0)) {
          s_last_pointer_activity_ms = now_ms;
        }
        const bool show_colliders =
            (g_opt_ui_colliders.load(std::memory_order_relaxed) == kCollidersOn);
        const bool sw_needed_gameplay =
            show_colliders ||
            (cursor_enabled && !s_controller_active && !menu_open &&
             (aim_mode_now == kAimStickServo || aim_mode_now == kAimMouseDelta) &&
             (now_ms - s_last_pointer_activity_ms) < 500);
        const bool sw_needed_menu =
            show_colliders || g_test_replay_mode.load(std::memory_order_relaxed) ||
            (cursor_enabled && !s_controller_active && !menu_open);
        rex::ui::Presenter::SetScreenWatcherPaused(
            in_gameplay ? !sw_needed_gameplay : !sw_needed_menu);

        g_hud_cursor_nx.store(norm_x, std::memory_order_relaxed);
        g_hud_cursor_ny.store(norm_y, std::memory_order_relaxed);
        g_hud_cursor_valid.store(cursor_in_vp && !menu_open, std::memory_order_relaxed);
        if (cursor_in_vp && !menu_open) {
          rex_monitor::UpdateCursorSample(norm_x, norm_y);
        }

        bool touch_dragging_now = false;
        if (cursor_enabled && cursor_in_vp && !menu_open && !s_controller_active) {
          if (in_gameplay) {
            int gz = HitTestGameplayZones(norm_x, norm_y);
            bool cursor_over_hud_btn = (gz == 0 || gz == 1);
            g_telemetry_hover_id.store(gz >= 0 ? kGameplayZones[gz].id : "none",
                                       std::memory_order_relaxed);

            bool ball_in_flight = false;
            uint32_t brd = g_last_board_obj.load(std::memory_order_relaxed);
            if (brd >= 0x40000000u && brd < 0x7FFF0000u) {
              uint32_t b_beg = REX_LOAD_U32(brd + 132);
              uint32_t b_end = REX_LOAD_U32(brd + 136);
              ball_in_flight = (b_beg >= 0x40000000u && b_end > b_beg);
            }
            if (brd != s_last_est_brd) {
              s_last_est_brd = brd;
              s_estimated_turret_angle = 0.0f;
              s_last_est_ms = now_ms;
            }
            const float est_dt_sec =
                (s_last_est_ms > 0 && now_ms > s_last_est_ms)
                    ? std::clamp(float(now_ms - s_last_est_ms) * 0.001f, 0.0f, 0.05f)
                    : (1.0f / 60.0f);
            s_last_est_ms = now_ms;

            rex::ui::Presenter::VisualTurretSample vis_sample{};
            if (rex::ui::Presenter::GetLatestVisualTurretSample(vis_sample, s_last_vis_seq) &&
                vis_sample.dot_count >= 4) {
              s_last_vis_seq = vis_sample.sequence;
              s_estimated_turret_angle = vis_sample.angle_rad;
            } else {
              const int64_t aim_age_ms =
                  now_ms - g_last_aim_update_ms.load(std::memory_order_relaxed);
              if (aim_age_ms >= 0 && aim_age_ms <= 50) {
                s_estimated_turret_angle =
                    g_real_guest_turret_angle.load(std::memory_order_relaxed);
              } else if (s_prev_written_lx != 0) {
                const float abs_lx = float(std::abs(int(s_prev_written_lx)));
                float eff = (std::max)(0.0f, (abs_lx - 7849.0f) / (32767.0f - 7849.0f));
                if (s_prev_written_lx < 0) eff = -eff;
                if (s_prev_written_lt > 128) eff *= 0.15f;
                constexpr float kStickRateRadPerSec = 1.65f;
                s_estimated_turret_angle = std::clamp(
                    s_estimated_turret_angle + eff * kStickRateRadPerSec * est_dt_sec, -1.28f,
                    1.28f);
              }
            }

            if (ball_in_flight && raw_primary_down && gz == 2) {
              mouse_buttons |= 0x8000;  // Y fast-forward while holding playfield
              g_hud_flash_zone.store(2, std::memory_order_relaxed);
              g_hud_flash_ticks.store(2, std::memory_order_relaxed);
            }

            if (!cursor_over_hud_btn || s_touch_was_aim) {
              if (aim_mode_now == kAimTouchSlingshot ||
                  (aim_mode_now == kAimGuestTurretLock && slingshot_active)) {
                if (raw_primary_down) {
                  g_mouse_target_angle.store(cur_angle, std::memory_order_relaxed);
                  direct_cursor_ok = true;
                  ComputeDeadzoneCompensatedStick(cur_angle - s_estimated_turret_angle,
                                                  mouse_stick_x, mouse_lt);
                }
              } else if (aim_mode_now == kAimGuestTurretLock) {
                if (wheel != 0) {
                  mouse_lt = 255;
                  mouse_stick_x = (wheel > 0) ? 16000 : -16000;
                  s_manual_aim_override = true;
                } else if (!s_manual_aim_override) {
                  g_mouse_target_angle.store(cur_angle, std::memory_order_relaxed);
                  direct_cursor_ok = true;
                  ComputeDeadzoneCompensatedStick(cur_angle - s_estimated_turret_angle,
                                                  mouse_stick_x, mouse_lt);
                }
              } else if (aim_mode_now == kAimStickServo) {
                ComputeDeadzoneCompensatedStick(cur_angle - s_estimated_turret_angle,
                                                mouse_stick_x, mouse_lt);
              } else {
                if (wheel != 0) {
                  s_gp_nudge_mask = (wheel > 0) ? 0x0008 : 0x0004;
                  s_gp_nudge_ticks = 2;
                }
                if (had_prev && dx_screen != 0) {
                  float s = (dx_screen > 0) ? 1.0f : -1.0f;
                  int abs_dx = std::abs(dx_screen);
                  mouse_lt = (abs_dx <= 3) ? 255 : 0;
                  float mag = (abs_dx <= 3) ? (9000.0f + float(abs_dx) * 4000.0f)
                                            : (10000.0f + float(abs_dx) * 1800.0f);
                  mouse_stick_x = static_cast<int16_t>(std::clamp(s * mag, -32768.0f, 32767.0f));
                }
              }
            }

            if (slingshot_active) {
              touch_dragging_now = raw_primary_down;
              if (primary_pressed) {
                s_touch_was_aim = !cursor_over_hud_btn;
              }
              if (touch_quick_tap) {
                s_primary_latch.Trigger(0x1000, now_ms);
                s_touch_was_aim = false;
              } else if (primary_released) {
                if (s_touch_was_aim || gz == 2) {
                  s_primary_latch.Trigger(0x1000, now_ms);
                } else if (cursor_over_hud_btn && gz >= 0) {
                  s_primary_latch.Trigger(kGameplayZones[gz].button_mask, now_ms);
                  if (gz == 1) rex_monitor::NotifyMenuActionInput(0x0010, uint64_t(now_ms));
                  g_hud_flash_zone.store(gz, std::memory_order_relaxed);
                  g_hud_flash_ticks.store(8, std::memory_order_relaxed);
                }
                s_touch_was_aim = false;
              }
            } else if (primary_pressed && gz >= 0 && (!ball_in_flight || cursor_over_hud_btn)) {
              s_primary_latch.Trigger(kGameplayZones[gz].button_mask, now_ms);
              if (gz == 1) rex_monitor::NotifyMenuActionInput(0x0010, uint64_t(now_ms));
              g_hud_flash_zone.store(gz, std::memory_order_relaxed);
              g_hud_flash_ticks.store(8, std::memory_order_relaxed);
            }
          } else {
            s_estimated_turret_angle = 0.0f;
            s_last_est_brd = 0;
            s_last_est_ms = now_ms;
            // Engine-Monitored Menu Mode: Zero hardcoded screen assumptions.
            if (std::hypot(norm_x - s_prev_hover_x, norm_y - s_prev_hover_y) > 0.01f) {
              s_hover_stay_since_ms = now_ms;
              s_prev_hover_x = norm_x;
              s_prev_hover_y = norm_y;
            }

            bool click_trigger = false;
            if (slingshot_active) {
              touch_dragging_now = raw_primary_down;
              if (primary_pressed) {
                s_touch_start_u = norm_x;
                s_touch_start_v = norm_y;
                s_touch_swiped = false;
              }
              if (raw_primary_down && !s_touch_swiped) {
                float du = norm_x - s_touch_start_u;
                float dv = norm_y - s_touch_start_v;
                if (std::fabs(du) > 0.08f && std::fabs(du) >= std::fabs(dv)) {
                  s_touch_swiped = true;
                  s_pulse_mask = (du < 0.0f) ? 0x0008 : 0x0004;
                  s_pulse_on = 3;
                } else if (std::fabs(dv) > 0.08f) {
                  s_touch_swiped = true;
                  s_pulse_mask = (dv < 0.0f) ? 0x0002 : 0x0001;
                  s_pulse_on = 3;
                }
              }
              if (touch_quick_tap) {
                s_touch_swiped = false;
                click_trigger = true;
              } else if (primary_released && !s_touch_swiped) {
                click_trigger = true;
              }
            } else {
              click_trigger = primary_pressed;
            }

            if (menu_mode_now == kMenuWheelFlickClick) {
              if (wheel != 0) {
                s_pulse_mask = (wheel > 0) ? 0x0001 : 0x0002;
                s_pulse_on = 3;
              } else if (had_prev) {
                s_menu_accum_x += dx_screen;
                s_menu_accum_y += dy_screen;
                if ((now_ms - s_last_flick_ms) >= 140) {
                  uint16_t fm = 0;
                  if (std::abs(s_menu_accum_x) >= 90 &&
                      std::abs(s_menu_accum_x) >= std::abs(s_menu_accum_y)) {
                    fm = (s_menu_accum_x > 0) ? 0x0008 : 0x0004;
                  } else if (std::abs(s_menu_accum_y) >= 90) {
                    fm = (s_menu_accum_y > 0) ? 0x0002 : 0x0001;
                  }
                  if (fm) {
                    s_pulse_mask = fm;
                    s_pulse_on = 3;
                    s_last_flick_ms = now_ms;
                    s_menu_accum_x = 0;
                    s_menu_accum_y = 0;
                  }
                }
              }
            }
            bool allow_hover_servo = false;
            if (menu_mode_now != kMenuWheelFlickClick && !slingshot_active) {
              if (menu_mode_now == kMenuDirectHover) {
                allow_hover_servo = true;
              } else if (menu_mode_now == kMenuHoverDebounced) {
                allow_hover_servo = (now_ms - s_hover_stay_since_ms) >= 90;
              }
            }
            const bool is_touch_tap =
#if REX_PLATFORM_IOS
                touch_quick_tap;
#else
                slingshot_active;
#endif
            RexMonitorServoOutput srv = rex_monitor::StepMenuFocusServo(
                norm_x, norm_y, allow_hover_servo, click_trigger, is_touch_tap,
                uint64_t(now_ms));
            g_telemetry_hover_id.store(
                srv.has_target ? (srv.aligned ? "AlignedBtn" : "MenuBtn")
                               : (menu_mode_now == kMenuWheelFlickClick ? "Wheel/Flick" : "None"),
                std::memory_order_relaxed);
            if (srv.buttons_or_mask & 0x000F) {
              s_pulse_mask = srv.buttons_or_mask & 0x000F;
              s_pulse_on = 3;  // 3-frame pulse per 0.1s tick
            }
            if (srv.buttons_or_mask & (0x1000 | 0x8000 | 0x0200)) {
              s_pulse_on = 0;
              s_primary_latch.Trigger(srv.buttons_or_mask & (0x1000 | 0x8000 | 0x0200), now_ms);
            }
            if (srv.buttons_or_mask & 0x2000) {
              s_pulse_on = 0;
              s_secondary_latch.Trigger(0x2000, now_ms);
            }
          }

          // Right-click triggers Master Power (X) ONLY during active gameplay; never B (Back) in menus.
          if (secondary_pressed && in_gameplay) {
            s_secondary_latch.Trigger(0x4000, now_ms);
          }
#ifdef _WIN32
          if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) s_mmb_latch.Trigger(0x8000, now_ms);
          if (in_gameplay && (GetAsyncKeyState(VK_XBUTTON1) & 0x8000)) mouse_buttons |= 0x2000;
#endif
        } else {
          g_telemetry_hover_id.store("none", std::memory_order_relaxed);
        }

        g_touch_dragging.store(touch_dragging_now, std::memory_order_relaxed);
        if (s_gp_nudge_ticks > 0) {
          --s_gp_nudge_ticks;
          if (!menu_open) mouse_buttons |= s_gp_nudge_mask;
        }
        if (s_pulse_on > 0) {
          --s_pulse_on;
          if (!menu_open) mouse_buttons |= s_pulse_mask;
        }
        mouse_buttons |= s_primary_latch.Step(now_ms, !menu_open);
        mouse_buttons |= s_secondary_latch.Step(now_ms, !menu_open);
        mouse_buttons |= s_start_latch.Step(now_ms, !menu_open);
        if (s_mmb_latch.Step(now_ms, !menu_open)) {
          mouse_buttons |= in_gameplay ? (0x8000 | 0x2000) : 0x8000;
        }

        if (!menu_open) {
          uint16_t kb_action_mask = 0;
#ifdef _WIN32
          if ((GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT) & 0x8000)) {
            mouse_buttons |= 0x0004;
            mouse_stick_x = -30000;
            s_controller_active = false;
            s_manual_aim_override = true;
            direct_cursor_ok = false;
          }
          if ((GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000)) {
            mouse_buttons |= 0x0008;
            mouse_stick_x = 30000;
            s_controller_active = false;
            s_manual_aim_override = true;
            direct_cursor_ok = false;
          }
          if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP) & 0x8000)) {
            mouse_buttons |= 0x0001;
          }
          if ((GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN) & 0x8000)) {
            mouse_buttons |= 0x0002;
          }
          const bool esc_down_raw = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
          if (!esc_down_raw) s_ignore_esc_until_release = false;
          const bool esc_active = esc_down_raw && !s_ignore_esc_until_release;

          if ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000)) {
            mouse_buttons |= 0x1000;
            kb_action_mask |= 0x1000;
          }
          if ((GetAsyncKeyState('E') & 0x8000) || (GetAsyncKeyState('Q') & 0x8000)) {
            mouse_buttons |= 0x4000;
          }
          if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
            mouse_lt = 255;
          }
          if (GetAsyncKeyState('F') & 0x8000) {
            mouse_buttons |= 0x8000;
            kb_action_mask |= 0x8000;
          }
          if (esc_active) {
            const RexScreenId sw_cur = rex_monitor::GetCurrentScreenId();
            const uint16_t esc_bit =
                (in_gameplay || sw_cur == SCREEN_GAMEPLAY || sw_cur == SCREEN_PAUSE_MENU ||
                 sw_cur == SCREEN_TITLE || sw_cur == SCREEN_BOOT)
                    ? 0x0010
                    : 0x2000;
            mouse_buttons |= esc_bit;
            kb_action_mask |= esc_bit;
          }
          if ((GetAsyncKeyState(VK_BACK) & 0x8000) || (GetAsyncKeyState(VK_TAB) & 0x8000)) {
            mouse_buttons |= 0x2000;
            kb_action_mask |= 0x2000;
          }
#endif

          const uint16_t kb_edge =
              kb_action_mask & static_cast<uint16_t>(~s_prev_kb_action_mask);
          s_prev_kb_action_mask = kb_action_mask;
          const uint16_t pad_action = existing_buttons & 0x3010;
          const uint16_t pad_edge =
              pad_action & static_cast<uint16_t>(~s_prev_pad_action_mask);
          s_prev_pad_action_mask = pad_action;
          if ((kb_edge | pad_edge) != 0) {
            rex_monitor::NotifyMenuActionInput(kb_edge | pad_edge, uint64_t(now_ms));
          }
        }
      } else {
        rex::ui::Presenter::SetScreenWatcherPaused(
            (g_opt_ui_colliders.load(std::memory_order_relaxed) != kCollidersOn) &&
            !g_test_replay_mode.load(std::memory_order_relaxed));
        g_hud_cursor_valid.store(false, std::memory_order_relaxed);
        g_touch_dragging.store(false, std::memory_order_relaxed);
        s_has_last_screen_cursor = false;
        s_prev_primary_down = false;
        s_prev_secondary_down = false;
        s_prev_kb_action_mask = 0;
        s_prev_pad_action_mask = 0;
        s_pulse_on = 0;
        g_telemetry_hover_id.store("none", std::memory_order_relaxed);
      }

      g_mouse_direct_active.store(direct_cursor_ok, std::memory_order_relaxed);
      combined_buttons = existing_buttons | mouse_buttons;
      combined_lt = (std::max)(existing_lt, mouse_lt);
      combined_rt = existing_rt;
      combined_lx = (!s_controller_active && mouse_stick_x != 0) ? mouse_stick_x : existing_lx;
      combined_ly = existing_ly;
      if (in_gameplay) {
        if (mouse_buttons & 0x0001) combined_ly = 30000;
        else if (mouse_buttons & 0x0002) combined_ly = -30000;
      }
      combined_rx = existing_rx;
      combined_ry = existing_ry;

      if (existing_packet > s_virtual_packet) s_virtual_packet = existing_packet;
      if (g_rec_active.load(std::memory_order_relaxed)) {
        InputSample rec_sample{};
        rec_sample.buttons = combined_buttons;
        rec_sample.lt = combined_lt;
        rec_sample.rt = combined_rt;
        rec_sample.lx = combined_lx;
        rec_sample.ly = combined_ly;
        rec_sample.rx = combined_rx;
        rec_sample.ry = combined_ry;
        rec_sample.direct_active = direct_cursor_ok ? 1 : 0;
        rec_sample.direct_angle = direct_cursor_ok ? g_mouse_target_angle.load(std::memory_order_relaxed) : 0.0f;
        RecordInputSample(rec_sample);
      }
    }

    uint16_t any_dpad = combined_buttons & 0x000F;
    if ((any_dpad & static_cast<uint16_t>(~s_prev_any_dpad)) != 0) {
      rex_monitor::NotifyHostDpadSent(any_dpad);
    }
    s_prev_any_dpad = any_dpad;

    if (combined_buttons != s_prev_written_buttons || combined_lt != s_prev_written_lt ||
        combined_rt != s_prev_written_rt || combined_lx != s_prev_written_lx ||
        combined_ly != s_prev_written_ly || combined_rx != s_prev_written_rx ||
        combined_ry != s_prev_written_ry) {
      ++s_virtual_packet;
    }
    s_prev_written_buttons = combined_buttons;
    s_prev_written_lt = combined_lt;
    s_prev_written_rt = combined_rt;
    s_prev_written_lx = combined_lx;
    s_prev_written_ly = combined_ly;
    s_prev_written_rx = combined_rx;
    s_prev_written_ry = combined_ry;

    REX_STORE_U32(state_ptr, s_virtual_packet);
    REX_STORE_U16(state_ptr + 4, combined_buttons);
    REX_STORE_U8(state_ptr + 6, combined_lt);
    REX_STORE_U8(state_ptr + 7, combined_rt);
    REX_STORE_U16(state_ptr + 8, static_cast<uint16_t>(combined_lx));
    REX_STORE_U16(state_ptr + 10, static_cast<uint16_t>(combined_ly));
    REX_STORE_U16(state_ptr + 12, static_cast<uint16_t>(combined_rx));
    REX_STORE_U16(state_ptr + 14, static_cast<uint16_t>(combined_ry));

    g_telemetry_stick_lx.store(combined_lx, std::memory_order_relaxed);
    g_telemetry_buttons.store(combined_buttons, std::memory_order_relaxed);
    ctx.r3.u64 = 0;
  }
}

REX_DEFINE_APP(peggle2, Peggle2App::Create)
