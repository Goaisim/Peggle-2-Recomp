#include "rex_engine_monitor.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "ios_bridge.h"
#endif
#endif

#include <rex/ui/presenter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
extern "C" {
struct RexGpuUiMonitorSnapshot;
uint32_t RexGpu_GetUiMonitorSnapshot(RexGpuUiMonitorSnapshot* out_snapshot);
void RexGpu_NotifyDpadInput(uint16_t dpad_mask, uint64_t timestamp_ms);
}
#endif

namespace rex_monitor {

namespace {

using FnGetSnapshot = uint32_t(__cdecl*)(RexGpuUiMonitorSnapshotMirror*);
using FnNotifyDpad = void(__cdecl*)(uint16_t, uint64_t);

std::mutex g_mutex;
FILE* g_log_file = nullptr;
FnGetSnapshot g_fn_get_snapshot = nullptr;
FnNotifyDpad g_fn_notify_dpad = nullptr;
bool g_gpu_procs_resolved = false;

bool g_has_synthetic_snap = false;
RexGpuUiMonitorSnapshotMirror g_synthetic_snap{};
RexGpuUiMonitorSnapshotMirror g_last_gpu_snap{};
RexMonitorServoOutput g_last_servo{};

std::unordered_map<uint32_t, RexGuestWidgetBox> g_guest_widgets;
std::unordered_map<std::string, uint64_t> g_last_log_ms_by_key;
std::string g_last_gpu_sig;
uint64_t g_last_dpad_log_ms = 0;
uint16_t g_pending_post_dpad_mask = 0;

std::atomic<uint64_t> g_last_turret_tick_ms{0};
std::atomic<uint64_t> g_gameplay_tick_count{0};
std::atomic<uint32_t> g_last_turret_ptr{0};
std::atomic<float> g_last_turret_angle_deg{0.0f};
float g_logged_turret_angle_deg = 9999.0f;

// 10 Hz (0.1s) Screen Watcher frame & click-locked UI state machine
rex::ui::Presenter::ScreenWatcherFrame g_sw_prev_frame;
rex::ui::Presenter::ScreenWatcherFrame g_sw_curr_frame;
bool g_has_synthetic_frame = false;
std::vector<RexCandidateButton> g_sw_candidates;
RexScreenWatcherStatus g_sw_status{};

std::atomic<uint8_t> g_atomic_screen_id{SCREEN_TITLE};
RexScreenId s_current_screen = SCREEN_TITLE;
RexScreenId s_prev_screen = SCREEN_MAIN_MENU;
RexScreenId s_help_parent_screen = SCREEN_MAIN_MENU;
RexScreenId s_modal_yes_screen = SCREEN_MAIN_MENU;
bool s_modal_is_yesno = false;
bool s_transition_pending = false;
uint64_t s_transition_settle_ms = 0;
RexScreenId s_pending_source_screen = SCREEN_TITLE;
char s_pending_target_name[32] = {};
float s_pending_target_gold = 0.0f;
uint16_t s_pending_action_mask = 0;
uint64_t s_paused_turret_tick_count = 0;
uint64_t s_last_gameplay_start_ms = 0;
uint64_t s_last_title_exit_ms = 0;
uint64_t g_last_step_now_ms = 0;
uint8_t s_title_heal_streak = 0;
uint64_t s_title_heal_last_seq = ~0ull;
uint64_t s_title_heal_first_ms = 0;

struct PageFingerprint {
  bool has_bottom_bar = false;
  bool has_teal_strip = false;
  bool has_back_pill = false;
  bool is_bottom_bar_dimmed = false;
  bool has_costume_btn = false;
  bool has_green_p2_pillar = false;
  bool has_adventure_sign = false;
  bool has_level_board = false;
  bool operator==(const PageFingerprint&) const = default;
};
bool s_page_watch_active = false;
bool s_page_watch_used_fallback = false;
uint64_t s_page_watch_started_ms = 0;
uint64_t s_page_watch_until_ms = 0;
RexScreenId s_page_watch_source = SCREEN_TITLE;
PageFingerprint s_fp_baseline{};
PageFingerprint s_fp_pending{};
uint8_t s_fp_change_streak = 0;
uint64_t s_fp_last_seq = ~0ull;
int8_t s_lobby_row = 0;
int8_t s_lobby_col = 0;
int8_t s_settings_row = 0;
int8_t s_pause_row = 0;
uint64_t s_built_seq = ~0ull;
RexScreenId s_built_screen = SCREEN_UNKNOWN;
bool s_built_gameplay_ticking = false;
bool s_candidates_dirty = true;

// Closed-loop 0.1s steering state
uint64_t g_last_servo_dpad_ms = 0;
uint64_t g_last_carousel_scroll_ms = 0;
int8_t g_last_carousel_scroll_dir = 0;
int8_t g_carousel_hover_side = 0;       // -1 = PREV, 0 = none, +1 = NEXT
uint8_t g_carousel_pending_steps = 0;
int8_t g_carousel_pending_dir = 0;
uint64_t g_carousel_next_pulse_ms = 0;
uint64_t g_carousel_next_scroll_ms = 0;
int s_adv_world_idx = 0;
int8_t s_level_idx = 0;
int8_t s_master_row = 0;
int8_t s_master_col = 0;
RexScreenId g_prev_classified_screen = SCREEN_TITLE;
int8_t g_sw_stepped_row = 0;
int8_t g_sw_stepped_col = 0;
bool g_sw_has_stepped_focus = false;
uint64_t g_pending_click_until_ms = 0;
float g_pending_click_x = 0.0f;
float g_pending_click_y = 0.0f;

float g_sw_last_cursor_u = 0.5f;
float g_sw_last_cursor_v = 0.5f;
bool g_sw_has_cursor_pos = false;

bool g_sw_prev_pulse_valid = false;
bool g_sw_used_orthogonal = false;
uint8_t g_sw_last_pulse_axis = 0;  // 1 = horiz, 2 = vert
uint16_t g_sw_last_dpad_mask = 0;
uint64_t g_sw_last_dpad_time_ms = 0;
uint64_t g_sw_last_pulse_frame_seq = 0;
int8_t g_sw_pre_pulse_row = -1;
int8_t g_sw_pre_pulse_col = -1;
float g_sw_pre_pulse_hl_u = 0.0f;
float g_sw_pre_pulse_hl_v = 0.0f;
float g_sw_last_target_u = 0.0f;
float g_sw_last_target_v = 0.0f;

#if defined(__APPLE__) && TARGET_OS_IPHONE
bool g_ui_log_enabled = false;  // Disabled on iOS by default to avoid per-frame flash writes.
#else
bool g_ui_log_enabled = true;
#endif

bool g_audit_enabled = false;
FILE* g_audit_file = nullptr;
std::string g_audit_path = "replay_hitbox_audit.log";
uint64_t g_audit_last_seq = ~0ull;
RexScreenId g_audit_last_screen = SCREEN_UNKNOWN;
std::string g_audit_last_target;
uint64_t g_audit_start_ms = 0;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

static FILE* OpenStdFile(const char* path, const char* mode) {
#ifdef _WIN32
  FILE* f = nullptr;
  fopen_s(&f, path, mode);
  return f;
#else
  return std::fopen(path, mode);
#endif
}

void ResolveGpuProcs() {
  if (g_gpu_procs_resolved) return;
#ifdef _WIN32
  HMODULE mod = GetModuleHandleA("rexgpu-xenos.dll");
  if (!mod) mod = GetModuleHandleA("rexgpu_xenos.dll");
  if (mod) {
    g_fn_get_snapshot = reinterpret_cast<FnGetSnapshot>(
        GetProcAddress(mod, "RexGpu_GetUiMonitorSnapshot"));
    g_fn_notify_dpad = reinterpret_cast<FnNotifyDpad>(
        GetProcAddress(mod, "RexGpu_NotifyDpadInput"));
    if (g_fn_get_snapshot) {
      g_gpu_procs_resolved = true;
    }
  }
#else
  g_fn_get_snapshot = reinterpret_cast<FnGetSnapshot>(&RexGpu_GetUiMonitorSnapshot);
  g_fn_notify_dpad = &RexGpu_NotifyDpadInput;
  g_gpu_procs_resolved = true;
#endif
}

void EnsureLogOpenLocked() {
  if (!g_ui_log_enabled) return;
  if (!g_log_file) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    std::string log_path = rex_ios_get_documents_dir();
    log_path = log_path.empty() ? std::string("ui_monitor.log") : (log_path + "/ui_monitor.log");
    g_log_file = OpenStdFile(log_path.c_str(), "w");
#else
    g_log_file = OpenStdFile("ui_monitor.log", "w");
#endif
    if (g_log_file) {
      std::fprintf(g_log_file, "=== Peggle 2 10Hz ScreenWatcher & Turret Telemetry ===\n");
      std::fflush(g_log_file);
    }
  }
}

void WriteLogLocked(const char* category, const char* msg) {
  if (!g_ui_log_enabled) return;
  EnsureLogOpenLocked();
  if (!g_log_file) return;
  std::fprintf(g_log_file, "[%llu ms] [%s] %s\n",
               static_cast<unsigned long long>(NowMs()), category, msg);
  std::fflush(g_log_file);
}

const char* ScreenIdToName(RexScreenId id);

void WriteAuditLineLocked(const char* tag, uint64_t now_ms, uint16_t out_mask) {
  if (!g_audit_enabled) return;
  if (!g_audit_file) {
    g_audit_file = OpenStdFile(g_audit_path.c_str(), "w");
    if (!g_audit_file) return;
    std::fprintf(g_audit_file, "=== Peggle 2 Replay Hitbox Audit ===\n");
  }
  if (g_audit_start_ms == 0) g_audit_start_ms = now_ms;
  const double sec =
      (now_ms >= g_audit_start_ms) ? double(now_ms - g_audit_start_ms) / 1000.0 : 0.0;

  char hl_buf[80];
  if (g_sw_status.has_highlight && g_sw_status.hl_name[0]) {
    std::snprintf(hl_buf, sizeof(hl_buf), "%s (%.2f,%.2f)", g_sw_status.hl_name,
                  g_sw_status.hl_u, g_sw_status.hl_v);
  } else {
    std::snprintf(hl_buf, sizeof(hl_buf), "None (-1.00,-1.00)");
  }

  char mouse_buf[32];
  if (g_sw_has_cursor_pos) {
    std::snprintf(mouse_buf, sizeof(mouse_buf), "%.2f,%.2f", g_sw_last_cursor_u,
                  g_sw_last_cursor_v);
  } else {
    std::snprintf(mouse_buf, sizeof(mouse_buf), "-1.00,-1.00");
  }

  const char* tgt_name =
      (g_sw_status.has_target && g_sw_status.target_name[0]) ? g_sw_status.target_name : "None";
  const char* scr_name =
      g_sw_status.screen_name[0] ? g_sw_status.screen_name : ScreenIdToName(s_current_screen);

  std::string scores;
  for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
    const auto& c = g_sw_candidates[i];
    char entry[64];
    std::snprintf(entry, sizeof(entry), "%s%s:%.2f(g=%.2f)", i ? " " : "", c.name,
                  c.total_score, c.gold_frac);
    scores += entry;
  }
  if (scores.empty()) {
    scores = "none";
  }

  std::fprintf(g_audit_file,
               "[%7.3fs] [%-10s] Screen=%s (cnt=%zu,mot=%.1f%%,pend=%d) | Highlight=%s | "
               "Mouse=(%s) | Target=%s | Out=0x%04X | Scores=%s\n",
               sec, tag ? tag : "TICK", scr_name, g_sw_candidates.size(),
               g_sw_status.motion_pct, s_transition_pending ? 1 : 0, hl_buf, mouse_buf, tgt_name,
               static_cast<unsigned int>(out_mask), scores.c_str());
  std::fflush(g_audit_file);
}

void MaybeAuditStateLocked(uint64_t now_ms, uint16_t out_mask, bool track_target) {
  if (!g_audit_enabled) return;
  const uint64_t seq = g_sw_curr_frame.sequence;
  const std::string tgt =
      (g_sw_status.has_target && g_sw_status.target_name[0]) ? g_sw_status.target_name : "None";
  const char* tag = nullptr;
  if (s_current_screen != g_audit_last_screen) {
    tag = "TRANSITION";
  } else if ((out_mask & 0x000F) != 0) {
    tag = "DPAD";
  } else if ((out_mask & (0x1000 | 0x2000 | 0x0010 | 0x8000 | 0x0200)) != 0) {
    tag = "ACTION";
  } else if (seq != g_audit_last_seq) {
    tag = "TICK_0.1S";
  } else if (track_target && tgt != g_audit_last_target) {
    tag = "TARGET";
  }
  if (!tag) return;
  g_audit_last_screen = s_current_screen;
  g_audit_last_seq = seq;
  g_audit_last_target = tgt;
  WriteAuditLineLocked(tag, now_ms, out_mask);
}

bool PointInRect(float px, float py, float x0, float y0, float x1, float y1) {
  return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

float RectArea(float x0, float y0, float x1, float y1) {
  return std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0);
}

float IntersectionArea(float ax0, float ay0, float ax1, float ay1,
                       float bx0, float by0, float bx1, float by1) {
  const float ix0 = std::max(ax0, bx0);
  const float iy0 = std::max(ay0, by0);
  const float ix1 = std::min(ax1, bx1);
  const float iy1 = std::min(ay1, by1);
  return RectArea(ix0, iy0, ix1, iy1);
}

float OverlapMinRatio(const RexCandidateButton& a, float bx0, float by0, float bx1, float by1) {
  const float ia = IntersectionArea(a.x0, a.y0, a.x1, a.y1, bx0, by0, bx1, by1);
  const float ma = std::min(RectArea(a.x0, a.y0, a.x1, a.y1), RectArea(bx0, by0, bx1, by1));
  return (ma > 1e-6f) ? (ia / ma) : 0.0f;
}

bool IsValidButtonSize(float w, float h) {
  return w >= 0.04f && w <= 0.55f && h >= 0.04f && h <= 0.45f;
}

bool IsUiBlue(uint8_t r, uint8_t g, uint8_t b) {
  return b > 155 && int(b) > int(r) + 45 && g > 70;
}

bool IsButtonBlue(uint8_t r, uint8_t g, uint8_t b) {
  return b > 155 && int(b) > int(r) + 95 && int(b) >= int(g) - 15 && r < 85 && g > 70;
}

bool IsUiGold(uint8_t r, uint8_t g, uint8_t b) {
  return r > 200 && g > 125 && b < 75 && (int(r) + int(g)) > 350;
}

bool IsBottomTealBar(uint8_t r, uint8_t g, uint8_t b) {
  return int(g) >= int(r) + 15 && int(b) >= int(r) + 20 && int(b) >= int(g) - 15 && b >= 35 && b <= 185 && r <= 115;
}

bool IsRedBackColor(uint8_t r, uint8_t g, uint8_t b) {
  return r > 135 && int(r) > int(g) + 25 && int(r) > int(b) + 30;
}

bool IsModalBlue(uint8_t r, uint8_t g, uint8_t b) {
  return b > 140 && int(b) > int(r) + 35;
}

bool IsGreenSelectColor(uint8_t r, uint8_t g, uint8_t b) {
  return g > 95 && int(g) > int(r) + 25 && int(g) > int(b) + 20;
}

const char* ScreenIdToName(RexScreenId id) {
  switch (id) {
    case SCREEN_GAMEPLAY:           return "GAMEPLAY";
    case SCREEN_TITLE:              return "TITLE";
    case SCREEN_MAIN_MENU:          return "MAIN MENU";
    case SCREEN_ADVENTURE_CAROUSEL: return "ADVENTURE";
    case SCREEN_LEVEL_SELECT:       return "LEVEL SELECT";
    case SCREEN_MASTER_SELECT:      return "MASTER SELECT";
    case SCREEN_MODAL_OK_POPUP:     return "MODAL OK";
    case SCREEN_PAUSE_MENU:         return "PAUSE MENU";
    case SCREEN_HELP_OPTIONS:       return "HELP & OPTIONS";
    case SCREEN_MULTIPLAYER_MENU:   return "MULTIPLAYER";
    case SCREEN_SETTINGS_SUBMENU:   return "SETTINGS";
    case SCREEN_BOOT:               return "BOOT";
    case SCREEN_INFO_SUBMENU:       return "INFO SUBMENU";
    case SCREEN_MP_LOCAL_DUEL:      return "LOCAL DUEL";
    case SCREEN_MP_MODE_SELECT:     return "MP MODE SELECT";
    default:                        return "UNKNOWN";
  }
}

struct RegionColorStats {
  float gold_frac = 0.0f;
  float blue_frac = 0.0f;
  float btn_blue_frac = 0.0f;
  float modal_blue_frac = 0.0f;
  float ok_blue_frac = 0.0f;
  float teal_frac = 0.0f;
  float red_frac = 0.0f;
  float green_frac = 0.0f;
  float bright_white_frac = 0.0f;
  float master_hl_frac = 0.0f;
  float dark_navy_frac = 0.0f;
  float padlock_navy_frac = 0.0f;
  float avg_r = 0.0f;
  float avg_g = 0.0f;
  float avg_b = 0.0f;
  float avg_luma = 0.0f;
  float ui_frac() const { return gold_frac + blue_frac; }
  float btn_frac() const { return gold_frac + btn_blue_frac; }
};

RegionColorStats SampleRegionColorsLocked(float x0, float y0, float x1, float y1) {
  RegionColorStats st{};
  const uint32_t W = g_sw_curr_frame.width;
  const uint32_t H = g_sw_curr_frame.height;
  if (W == 0 || H == 0 || g_sw_curr_frame.pixels.size() < size_t(W) * H * 4) return st;

  const uint32_t px0 = std::clamp<uint32_t>(uint32_t(x0 * float(W)), 0, W - 1);
  const uint32_t py0 = std::clamp<uint32_t>(uint32_t(y0 * float(H)), 0, H - 1);
  const uint32_t px1 = std::clamp<uint32_t>(uint32_t(x1 * float(W) + 0.5f), px0 + 1, W);
  const uint32_t py1 = std::clamp<uint32_t>(uint32_t(y1 * float(H) + 0.5f), py0 + 1, H);
  const uint8_t* curr = g_sw_curr_frame.pixels.data();

  uint32_t total = 0, gold = 0, blue = 0, btn_blue = 0, modal_blue = 0, ok_blue = 0, teal = 0,
           red = 0, green = 0;
  uint32_t bright_white = 0, master_hl = 0, dark_navy = 0, padlock_navy = 0;
  uint64_t r_sum = 0, g_sum = 0, b_sum = 0, luma_sum = 0;
  for (uint32_t y = py0; y < py1; ++y) {
    for (uint32_t x = px0; x < px1; ++x) {
      const size_t off = (size_t(y) * W + x) * 4;
      const uint8_t r = curr[off + 0];
      const uint8_t g = curr[off + 1];
      const uint8_t b = curr[off + 2];
      const uint8_t luma = curr[off + 3];
      r_sum += r;
      g_sum += g;
      b_sum += b;
      luma_sum += luma;
      if (IsUiGold(r, g, b)) ++gold;
      else if (IsUiBlue(r, g, b)) ++blue;
      if (IsButtonBlue(r, g, b)) ++btn_blue;
      if (IsModalBlue(r, g, b)) ++modal_blue;
      if (b > 165 && int(b) > int(r) + 50) ++ok_blue;
      if (IsBottomTealBar(r, g, b)) ++teal;
      if (IsRedBackColor(r, g, b)) ++red;
      if (IsGreenSelectColor(r, g, b)) ++green;
      if (r > 200 && g > 205 && b > 205) ++bright_white;
      if (luma > 185 || (r > 200 && g > 140 && b < 80)) ++master_hl;
      if (luma < 65) ++dark_navy;
      if (r < 95 && g < 115 && int(b) > int(r) + 20 && luma < 120) ++padlock_navy;
      ++total;
    }
  }
  if (total > 0) {
    const float inv = 1.0f / float(total);
    st.gold_frac = float(gold) * inv;
    st.blue_frac = float(blue) * inv;
    st.btn_blue_frac = float(btn_blue) * inv;
    st.modal_blue_frac = float(modal_blue) * inv;
    st.ok_blue_frac = float(ok_blue) * inv;
    st.teal_frac = float(teal) * inv;
    st.red_frac = float(red) * inv;
    st.green_frac = float(green) * inv;
    st.bright_white_frac = float(bright_white) * inv;
    st.master_hl_frac = float(master_hl) * inv;
    st.dark_navy_frac = float(dark_navy) * inv;
    st.padlock_navy_frac = float(padlock_navy) * inv;
    st.avg_r = float(r_sum) * inv;
    st.avg_g = float(g_sum) * inv;
    st.avg_b = float(b_sum) * inv;
    st.avg_luma = float(luma_sum) * inv;
  }
  return st;
}

float SampleNonPanelBlueFracLocked(float x0, float y0, float x1, float y1,
                                   bool require_warm = true) {
  const uint32_t W = g_sw_curr_frame.width;
  const uint32_t H = g_sw_curr_frame.height;
  if (W == 0 || H == 0 || g_sw_curr_frame.pixels.size() < size_t(W) * H * 4) return 0.0f;
  const uint32_t px0 = std::clamp<uint32_t>(uint32_t(x0 * float(W)), 0, W - 1);
  const uint32_t py0 = std::clamp<uint32_t>(uint32_t(y0 * float(H)), 0, H - 1);
  const uint32_t px1 = std::clamp<uint32_t>(uint32_t(x1 * float(W) + 0.5f), px0 + 1, W);
  const uint32_t py1 = std::clamp<uint32_t>(uint32_t(y1 * float(H) + 0.5f), py0 + 1, H);
  const uint8_t* curr = g_sw_curr_frame.pixels.data();
  uint32_t total = 0, glow = 0;
  for (uint32_t y = py0; y < py1; ++y) {
    for (uint32_t x = px0; x < px1; ++x) {
      const size_t off = (size_t(y) * W + x) * 4;
      const uint8_t r = curr[off + 0];
      const uint8_t g = curr[off + 1];
      const uint8_t b = curr[off + 2];
      const int luma = (int(r) * 299 + int(g) * 587 + int(b) * 114) / 1000;
      const bool is_glow = require_warm
          ? (luma > 160 && (r > 175 || int(b) - int(r) < 30))
          : (luma > 175);
      if (is_glow) ++glow;
      ++total;
    }
  }
  return (total > 0) ? (float(glow) / float(total)) : 0.0f;
}

void SampleCursorRgbLocked(float norm_x, float norm_y) {
  const uint32_t W = g_sw_curr_frame.width;
  const uint32_t H = g_sw_curr_frame.height;
  if (W == 0 || H == 0 || g_sw_curr_frame.pixels.size() < size_t(W) * H * 4) {
    g_sw_status.has_cursor_rgb = false;
    return;
  }
  const float cx = std::clamp(norm_x, 0.0f, 0.9999f);
  const float cy = std::clamp(norm_y, 0.0f, 0.9999f);
  const uint32_t px = std::clamp<uint32_t>(uint32_t(cx * float(W)), 0, W - 1);
  const uint32_t py = std::clamp<uint32_t>(uint32_t(cy * float(H)), 0, H - 1);
  const size_t off = (size_t(py) * W + px) * 4;
  g_sw_status.cursor_r = g_sw_curr_frame.pixels[off + 0];
  g_sw_status.cursor_g = g_sw_curr_frame.pixels[off + 1];
  g_sw_status.cursor_b = g_sw_curr_frame.pixels[off + 2];
  g_sw_status.has_cursor_rgb = true;
}

RexCandidateButton MakeScoredBoxLocked(float x0, float y0, float x1, float y1, int8_t row,
                                       int8_t col, uint16_t flags, const char* name) {
  RexCandidateButton c{};
  c.x0 = x0;
  c.y0 = y0;
  c.x1 = x1;
  c.y1 = y1;
  c.cu = 0.5f * (x0 + x1);
  c.cv = 0.5f * (y0 + y1);
  c.row = row;
  c.col = col;
  c.flags = flags;
  std::snprintf(c.name, sizeof(c.name), "%s", name ? name : "BTN");
  const RegionColorStats st = SampleRegionColorsLocked(x0, y0, x1, y1);
  c.gold_frac = st.gold_frac;
  c.blue_frac = st.blue_frac;
  c.total_score = st.gold_frac * 100.0f;
  return c;
}

void UpdateScreenWatcherFrameLocked(uint64_t now_ms) {
  if (g_has_synthetic_snap || g_has_synthetic_frame) return;

  rex::ui::Presenter::ScreenWatcherFrame new_frame;
  if (!rex::ui::Presenter::GetLatestScreenWatcherFrame(new_frame, g_sw_curr_frame.sequence)) {
    return;
  }

  g_sw_prev_frame = std::move(g_sw_curr_frame);
  g_sw_curr_frame = std::move(new_frame);
  g_sw_status.last_frame_seq = g_sw_curr_frame.sequence;
  g_sw_status.last_update_ms = now_ms;
  g_sw_status.has_pixel_frame = !g_sw_curr_frame.pixels.empty();
  uint32_t pix_hash = 2166136261u;
  const uint8_t* hash_px = g_sw_curr_frame.pixels.data();
  const size_t hash_bytes = g_sw_curr_frame.pixels.size();
  for (size_t i = 0; i < hash_bytes; i += 16) {
    pix_hash ^= hash_px[i];
    pix_hash *= 16777619u;
  }
  const uint32_t new_hash = hash_bytes ? pix_hash : 0u;
  if (new_hash != 0u &&
      (new_hash != g_sw_status.pixel_hash || g_sw_status.last_hash_change_ms == 0)) {
    g_sw_status.last_hash_change_ms = now_ms;
  }
  g_sw_status.pixel_hash = new_hash;

  const size_t px_count = size_t(g_sw_curr_frame.width) * size_t(g_sw_curr_frame.height);
  if (px_count > 0 && g_sw_prev_frame.pixels.size() == g_sw_curr_frame.pixels.size()) {
    const uint8_t* curr = g_sw_curr_frame.pixels.data();
    const uint8_t* prev = g_sw_prev_frame.pixels.data();
    size_t changed = 0;
    for (size_t i = 0; i < px_count; ++i) {
      if (std::abs(int(curr[i * 4 + 3]) - int(prev[i * 4 + 3])) > 25) {
        ++changed;
      }
    }
    g_sw_status.motion_pct = 100.0f * float(changed) / float(px_count);
    g_sw_status.in_transition = (g_sw_status.motion_pct > 30.0f);
    if (g_sw_status.in_transition) {
      g_sw_prev_pulse_valid = false;
      g_sw_used_orthogonal = false;
    }
  } else {
    g_sw_status.motion_pct = 0.0f;
    g_sw_status.in_transition = false;
  }

  if (g_sw_has_cursor_pos) {
    SampleCursorRgbLocked(g_sw_last_cursor_u, g_sw_last_cursor_v);
  }
}

struct CanonicalBoxSpec {
  float x0, y0, x1, y1;
  int8_t row, col;
  uint16_t flags;
  const char* name;
};

constexpr CanonicalBoxSpec kModalOkBoxes[] = {
  {0.410f, 0.835f, 0.590f, 0.930f, 0, 0, kCandNavigable | kCandImmediateA, "OK_BTN"},
};

constexpr CanonicalBoxSpec kModalYesNoBoxes[] = {
  {0.250f, 0.840f, 0.430f, 0.925f, 0, 0, kCandNavigable | kCandBackBtn, "MODAL_CANCEL_B"},
  {0.570f, 0.840f, 0.750f, 0.925f, 0, 1, kCandNavigable | kCandImmediateA, "MODAL_OK_A"},
};

constexpr CanonicalBoxSpec kPauseMenuBoxes[] = {
  {0.35f, 0.18f, 0.57f, 0.26f, 0, 0, kCandNavigable, "PAUSE_RESUME"},
  {0.35f, 0.28f, 0.57f, 0.36f, 1, 0, kCandNavigable, "PAUSE_RESTART"},
  {0.35f, 0.38f, 0.57f, 0.46f, 2, 0, kCandNavigable, "PAUSE_OPTIONS"},
  {0.35f, 0.48f, 0.57f, 0.56f, 3, 0, kCandNavigable, "PAUSE_QUIT"},
};

constexpr CanonicalBoxSpec kTitleBoxes[] = {
  {0.395f, 0.815f, 0.605f, 0.915f, 0, 0, kCandNavigable, "PLAY_BTN"},
};

constexpr CanonicalBoxSpec kHelpOptionsBoxes[] = {
  {0.355f, 0.272f, 0.560f, 0.362f, 0, 0, kCandNavigable, "OPT_HOW_TO_PLAY"},
  {0.360f, 0.390f, 0.555f, 0.474f, 1, 0, kCandNavigable, "OPT_SETTINGS"},
  {0.360f, 0.508f, 0.555f, 0.592f, 2, 0, kCandNavigable, "OPT_CREDITS"},
  {0.360f, 0.624f, 0.555f, 0.708f, 3, 0, kCandNavigable, "OPT_DONE"},
};

constexpr CanonicalBoxSpec kMasterSelectBoxes[] = {
  {0.350f, 0.365f, 0.440f, 0.572f, 0, 0, kCandNavigable, "MASTER_0 (Bjorn)"},
  {0.462f, 0.365f, 0.552f, 0.572f, 0, 1, kCandNavigable, "MASTER_1 (Jeffrey)"},
  {0.574f, 0.365f, 0.664f, 0.572f, 0, 2, kCandNavigable, "MASTER_2 (Berg)"},
  {0.686f, 0.365f, 0.776f, 0.572f, 0, 3, kCandNavigable, "MASTER_3 (Gnorman)"},
  {0.798f, 0.365f, 0.888f, 0.572f, 0, 4, kCandNavigable, "MASTER_4 (Luna)"},
  {0.574f, 0.600f, 0.664f, 0.790f, 1, 2, kCandNavigable, "MASTER_RANDOM"},
};

constexpr CanonicalBoxSpec kLevelSelectBoxes[] = {
  {0.212f, 0.148f, 0.280f, 0.264f, 0, 0, kCandNavigable, "HEX_1"},
  {0.248f, 0.274f, 0.326f, 0.390f, 0, 1, kCandNavigable, "HEX_2"},
  {0.320f, 0.195f, 0.394f, 0.310f, 0, 2, kCandNavigable, "HEX_3"},
  {0.362f, 0.308f, 0.436f, 0.422f, 0, 3, kCandNavigable, "HEX_4"},
  {0.438f, 0.248f, 0.512f, 0.362f, 0, 4, kCandNavigable, "HEX_5"},
  {0.514f, 0.190f, 0.588f, 0.304f, 0, 5, kCandNavigable, "HEX_6"},
  {0.548f, 0.306f, 0.624f, 0.420f, 0, 6, kCandNavigable, "HEX_7"},
  {0.626f, 0.246f, 0.700f, 0.360f, 0, 7, kCandNavigable, "HEX_8"},
  {0.702f, 0.178f, 0.774f, 0.292f, 0, 8, kCandNavigable, "HEX_9"},
  {0.744f, 0.282f, 0.818f, 0.396f, 0, 9, kCandNavigable, "HEX_10"},
};

constexpr CanonicalBoxSpec kAdventurePeggleInstituteBoxes[] = {
  {0.165f, 0.445f, 0.278f, 0.705f,  0,  0, kCandNavigable, "ADV_LEVELS"},
  {0.285f, 0.445f, 0.402f, 0.705f,  0,  1, kCandNavigable, "ADV_TRIALS"},
  {0.580f, 0.340f, 0.960f, 0.740f, -1, -1, kCandCarouselScrollRight, "ADV_NEXT_WORLD"},
};

constexpr CanonicalBoxSpec kAdventureBoxes[] = {
  {0.010f, 0.340f, 0.155f, 0.740f, -1, -1, kCandCarouselScrollLeft, "ADV_PREV_WORLD"},
  {0.228f, 0.445f, 0.342f, 0.705f,  0,  0, kCandNavigable, "ADV_LEVELS"},
  {0.348f, 0.445f, 0.462f, 0.705f,  0,  1, kCandNavigable, "ADV_TRIALS"},
  {0.580f, 0.340f, 0.960f, 0.740f, -1, -1, kCandCarouselScrollRight, "ADV_NEXT_WORLD"},
};

constexpr CanonicalBoxSpec kAdventureCelestialBoxes[] = {
  {0.010f, 0.340f, 0.155f, 0.740f, -1, -1, kCandCarouselScrollLeft, "ADV_PREV_WORLD"},
  {0.228f, 0.445f, 0.342f, 0.705f,  0,  0, kCandNavigable, "ADV_LEVELS"},
  {0.348f, 0.445f, 0.462f, 0.705f,  0,  1, kCandNavigable, "ADV_TRIALS"},
};

constexpr CanonicalBoxSpec kMultiplayerMenuBoxes[] = {
  {0.42f, 0.19f, 0.73f, 0.32f, 0, 0, kCandNavigable, "MP_QUICK_MATCH"},
  {0.44f, 0.36f, 0.71f, 0.49f, 1, 0, kCandNavigable, "MP_PEG_PARTY"},
  {0.44f, 0.53f, 0.71f, 0.66f, 2, 0, kCandNavigable, "MP_PRIVATE_DUEL"},
  {0.44f, 0.70f, 0.71f, 0.83f, 3, 0, kCandNavigable, "MP_LOCAL_DUEL"},
};

constexpr CanonicalBoxSpec kMpModeSelectBoxes[] = {
  {0.395f, 0.275f, 0.715f, 0.435f, 0, 0, kCandNavigable, "MP_MODE_PEG_PARTY"},
  {0.410f, 0.455f, 0.695f, 0.610f, 1, 0, kCandNavigable, "MP_MODE_DUEL"},
};

constexpr CanonicalBoxSpec kMpLocalDuelBoxes[] = {
  {0.212f, 0.045f, 0.302f, 0.265f,  0,  0, kCandNavigable, "LOBBY_MASTER_0 (Bjorn)"},
  {0.310f, 0.045f, 0.400f, 0.265f,  0,  1, kCandNavigable, "LOBBY_MASTER_1 (Jeffrey)"},
  {0.408f, 0.045f, 0.498f, 0.265f,  0,  2, kCandNavigable, "LOBBY_MASTER_2 (Berg)"},
  {0.506f, 0.045f, 0.596f, 0.265f,  0,  3, kCandNavigable, "LOBBY_MASTER_3 (Gnorman)"},
  {0.604f, 0.045f, 0.694f, 0.265f,  0,  4, kCandNavigable, "LOBBY_MASTER_4 (Luna)"},
  {0.702f, 0.045f, 0.792f, 0.265f,  0,  5, kCandNavigable, "LOBBY_MASTER_RANDOM (?)"},
  {0.270f, 0.388f, 0.515f, 0.445f,  1,  0, kCandNavigable, "LOBBY_MODE"},
  {0.270f, 0.452f, 0.515f, 0.512f,  2,  0, kCandNavigable, "LOBBY_LEVEL"},
  {0.270f, 0.528f, 0.525f, 0.582f,  3,  0, kCandNavigable, "LOBBY_SHOT_CLOCK"},
  {0.405f, 0.588f, 0.585f, 0.675f, -1, -1, kCandRightShoulder, "LOBBY_SETTINGS_RB"},
};

constexpr CanonicalBoxSpec kSettingsRowBoxes[] = {
  {0.380f, 0.355f, 0.630f, 0.435f, 0, 0, kCandNavigable, "SET_COLORBLIND"},
  {0.285f, 0.468f, 0.745f, 0.542f, 1, 0, kCandNavigable | kCandSettingsRow, "SET_SOUND_VOL"},
  {0.285f, 0.576f, 0.745f, 0.650f, 2, 0, kCandNavigable | kCandSettingsRow, "SET_MUSIC_VOL"},
  {0.285f, 0.680f, 0.745f, 0.755f, 3, 0, kCandNavigable | kCandSettingsRow, "SET_SCREEN_SIZE"},
  {0.415f, 0.770f, 0.585f, 0.848f, 4, 0, kCandNavigable, "SET_ACCEPT"},
};

constexpr CanonicalBoxSpec kMainMenuBoxes[] = {
  {0.405f, 0.155f, 0.735f, 0.308f, 0, 0, kCandNavigable, "SINGLE PLAYER"},
  {0.440f, 0.362f, 0.765f, 0.492f, 1, 0, kCandNavigable, "MULTIPLAYER"},
  {0.268f, 0.618f, 0.388f, 0.768f, 2, 0, kCandNavigable, "MENU BTN 0"},
  {0.398f, 0.618f, 0.518f, 0.768f, 2, 1, kCandNavigable, "MENU BTN 1"},
  {0.528f, 0.618f, 0.648f, 0.768f, 2, 2, kCandNavigable, "MENU BTN 2"},
  {0.658f, 0.618f, 0.778f, 0.768f, 2, 3, kCandNavigable, "MENU BTN 3"},
  {0.788f, 0.618f, 0.908f, 0.768f, 2, 4, kCandNavigable, "MENU BTN 4"},
};

void AppendCanonicalBoxesLocked(const CanonicalBoxSpec* specs, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const auto& s = specs[i];
    g_sw_candidates.push_back(
        MakeScoredBoxLocked(s.x0, s.y0, s.x1, s.y1, s.row, s.col, s.flags, s.name));
  }
}

void AppendFooterBoxesLocked(bool include_y = false, const char* y_label = "CHANGE_COSTUME (Y)") {
  g_sw_candidates.push_back(
      MakeScoredBoxLocked(0.024f, 0.905f, 0.175f, 0.978f, -1, -1, kCandBackBtn, "BACK (B)"));
  if (include_y) {
    g_sw_candidates.push_back(
        MakeScoredBoxLocked(0.262f, 0.905f, 0.422f, 0.978f, -1, -1, kCandCostumeBtn, y_label));
  }
  g_sw_candidates.push_back(
      MakeScoredBoxLocked(0.825f, 0.905f, 0.976f, 0.978f, -1, -1, kCandSelectBtn, "SELECT (A)"));
}

int FindNavCandidateByGridLocked(int8_t row, int8_t col) {
  int best_idx = -1;
  int best_dist = 10000;
  for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
    const auto& c = g_sw_candidates[i];
    if (!(c.flags & kCandNavigable) || c.row < 0 || c.col < 0) continue;
    const int d = std::abs(int(c.row) - int(row)) * 100 + std::abs(int(c.col) - int(col));
    if (d < best_dist) {
      best_dist = d;
      best_idx = int(i);
    }
  }
  return best_idx;
}

int DetectHighlightIndexLocked(RexScreenId screen_id) {
  if (screen_id == SCREEN_TITLE || (screen_id == SCREEN_MODAL_OK_POPUP && !s_modal_is_yesno)) {
    for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
      if (g_sw_candidates[i].flags & kCandNavigable) return int(i);
    }
    return -1;
  }

  if (screen_id == SCREEN_MASTER_SELECT) {
    auto score_master = [](const RexCandidateButton& c) -> float {
      if (c.row == 0) {
        const float top_exp =
            SampleNonPanelBlueFracLocked(c.x0 + 0.01f, 0.345f, c.x1 - 0.01f, 0.365f, false);
        const float bot_exp =
            SampleNonPanelBlueFracLocked(c.x0 + 0.01f, 0.570f, c.x1 - 0.01f, 0.588f, true);
        return (top_exp >= 0.04f) ? 0.5f * (top_exp + bot_exp) : bot_exp;
      }
      return SampleNonPanelBlueFracLocked(c.x0 + 0.01f, 0.798f, c.x1 - 0.01f, 0.814f, true);
    };
    const int tracked_idx = FindNavCandidateByGridLocked(s_master_row, s_master_col);
    float tracked_score = 0.0f;
    int best_idx = -1;
    float best_score = 0.0f;
    float second_score = 0.0f;
    for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
      auto& c = g_sw_candidates[i];
      if (!(c.flags & kCandNavigable)) continue;
      const float s = score_master(c);
      c.total_score = s * 100.0f;
      if (int(i) == tracked_idx) tracked_score = s;
      if (s > best_score) {
        second_score = best_score;
        best_score = s;
        best_idx = int(i);
      } else if (s > second_score) {
        second_score = s;
      }
    }
    const bool clear_other_winner = (best_idx >= 0 && best_idx != tracked_idx &&
                                     best_score >= 0.22f && best_score > tracked_score + 0.08f);
    if (tracked_idx >= 0 && tracked_score >= 0.14f && !clear_other_winner) {
      return tracked_idx;
    }
    if (best_idx >= 0 && best_score >= 0.22f && (best_score - second_score) >= 0.08f) {
      s_master_row = g_sw_candidates[size_t(best_idx)].row;
      s_master_col = g_sw_candidates[size_t(best_idx)].col;
      return best_idx;
    }
    return tracked_idx;
  }

  if (screen_id == SCREEN_LEVEL_SELECT) {
    // Deterministic 0..9 focus tracking: initialize to 0 on entry and step +/-1 on DPAD only.
    g_sw_stepped_col = s_level_idx;
    return FindNavCandidateByGridLocked(0, s_level_idx);
  }

  if (screen_id == SCREEN_ADVENTURE_CAROUSEL) {
    int idx0 = -1, idx1 = -1;
    float g0 = 0.0f, g1 = 0.0f;
    RegionColorStats top0{}, bot0{}, top1{}, bot1{};
    for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
      auto& c = g_sw_candidates[i];
      if (!(c.flags & kCandNavigable)) continue;
      if (c.col == 0) {
        idx0 = int(i);
        g0 = c.gold_frac;
        top0 = SampleRegionColorsLocked(c.x0, 0.44f, c.x1, 0.47f);
        bot0 = SampleRegionColorsLocked(c.x0, 0.655f, c.x1, 0.705f);
      } else if (c.col == 1) {
        idx1 = int(i);
        g1 = c.gold_frac;
        top1 = SampleRegionColorsLocked(c.x0, 0.44f, c.x1, 0.47f);
        bot1 = SampleRegionColorsLocked(c.x0, 0.655f, c.x1, 0.705f);
      }
    }
    const bool has_gold = (std::max(g0, g1) >= 0.08f);
    const float nav0 = top0.padlock_navy_frac + bot0.padlock_navy_frac;
    const float nav1 = top1.padlock_navy_frac + bot1.padlock_navy_frac;
    const bool both_padlocks = (!has_gold && std::max(nav0, nav1) >= 0.25f);
    // Zoomed locked padlocks extend dark steel-blue pixels into top/bot bands instead of exposing ice-blue card glass.
    auto finalize_score = [&](int idx, float g, const RegionColorStats& top_s,
                              const RegionColorStats& bot_s, float nav) -> float {
      if (idx < 0) return 0.0f;
      float s = 0.0f;
      if (both_padlocks) {
        s = nav * 2.0f + (top_s.bright_white_frac + bot_s.bright_white_frac) * 0.5f;
      } else {
        const float exp_top = top_s.gold_frac * 2.0f + top_s.blue_frac + top_s.bright_white_frac;
        const float exp_bot = bot_s.gold_frac * 2.0f + bot_s.blue_frac + bot_s.bright_white_frac;
        s = g * 3.0f + exp_top + exp_bot;
      }
      g_sw_candidates[size_t(idx)].total_score = s * 100.0f;
      return s;
    };
    const float s0 = finalize_score(idx0, g0, top0, bot0, nav0);
    const float s1 = finalize_score(idx1, g1, top1, bot1, nav1);
    if (idx0 >= 0 && idx1 >= 0 && std::fabs(s0 - s1) >= 0.04f) {
      return (s0 >= s1) ? idx0 : idx1;
    }
    return FindNavCandidateByGridLocked(0, g_sw_stepped_col);
  }

  if (screen_id == SCREEN_SETTINGS_SUBMENU || screen_id == SCREEN_MP_LOCAL_DUEL ||
      screen_id == SCREEN_PAUSE_MENU) {
    const float min_gold = (screen_id == SCREEN_MP_LOCAL_DUEL) ? 0.10f : 0.04f;
    int best_hl_idx = -1;
    float best_gold = min_gold;
    for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
      const auto& c = g_sw_candidates[i];
      if (!(c.flags & kCandNavigable)) continue;
      if (c.gold_frac > best_gold) {
        best_gold = c.gold_frac;
        best_hl_idx = int(i);
      }
    }
    if (best_hl_idx >= 0) {
      const auto& win = g_sw_candidates[size_t(best_hl_idx)];
      if (screen_id == SCREEN_SETTINGS_SUBMENU) {
        s_settings_row = win.row;
      } else if (screen_id == SCREEN_PAUSE_MENU) {
        s_pause_row = win.row;
      } else if (best_gold >= 0.14f) {
        s_lobby_row = win.row;
        s_lobby_col = win.col;
      }
      return best_hl_idx;
    }
    if (screen_id == SCREEN_PAUSE_MENU) return FindNavCandidateByGridLocked(s_pause_row, 0);
    return (screen_id == SCREEN_SETTINGS_SUBMENU)
               ? FindNavCandidateByGridLocked(s_settings_row, 0)
               : FindNavCandidateByGridLocked(s_lobby_row, s_lobby_col);
  }

  int best_hl_idx = -1;
  float best_gold = 0.04f;
  for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
    const auto& c = g_sw_candidates[i];
    if (!(c.flags & kCandNavigable)) continue;
    if (c.gold_frac > best_gold) {
      best_gold = c.gold_frac;
      best_hl_idx = int(i);
    }
  }
  return best_hl_idx;
}

void SetLockedScreenLocked(RexScreenId next_screen) {
  if (next_screen == s_current_screen) return;
  const RexScreenId prev_locked = s_current_screen;
  s_current_screen = next_screen;
  if (next_screen != SCREEN_MODAL_OK_POPUP) {
    s_modal_is_yesno = false;
  }
  g_atomic_screen_id.store(uint8_t(next_screen), std::memory_order_relaxed);
  g_prev_classified_screen = next_screen;
  g_sw_stepped_row = 0;
  g_sw_stepped_col = 0;
  g_sw_has_stepped_focus = true;
  g_carousel_hover_side = 0;
  g_carousel_pending_steps = 0;
  g_carousel_pending_dir = 0;
  g_carousel_next_pulse_ms = 0;
  g_carousel_next_scroll_ms = 0;
  g_sw_prev_pulse_valid = false;
  g_sw_used_orthogonal = false;
  g_sw_last_pulse_frame_seq = 0;
  if (next_screen == SCREEN_ADVENTURE_CAROUSEL &&
      prev_locked != SCREEN_LEVEL_SELECT && prev_locked != SCREEN_MODAL_OK_POPUP) {
    s_adv_world_idx = 0;
  }
  if (next_screen == SCREEN_LEVEL_SELECT &&
      prev_locked != SCREEN_MASTER_SELECT && prev_locked != SCREEN_MODAL_OK_POPUP) {
    s_level_idx = 0;
  } else if (next_screen == SCREEN_LEVEL_SELECT) {
    g_sw_stepped_col = s_level_idx;
  }
  if (next_screen == SCREEN_MASTER_SELECT && prev_locked != SCREEN_MODAL_OK_POPUP) {
    s_master_row = 0;
    s_master_col = 0;
  } else if (next_screen == SCREEN_MASTER_SELECT) {
    g_sw_stepped_row = s_master_row;
    g_sw_stepped_col = s_master_col;
  }
  if (next_screen == SCREEN_MP_LOCAL_DUEL && prev_locked != SCREEN_MODAL_OK_POPUP) {
    s_lobby_row = 0;
    s_lobby_col = 0;
  } else if (next_screen == SCREEN_MP_LOCAL_DUEL) {
    g_sw_stepped_row = s_lobby_row;
    g_sw_stepped_col = s_lobby_col;
  }
  if (next_screen == SCREEN_SETTINGS_SUBMENU && prev_locked != SCREEN_MODAL_OK_POPUP) {
    s_settings_row = 0;
  } else if (next_screen == SCREEN_SETTINGS_SUBMENU) {
    g_sw_stepped_row = s_settings_row;
  }
  if (next_screen == SCREEN_PAUSE_MENU) {
    if (prev_locked == SCREEN_GAMEPLAY) {
      s_pause_row = 0;
    }
    g_sw_stepped_row = s_pause_row;
    s_paused_turret_tick_count = g_gameplay_tick_count.load(std::memory_order_relaxed);
  } else if (next_screen != SCREEN_GAMEPLAY) {
    s_paused_turret_tick_count = g_gameplay_tick_count.load(std::memory_order_relaxed);
  }
  rex::ui::Presenter::SetScreenWatcherFastCadence(next_screen == SCREEN_GAMEPLAY);
  if (next_screen != SCREEN_GAMEPLAY) {
    s_last_gameplay_start_ms = 0;
  }
  if (prev_locked == SCREEN_TITLE && next_screen != SCREEN_TITLE) {
    s_last_title_exit_ms = (g_last_step_now_ms != 0) ? g_last_step_now_ms : NowMs();
  }
  s_title_heal_streak = 0;
  s_title_heal_last_seq = ~0ull;
  s_title_heal_first_ms = 0;
  g_audit_last_seq = ~0ull;
  g_audit_last_screen = SCREEN_UNKNOWN;
  g_audit_last_target.clear();
}

bool HasActivePixelFrameLocked() {
  return !g_has_synthetic_snap && g_sw_curr_frame.width > 0 && g_sw_curr_frame.height > 0 &&
         g_sw_curr_frame.pixels.size() == size_t(g_sw_curr_frame.width) * g_sw_curr_frame.height * 4;
}

bool IsFullScreenFadeLocked() {
  const RegionColorStats full = SampleRegionColorsLocked(0.05f, 0.05f, 0.95f, 0.95f);
  return (full.avg_luma > 210.0f) || (full.avg_luma < 18.0f);
}

void ArmMenuTransitionLocked(uint16_t action_mask, const char* target_name, float target_gold,
                             uint64_t now_ms) {
  if ((action_mask & 0x0010) != 0 && s_current_screen == SCREEN_GAMEPLAY) {
    s_last_gameplay_start_ms = now_ms;
  }
  if ((action_mask & (0x1000 | 0x2000 | 0x0010 | 0x8000 | 0x0200)) == 0) return;
  s_transition_pending = true;
  s_pending_source_screen = s_current_screen;
  s_pending_action_mask = action_mask;
  s_pending_target_gold = target_gold;
  std::snprintf(s_pending_target_name, sizeof(s_pending_target_name), "%s",
                (target_name && target_name[0]) ? target_name : "");
  s_transition_settle_ms = now_ms + 450ull;
  s_page_watch_active = true;
  s_page_watch_used_fallback = false;
  s_page_watch_started_ms = now_ms;
  s_page_watch_until_ms = now_ms + 1500ull;
  s_page_watch_source = s_current_screen;
  if (g_audit_enabled) {
    g_audit_last_screen = s_current_screen;
    g_audit_last_seq = g_sw_curr_frame.sequence;
    g_audit_last_target =
        (g_sw_status.has_target && g_sw_status.target_name[0]) ? g_sw_status.target_name : "None";
    WriteAuditLineLocked("CLICK", now_ms, action_mask);
  }
}

PageFingerprint ComputePageFingerprintLocked() {
  PageFingerprint fp{};
  const RegionColorStats bar_l = SampleRegionColorsLocked(0.19f, 0.92f, 0.24f, 0.98f);
  const RegionColorStats bar_r = SampleRegionColorsLocked(0.76f, 0.92f, 0.81f, 0.98f);
  const RegionColorStats bar_back = SampleRegionColorsLocked(0.03f, 0.91f, 0.14f, 0.97f);
  const RegionColorStats bar_pill = SampleRegionColorsLocked(0.025f, 0.905f, 0.175f, 0.978f);
  const RegionColorStats bar_sel = SampleRegionColorsLocked(0.85f, 0.91f, 0.96f, 0.97f);
  const RegionColorStats bar_sel_wide = SampleRegionColorsLocked(0.825f, 0.905f, 0.978f, 0.978f);
  auto is_teal_bar = [](const RegionColorStats& s) {
    return s.avg_r < 50.0f && s.avg_g >= 40.0f && s.avg_g <= 120.0f &&
           s.avg_b >= 60.0f && s.avg_b <= 140.0f &&
           s.avg_b >= s.avg_g && s.avg_g >= s.avg_r + 12.0f;
  };
  fp.has_teal_strip = is_teal_bar(bar_l) && is_teal_bar(bar_r) && (bar_back.red_frac >= 0.025f);
  fp.has_back_pill = !IsFullScreenFadeLocked() &&
                     (bar_pill.ui_frac() >= 0.10f) && (bar_back.red_frac >= 0.025f) &&
                     (bar_sel_wide.green_frac >= 0.01f || bar_sel_wide.ui_frac() >= 0.08f ||
                      bar_sel_wide.avg_g > bar_sel_wide.avg_r + 8.0f);
  fp.has_bottom_bar = fp.has_teal_strip || fp.has_back_pill;

  const RegionColorStats modal_body = SampleRegionColorsLocked(0.32f, 0.28f, 0.68f, 0.72f);
  fp.is_bottom_bar_dimmed =
      !fp.has_bottom_bar && (bar_back.avg_luma < 65.0f) && (bar_sel.avg_luma < 65.0f) &&
      !IsFullScreenFadeLocked() && (modal_body.modal_blue_frac > 0.45f);

  const RegionColorStats costume = SampleRegionColorsLocked(0.27f, 0.91f, 0.41f, 0.97f);
  fp.has_costume_btn = fp.has_bottom_bar && (costume.blue_frac > 0.30f);

  const RegionColorStats p2_pillar = SampleRegionColorsLocked(0.86f, 0.15f, 0.98f, 0.75f);
  fp.has_green_p2_pillar =
      (p2_pillar.avg_g > p2_pillar.avg_r + 40.0f) && (p2_pillar.avg_g > p2_pillar.avg_b + 25.0f);

  const RegionColorStats tl = SampleRegionColorsLocked(0.02f, 0.02f, 0.18f, 0.10f);
  const RegionColorStats tl_adv = SampleRegionColorsLocked(0.03f, 0.07f, 0.21f, 0.16f);
  fp.has_adventure_sign =
      ((tl.avg_b < 115.0f) && (tl.avg_r > tl.avg_b)) ||
      (fp.has_bottom_bar && (tl_adv.gold_frac >= 0.025f));

  const RegionColorStats obj_box = SampleRegionColorsLocked(0.45f, 0.43f, 0.74f, 0.74f);
  fp.has_level_board =
      (obj_box.avg_r < 45.0f) &&
      (obj_box.avg_g >= 65.0f && obj_box.avg_g <= 125.0f) &&
      (obj_box.avg_b >= 90.0f && obj_box.avg_b <= 155.0f) &&
      (tl.avg_b > 155.0f);
  return fp;
}

bool HasBottomBarLocked() {
  return ComputePageFingerprintLocked().has_bottom_bar;
}

int DetectModalDialogLocked() {
  const RegionColorStats bar_left = SampleRegionColorsLocked(0.03f, 0.91f, 0.14f, 0.97f);
  const RegionColorStats bar_right = SampleRegionColorsLocked(0.85f, 0.91f, 0.96f, 0.97f);
  if (bar_left.avg_luma >= 65.0f || bar_right.avg_luma >= 65.0f) return 0;
  const RegionColorStats modal_body = SampleRegionColorsLocked(0.32f, 0.28f, 0.68f, 0.72f);
  const RegionColorStats modal_mid_gold = SampleRegionColorsLocked(0.32f, 0.18f, 0.68f, 0.75f);
  if (modal_body.modal_blue_frac <= 0.35f || modal_mid_gold.gold_frac >= 0.02f) return 0;

  const RegionColorStats cancel_btn = SampleRegionColorsLocked(0.250f, 0.840f, 0.430f, 0.925f);
  const RegionColorStats ok2_btn = SampleRegionColorsLocked(0.570f, 0.840f, 0.750f, 0.925f);
  const RegionColorStats dead_ctr = SampleRegionColorsLocked(0.460f, 0.855f, 0.540f, 0.922f);
  const RegionColorStats b_badge = SampleRegionColorsLocked(0.262f, 0.850f, 0.302f, 0.908f);
  const RegionColorStats a2_badge = SampleRegionColorsLocked(0.692f, 0.850f, 0.734f, 0.908f);
  const bool cancel_lit =
      (cancel_btn.ui_frac() + cancel_btn.ok_blue_frac + cancel_btn.bright_white_frac) > 0.28f;
  const bool ok2_lit =
      (ok2_btn.ui_frac() + ok2_btn.ok_blue_frac + ok2_btn.bright_white_frac) > 0.28f;
  const bool center_empty = std::max(dead_ctr.ui_frac(), dead_ctr.ok_blue_frac) < 0.15f;
  const bool has_red_b = (b_badge.red_frac > 0.05f);
  const bool has_green_a2 =
      (a2_badge.green_frac > 0.05f) ||
      (a2_badge.avg_g > a2_badge.avg_r + 25.0f && a2_badge.avg_g > a2_badge.avg_b + 15.0f);
  if (modal_body.modal_blue_frac > 0.50f && cancel_lit && ok2_lit && center_empty &&
      (has_red_b || has_green_a2)) {
    return 2;
  }

  const RegionColorStats ok_btn = SampleRegionColorsLocked(0.415f, 0.840f, 0.585f, 0.925f);
  const RegionColorStats a_badge = SampleRegionColorsLocked(0.542f, 0.858f, 0.572f, 0.910f);
  const bool ok_lit =
      (ok_btn.ui_frac() > 0.30f) || ((ok_btn.ok_blue_frac + ok_btn.gold_frac) > 0.30f);
  const bool has_green_a =
      (a_badge.green_frac > 0.05f) ||
      (a_badge.avg_g > a_badge.avg_r + 20.0f && a_badge.avg_g > a_badge.avg_b + 12.0f);
  if (ok_lit && has_green_a) return 1;
  return 0;
}

bool IsModalOkVisibleLocked() {
  return DetectModalDialogLocked() != 0;
}

bool IsPauseOverlayTreeLocked() {
  return (s_current_screen == SCREEN_PAUSE_MENU) ||
         (s_current_screen == SCREEN_MODAL_OK_POPUP &&
          (s_prev_screen == SCREEN_PAUSE_MENU || s_prev_screen == SCREEN_GAMEPLAY)) ||
         (s_help_parent_screen == SCREEN_PAUSE_MENU &&
          (s_current_screen == SCREEN_HELP_OPTIONS ||
           s_current_screen == SCREEN_SETTINGS_SUBMENU ||
           s_current_screen == SCREEN_INFO_SUBMENU));
}

bool IsLevelHexLockedPadlockLocked(const RexCandidateButton& c) {
  if (!HasActivePixelFrameLocked() || c.row != 0 || c.col < 0 || c.col > 9) return false;
  const RegionColorStats hs = SampleRegionColorsLocked(
      c.x0 + 0.015f, c.y0 + 0.020f, c.x1 - 0.015f, c.y1 - 0.020f);
  // Locked padlock tiles are dark achromatic grey; unlocked peg-board previews are blue/green/teal or gold.
  const bool achromatic_grey =
      (std::fabs(hs.avg_r - hs.avg_g) < 10.0f) &&
      (std::fabs(hs.avg_g - hs.avg_b) < 10.0f) && (hs.avg_b < 105.0f);
  const bool colored_unlocked =
      ((hs.avg_b - hs.avg_r) > 25.0f) || ((hs.avg_g - hs.avg_r) > 25.0f);
  return achromatic_grey && !colored_unlocked;
}

RexScreenId EnterModalPopupLocked(RexScreenId parent, int modal_kind) {
  s_prev_screen = parent;
  s_modal_is_yesno = (modal_kind == 2);
  if (parent == SCREEN_MP_LOCAL_DUEL) {
    s_modal_yes_screen = SCREEN_MULTIPLAYER_MENU;
  } else if (parent == SCREEN_PAUSE_MENU) {
    s_modal_yes_screen =
        (std::string_view(s_pending_target_name) == "PAUSE_RESTART") ? SCREEN_GAMEPLAY
                                                                     : SCREEN_LEVEL_SELECT;
  } else {
    s_modal_yes_screen = parent;
  }
  return SCREEN_MODAL_OK_POPUP;
}

bool IsModalOkDismissedLocked() {
  const RegionColorStats bar_left = SampleRegionColorsLocked(0.03f, 0.91f, 0.14f, 0.97f);
  const RegionColorStats bar_right = SampleRegionColorsLocked(0.85f, 0.91f, 0.96f, 0.97f);
  return (bar_left.avg_luma >= 85.0f && bar_right.avg_luma >= 85.0f) && !IsFullScreenFadeLocked();
}

bool HasPauseMenuStackLocked() {
  if (HasBottomBarLocked()) return false;
  const RegionColorStats pause_frame = SampleRegionColorsLocked(0.31f, 0.15f, 0.60f, 0.75f);
  if (pause_frame.ui_frac() < 0.22f) return false;
  int pause_btn_hits = 0;
  for (const auto& b : kPauseMenuBoxes) {
    if (SampleRegionColorsLocked(b.x0, b.y0, b.x1, b.y1).ui_frac() >= 0.15f) ++pause_btn_hits;
  }
  return pause_btn_hits >= 3;
}

bool HasLevelSelectLandmarksLocked() {
  if (ComputePageFingerprintLocked().has_level_board) return true;
  const RegionColorStats grass = SampleRegionColorsLocked(0.50f, 0.74f, 0.65f, 0.82f);
  const bool is_green_grass =
      (grass.avg_g > grass.avg_b + 15.0f && grass.avg_g > grass.avg_r + 15.0f);
  const RegionColorStats tl = SampleRegionColorsLocked(0.02f, 0.02f, 0.18f, 0.11f);
  const bool has_tl_blue_sky = (tl.avg_b > 155.0f && tl.avg_b > tl.avg_r + 20.0f);
  int hex_hits = 0;
  for (const auto& b : kLevelSelectBoxes) {
    const RegionColorStats hs = SampleRegionColorsLocked(b.x0, b.y0, b.x1, b.y1);
    if (hs.ui_frac() >= 0.04f || hs.dark_navy_frac >= 0.20f || hs.gold_frac >= 0.03f) {
      ++hex_hits;
    }
  }
  const RegionColorStats bl = SampleRegionColorsLocked(0.20f, 0.42f, 0.24f, 0.78f);
  const RegionColorStats br = SampleRegionColorsLocked(0.76f, 0.42f, 0.80f, 0.78f);
  return !is_green_grass && has_tl_blue_sky && hex_hits >= 5 &&
         bl.modal_blue_frac >= 0.15f && br.modal_blue_frac >= 0.15f;
}

bool IsNeutralBrightRegion(const RegionColorStats& c) {
  return (c.avg_luma > 180.0f) && (std::abs(c.avg_r - c.avg_b) < 25.0f) &&
         (std::abs(c.avg_g - c.avg_b) < 25.0f);
}

bool IsWhiteSplashCornersLocked() {
  const RegionColorStats ul = SampleRegionColorsLocked(0.06f, 0.05f, 0.16f, 0.25f);
  const RegionColorStats ur = SampleRegionColorsLocked(0.84f, 0.05f, 0.94f, 0.25f);
  return IsNeutralBrightRegion(ul) && IsNeutralBrightRegion(ur);
}

bool LooksLikeHelpOptionsLocked();

bool LooksLikeTitleScreenLocked() {
  if (HasBottomBarLocked() || IsWhiteSplashCornersLocked() || LooksLikeHelpOptionsLocked()) {
    return false;
  }
  const RegionColorStats title_play = SampleRegionColorsLocked(0.420f, 0.840f, 0.580f, 0.905f);
  const RegionColorStats title_sky  = SampleRegionColorsLocked(0.15f, 0.05f, 0.85f, 0.35f);
  const RegionColorStats full_mid   = SampleRegionColorsLocked(0.10f, 0.10f, 0.90f, 0.85f);
  return (title_play.ui_frac() >= 0.25f) && (title_sky.avg_luma > 100.0f) &&
         (full_mid.avg_luma < 245.0f);
}

bool LooksLikeTitleSelfHealLocked() {
  if (HasBottomBarLocked() || IsFullScreenFadeLocked() || IsWhiteSplashCornersLocked()) return false;
  const RegionColorStats cloud_l = SampleRegionColorsLocked(0.18f, 0.92f, 0.38f, 0.98f);
  const RegionColorStats cloud_r = SampleRegionColorsLocked(0.62f, 0.92f, 0.82f, 0.98f);
  const bool pink_clouds =
      (cloud_l.avg_r > 145.0f && cloud_l.avg_g > 115.0f && cloud_l.avg_b > 150.0f) &&
      (cloud_r.avg_r > 145.0f && cloud_r.avg_g > 115.0f && cloud_r.avg_b > 150.0f);
  const RegionColorStats play_btn = SampleRegionColorsLocked(0.40f, 0.81f, 0.60f, 0.92f);
  const RegionColorStats sky_ul = SampleRegionColorsLocked(0.10f, 0.15f, 0.30f, 0.45f);
  return pink_clouds && (play_btn.ui_frac() >= 0.10f) && (sky_ul.avg_luma > 125.0f);
}

bool LooksLikeSettingsMenuLocked() {
  if (DetectModalDialogLocked() != 0) return false;
  const RegionColorStats bar_back = SampleRegionColorsLocked(0.03f, 0.91f, 0.14f, 0.97f);
  const RegionColorStats bar_sel = SampleRegionColorsLocked(0.85f, 0.91f, 0.96f, 0.97f);
  const RegionColorStats slider_troughs = SampleRegionColorsLocked(0.44f, 0.48f, 0.70f, 0.74f);
  const bool bright_footer = (bar_back.avg_luma >= 65.0f || bar_sel.avg_luma >= 65.0f);
  const bool has_sliders = (slider_troughs.dark_navy_frac >= 0.12f);
  if (!bright_footer && !has_sliders) return false;
  const RegionColorStats out_l = SampleRegionColorsLocked(0.03f, 0.24f, 0.16f, 0.72f);
  const RegionColorStats out_r = SampleRegionColorsLocked(0.84f, 0.24f, 0.97f, 0.72f);
  const RegionColorStats lf = SampleRegionColorsLocked(0.23f, 0.24f, 0.28f, 0.72f);
  const RegionColorStats rf = SampleRegionColorsLocked(0.72f, 0.24f, 0.77f, 0.72f);
  const RegionColorStats acc = SampleRegionColorsLocked(0.415f, 0.770f, 0.585f, 0.848f);
  const RegionColorStats modal_ok_area = SampleRegionColorsLocked(0.415f, 0.848f, 0.585f, 0.925f);
  return (out_l.modal_blue_frac < 0.20f) && (out_r.modal_blue_frac < 0.20f) &&
         (lf.modal_blue_frac >= 0.30f) && (rf.modal_blue_frac >= 0.30f) &&
         (acc.btn_frac() >= 0.20f) && (modal_ok_area.btn_frac() < 0.20f);
}

bool LooksLikeMpModeSelectLocked() {
  const RegionColorStats btn1 = SampleRegionColorsLocked(0.395f, 0.275f, 0.715f, 0.435f);
  const RegionColorStats btn1_r = SampleRegionColorsLocked(0.580f, 0.285f, 0.695f, 0.425f);
  const RegionColorStats peg_gap = SampleRegionColorsLocked(0.560f, 0.315f, 0.700f, 0.358f);
  const RegionColorStats btn2 = SampleRegionColorsLocked(0.410f, 0.455f, 0.695f, 0.610f);
  const RegionColorStats top_gap = SampleRegionColorsLocked(0.420f, 0.165f, 0.710f, 0.250f);
  const RegionColorStats priv_low = SampleRegionColorsLocked(0.450f, 0.615f, 0.690f, 0.652f);
  const RegionColorStats row2_l = SampleRegionColorsLocked(0.268f, 0.618f, 0.388f, 0.768f);
  const RegionColorStats row2_mid = SampleRegionColorsLocked(0.350f, 0.620f, 0.780f, 0.760f);
  const RegionColorStats bot_valley = SampleRegionColorsLocked(0.450f, 0.700f, 0.680f, 0.820f);
  const RegionColorStats row2_r = SampleRegionColorsLocked(0.788f, 0.618f, 0.908f, 0.768f);
  return (btn1.btn_frac() >= 0.20f) && (btn1_r.btn_frac() >= 0.15f) &&
         (peg_gap.btn_frac() >= 0.18f || peg_gap.ui_frac() >= 0.35f) &&
         (btn2.btn_frac() >= 0.20f) && (top_gap.btn_frac() < 0.12f) &&
         (priv_low.btn_frac() < 0.14f) && (row2_l.btn_frac() < 0.12f) &&
         (row2_mid.btn_frac() < 0.14f) && (bot_valley.btn_frac() < 0.12f) &&
         (row2_r.btn_frac() < 0.12f);
}

bool LooksLikeMultiplayerMenuLocked() {
  static constexpr float kBandY0[4] = {0.275f, 0.445f, 0.612f, 0.782f};
  static constexpr float kBandY1[4] = {0.312f, 0.482f, 0.648f, 0.818f};
  int hits = 0;
  for (int i = 0; i < 4; ++i) {
    if (SampleRegionColorsLocked(0.450f, kBandY0[i], 0.690f, kBandY1[i]).btn_frac() >= 0.22f) {
      ++hits;
    }
  }
  const RegionColorStats priv_low = SampleRegionColorsLocked(0.450f, 0.612f, 0.690f, 0.648f);
  const RegionColorStats left_bg = SampleRegionColorsLocked(0.280f, 0.250f, 0.390f, 0.600f);
  const RegionColorStats row2_l = SampleRegionColorsLocked(0.268f, 0.625f, 0.380f, 0.740f);
  const RegionColorStats row2_r = SampleRegionColorsLocked(0.790f, 0.625f, 0.900f, 0.740f);
  return (hits >= 3) && (priv_low.btn_frac() >= 0.22f) && (left_bg.btn_frac() < 0.20f) &&
         (row2_l.btn_frac() < 0.12f) && (row2_r.btn_frac() < 0.12f);
}

bool LooksLikeMainMenuLocked() {
  const RegionColorStats sp = SampleRegionColorsLocked(0.420f, 0.215f, 0.720f, 0.298f);
  const RegionColorStats mp = SampleRegionColorsLocked(0.445f, 0.370f, 0.750f, 0.482f);
  const RegionColorStats gap_mp_r2 = SampleRegionColorsLocked(0.450f, 0.502f, 0.700f, 0.556f);
  int row2_hits = 0;
  for (size_t i = 2; i <= 6; ++i) {
    const auto& b = kMainMenuBoxes[i];
    const RegionColorStats bs =
        SampleRegionColorsLocked(b.x0 + 0.005f, 0.625f, b.x1 - 0.005f, 0.712f);
    if (bs.btn_frac() >= 0.08f || bs.ui_frac() >= 0.15f) {
      ++row2_hits;
    }
  }
  const RegionColorStats r0 = SampleRegionColorsLocked(0.275f, 0.625f, 0.375f, 0.712f);
  const RegionColorStats r4 = SampleRegionColorsLocked(0.790f, 0.625f, 0.890f, 0.712f);
  return (sp.ui_frac() >= 0.25f) && (sp.btn_frac() >= 0.04f) &&
         (mp.ui_frac() >= 0.25f) && (mp.btn_frac() >= 0.04f) &&
         (gap_mp_r2.btn_frac() < 0.15f) && (row2_hits >= 4) &&
         (r0.btn_frac() >= 0.10f) && (r4.btn_frac() >= 0.10f);
}

bool LooksLikeHelpOptionsLocked() {
  if (DetectModalDialogLocked() != 0 || LooksLikeSettingsMenuLocked()) {
    return false;
  }
  const RegionColorStats cloud_l = SampleRegionColorsLocked(0.18f, 0.92f, 0.38f, 0.98f);
  const RegionColorStats cloud_r = SampleRegionColorsLocked(0.62f, 0.92f, 0.82f, 0.98f);
  if (cloud_l.avg_r > 145.0f && cloud_l.avg_b > 150.0f &&
      cloud_r.avg_r > 145.0f && cloud_r.avg_b > 150.0f) {
    return false;  // Title Screen bottom pink clouds
  }
  int hits = 0;
  bool has_gold = false;
  for (const auto& b : kHelpOptionsBoxes) {
    const RegionColorStats bs = SampleRegionColorsLocked(b.x0, b.y0, b.x1, b.y1);
    if (bs.btn_frac() >= 0.15f) ++hits;
    if (bs.gold_frac >= 0.12f) has_gold = true;
  }
  const RegionColorStats ribbon_l = SampleRegionColorsLocked(0.305f, 0.14f, 0.350f, 0.72f);
  const RegionColorStats ribbon_bot = SampleRegionColorsLocked(0.320f, 0.73f, 0.570f, 0.82f);
  const RegionColorStats bot_acc = SampleRegionColorsLocked(0.415f, 0.770f, 0.585f, 0.848f);
  const RegionColorStats right_clear = SampleRegionColorsLocked(0.640f, 0.365f, 0.890f, 0.750f);
  return (hits >= 4) && has_gold && (ribbon_l.modal_blue_frac >= 0.35f) &&
         (ribbon_bot.modal_blue_frac >= 0.50f) && (bot_acc.btn_frac() < 0.15f) &&
         (right_clear.btn_frac() < 0.10f);
}

RexScreenId ClassifyPageFromPixelsLocked(const PageFingerprint& fp) {
  if (IsFullScreenFadeLocked() || IsWhiteSplashCornersLocked()) {
    return SCREEN_UNKNOWN;
  }
  if (s_current_screen == SCREEN_BOOT) {
    return LooksLikeTitleScreenLocked() ? SCREEN_TITLE : SCREEN_BOOT;
  }
  if (IsInGameplay() &&
      (!IsPauseOverlayTreeLocked() ||
       (!s_transition_pending && !HasPauseMenuStackLocked() &&
        g_gameplay_tick_count.load(std::memory_order_relaxed) > s_paused_turret_tick_count + 2))) {
    return SCREEN_GAMEPLAY;
  }
  if (DetectModalDialogLocked() != 0) {
    return SCREEN_MODAL_OK_POPUP;
  }
  if (!fp.has_bottom_bar) {
    if (LooksLikeSettingsMenuLocked()) {
      return SCREEN_SETTINGS_SUBMENU;
    }
    if (LooksLikeHelpOptionsLocked() &&
        !(s_current_screen == SCREEN_HELP_OPTIONS && s_help_parent_screen == SCREEN_PAUSE_MENU &&
          s_pending_action_mask != 0)) {
      return SCREEN_HELP_OPTIONS;
    }
    if (LooksLikeTitleScreenLocked()) {
      return SCREEN_TITLE;
    }
    if ((s_current_screen == SCREEN_GAMEPLAY || s_current_screen == SCREEN_PAUSE_MENU ||
         (s_current_screen == SCREEN_HELP_OPTIONS && s_help_parent_screen == SCREEN_PAUSE_MENU)) &&
        HasPauseMenuStackLocked()) {
      return SCREEN_PAUSE_MENU;
    }
    const bool can_be_gameplay_now =
        (s_current_screen == SCREEN_LEVEL_SELECT || s_current_screen == SCREEN_MASTER_SELECT ||
         s_current_screen == SCREEN_PAUSE_MENU || s_current_screen == SCREEN_GAMEPLAY ||
         (!s_transition_pending && s_current_screen != SCREEN_INFO_SUBMENU &&
          s_current_screen != SCREEN_HELP_OPTIONS && s_current_screen != SCREEN_SETTINGS_SUBMENU &&
          s_current_screen != SCREEN_MAIN_MENU && s_current_screen != SCREEN_BOOT));
    if (can_be_gameplay_now && !fp.is_bottom_bar_dimmed && s_current_screen != SCREEN_TITLE) {
      const RegionColorStats full = SampleRegionColorsLocked(0.05f, 0.05f, 0.95f, 0.95f);
      if (full.avg_luma >= 24.0f) {
        return SCREEN_GAMEPLAY;
      }
    }
    return SCREEN_UNKNOWN;
  }

  if (fp.has_costume_btn) {
    return fp.has_green_p2_pillar ? SCREEN_MP_LOCAL_DUEL : SCREEN_MASTER_SELECT;
  }
  if (fp.has_level_board) {
    return SCREEN_LEVEL_SELECT;
  }
  if (fp.has_adventure_sign) {
    return SCREEN_ADVENTURE_CAROUSEL;
  }
  if (LooksLikeSettingsMenuLocked()) {
    return SCREEN_SETTINGS_SUBMENU;
  }
  if (LooksLikeMainMenuLocked()) {
    return SCREEN_MAIN_MENU;
  }
  if (LooksLikeMpModeSelectLocked()) {
    return SCREEN_MP_MODE_SELECT;
  }
  if (LooksLikeMultiplayerMenuLocked()) {
    return SCREEN_MULTIPLAYER_MENU;
  }
  if (LooksLikeHelpOptionsLocked()) {
    return SCREEN_HELP_OPTIONS;
  }
  return SCREEN_UNKNOWN;
}

void AppendDynamicAdventureBoxesLocked() {
  // Peggle Institute's left card is solid bright UI-blue, whereas Mountain Highs has Berg's
  // ice-blue card glass/arm in [0.17..0.22]. Checking btn_blue_frac avoids misclassifying
  // Mountain Highs as Peggle Institute and shifting LEVELS onto Berg's card / TRIALS onto LEVELS.
  const RegionColorStats inst_probe = SampleRegionColorsLocked(0.170f, 0.470f, 0.220f, 0.670f);
  const float blue_or_gold_ratio = inst_probe.gold_frac + inst_probe.btn_blue_frac;
  const RegionColorStats right_card = SampleRegionColorsLocked(0.60f, 0.45f, 0.92f, 0.70f);
  const bool no_right_card =
      (right_card.ui_frac() < 0.08f && right_card.modal_blue_frac < 0.10f);

  bool is_peggle_institute = (s_adv_world_idx == 0);
  if (blue_or_gold_ratio > 0.45f) {
    is_peggle_institute = true;
    s_adv_world_idx = 0;
  } else if (blue_or_gold_ratio < 0.15f) {
    is_peggle_institute = false;
    if (s_adv_world_idx == 0) s_adv_world_idx = 1;
  }

  if (is_peggle_institute) {
    AppendCanonicalBoxesLocked(kAdventurePeggleInstituteBoxes,
                               std::size(kAdventurePeggleInstituteBoxes));
  } else if (no_right_card || s_adv_world_idx >= 5) {
    s_adv_world_idx = 5;
    AppendCanonicalBoxesLocked(kAdventureCelestialBoxes, std::size(kAdventureCelestialBoxes));
  } else {
    AppendCanonicalBoxesLocked(kAdventureBoxes, std::size(kAdventureBoxes));
  }
}

RexScreenId ResolvePendingTransitionLocked(bool has_pixels) {
  const std::string_view tgt(s_pending_target_name);
  const bool is_a = (s_pending_action_mask & 0x1000) != 0;
  const bool is_esc_or_start = (s_pending_action_mask & 0x0010) != 0;
  const bool is_back_or_b = (tgt == "BACK (B)") || ((s_pending_action_mask & 0x2000) != 0);
  const bool modal_ok = has_pixels && IsModalOkVisibleLocked();
  const PageFingerprint fp = has_pixels ? ComputePageFingerprintLocked() : PageFingerprint{};

  switch (s_pending_source_screen) {
    case SCREEN_TITLE:
      if (tgt == "PLAY_BTN" || tgt == "PLAY" || is_a || is_esc_or_start) return SCREEN_MAIN_MENU;
      return SCREEN_TITLE;

    case SCREEN_MAIN_MENU:
      if (tgt == "SINGLE PLAYER") return SCREEN_ADVENTURE_CAROUSEL;
      if (tgt == "MULTIPLAYER") {
        if (modal_ok) {
          s_prev_screen = SCREEN_MAIN_MENU;
          return SCREEN_MODAL_OK_POPUP;
        }
        return SCREEN_MULTIPLAYER_MENU;
      }
      if (tgt == "MENU BTN 3") {
        s_help_parent_screen = SCREEN_MAIN_MENU;
        return SCREEN_HELP_OPTIONS;
      }
      if (tgt == "MENU BTN 0" || tgt == "MENU BTN 1" || tgt == "MENU BTN 2") {
        if (modal_ok) {
          s_prev_screen = SCREEN_MAIN_MENU;
          return SCREEN_MODAL_OK_POPUP;
        }
        return SCREEN_MAIN_MENU;
      }
      if (is_back_or_b || is_esc_or_start) return SCREEN_TITLE;
      return SCREEN_MAIN_MENU;

    case SCREEN_ADVENTURE_CAROUSEL:
      if (is_back_or_b) return SCREEN_MAIN_MENU;
      if (tgt == "ADV_PREV_WORLD" || tgt == "ADV_NEXT_WORLD") return SCREEN_ADVENTURE_CAROUSEL;
      if (tgt == "ADV_LEVELS") {
        if (modal_ok) {
          s_prev_screen = SCREEN_ADVENTURE_CAROUSEL;
          return SCREEN_MODAL_OK_POPUP;
        }
        if (s_pending_target_gold > 0.03f || (has_pixels && HasLevelSelectLandmarksLocked())) {
          return SCREEN_LEVEL_SELECT;
        }
        return SCREEN_ADVENTURE_CAROUSEL;
      }
      if (tgt == "ADV_TRIALS") {
        if (modal_ok) {
          s_prev_screen = SCREEN_ADVENTURE_CAROUSEL;
          return SCREEN_MODAL_OK_POPUP;
        }
        if (has_pixels && HasLevelSelectLandmarksLocked()) return SCREEN_LEVEL_SELECT;
        return SCREEN_ADVENTURE_CAROUSEL;
      }
      if (modal_ok && is_a) {
        s_prev_screen = SCREEN_ADVENTURE_CAROUSEL;
        return SCREEN_MODAL_OK_POPUP;
      }
      return SCREEN_ADVENTURE_CAROUSEL;

    case SCREEN_MULTIPLAYER_MENU:
      if (is_back_or_b) return SCREEN_MAIN_MENU;
      if (modal_ok && is_a) {
        s_prev_screen = SCREEN_MULTIPLAYER_MENU;
        return SCREEN_MODAL_OK_POPUP;
      }
      if (tgt == "MP_QUICK_MATCH") return SCREEN_MP_MODE_SELECT;
      if (tgt == "MP_LOCAL_DUEL") return SCREEN_MP_LOCAL_DUEL;
      return SCREEN_MULTIPLAYER_MENU;

    case SCREEN_MP_MODE_SELECT:
      if (is_back_or_b) return SCREEN_MULTIPLAYER_MENU;
      if (modal_ok && is_a) {
        s_prev_screen = SCREEN_MP_MODE_SELECT;
        return SCREEN_MODAL_OK_POPUP;
      }
      return SCREEN_MP_MODE_SELECT;

    case SCREEN_MP_LOCAL_DUEL:
      if (has_pixels) {
        if (const int mk = DetectModalDialogLocked(); mk != 0) {
          return EnterModalPopupLocked(SCREEN_MP_LOCAL_DUEL, mk);
        }
      }
      return SCREEN_MP_LOCAL_DUEL;

    case SCREEN_LEVEL_SELECT:
      if (is_back_or_b) return SCREEN_ADVENTURE_CAROUSEL;
      if (is_a || tgt == "PLAY_PREVIEW" || tgt.rfind("HEX_", 0) == 0) {
        if (modal_ok) {
          s_prev_screen = SCREEN_LEVEL_SELECT;
          return SCREEN_MODAL_OK_POPUP;
        }
        if (has_pixels && !fp.has_bottom_bar && !fp.is_bottom_bar_dimmed &&
            !IsFullScreenFadeLocked()) {
          return SCREEN_GAMEPLAY;
        }
        if (!has_pixels || (fp.has_bottom_bar && fp.has_costume_btn)) {
          return SCREEN_MASTER_SELECT;
        }
      }
      return SCREEN_LEVEL_SELECT;

    case SCREEN_MASTER_SELECT:
      if (is_back_or_b) return SCREEN_LEVEL_SELECT;
      if (tgt == "CHANGE_COSTUME (Y)" || (s_pending_action_mask & 0x8000) != 0) {
        if (modal_ok) {
          s_prev_screen = SCREEN_MASTER_SELECT;
          return SCREEN_MODAL_OK_POPUP;
        }
        return SCREEN_MASTER_SELECT;
      }
      if (is_a || tgt == "MASTER_RANDOM" || tgt.rfind("MASTER_", 0) == 0) {
        if (modal_ok) {
          s_prev_screen = SCREEN_MASTER_SELECT;
          return SCREEN_MODAL_OK_POPUP;
        }
        return SCREEN_GAMEPLAY;
      }
      return SCREEN_MASTER_SELECT;

    case SCREEN_HELP_OPTIONS:
      if (is_back_or_b || tgt == "OPT_DONE") return s_help_parent_screen;
      if (tgt == "OPT_SETTINGS") {
        s_prev_screen = SCREEN_HELP_OPTIONS;
        return SCREEN_SETTINGS_SUBMENU;
      }
      if (tgt == "OPT_HOW_TO_PLAY" || tgt == "OPT_CREDITS") {
        s_prev_screen = SCREEN_HELP_OPTIONS;
        return SCREEN_INFO_SUBMENU;
      }
      return SCREEN_HELP_OPTIONS;

    case SCREEN_SETTINGS_SUBMENU:
      if (is_back_or_b || tgt == "SET_ACCEPT") return SCREEN_HELP_OPTIONS;
      return SCREEN_SETTINGS_SUBMENU;

    case SCREEN_INFO_SUBMENU:
      if (is_back_or_b) return SCREEN_HELP_OPTIONS;
      return SCREEN_INFO_SUBMENU;

    case SCREEN_MODAL_OK_POPUP:
      if (s_modal_is_yesno) {
        if (tgt == "MODAL_OK_A" || tgt == "OK_BTN" || is_a) {
          s_modal_is_yesno = false;
          return s_modal_yes_screen;
        }
        if (tgt == "MODAL_CANCEL_B" || tgt == "CANCEL (B)" || is_back_or_b || is_esc_or_start) {
          s_modal_is_yesno = false;
          return s_prev_screen;
        }
        return SCREEN_MODAL_OK_POPUP;
      }
      if (tgt == "OK_BTN" || is_a || is_back_or_b || is_esc_or_start) return s_prev_screen;
      return SCREEN_MODAL_OK_POPUP;

    case SCREEN_GAMEPLAY:
      if (is_esc_or_start && has_pixels && HasPauseMenuStackLocked()) {
        return SCREEN_PAUSE_MENU;
      }
      return SCREEN_GAMEPLAY;

    case SCREEN_PAUSE_MENU:
      if (tgt == "PAUSE_RESUME" || is_back_or_b || is_esc_or_start) {
        return SCREEN_GAMEPLAY;
      }
      if (tgt == "PAUSE_RESTART") {
        if (modal_ok) {
          s_prev_screen = SCREEN_PAUSE_MENU;
          return SCREEN_MODAL_OK_POPUP;
        }
        return SCREEN_GAMEPLAY;
      }
      if (tgt == "PAUSE_OPTIONS") {
        s_help_parent_screen = SCREEN_PAUSE_MENU;
        return SCREEN_HELP_OPTIONS;
      }
      if (tgt == "PAUSE_QUIT") {
        if (modal_ok) {
          s_prev_screen = SCREEN_PAUSE_MENU;
          return SCREEN_MODAL_OK_POPUP;
        }
        return SCREEN_LEVEL_SELECT;
      }
      return SCREEN_PAUSE_MENU;

    default:
      return s_current_screen;
  }
}

void BuildAndScoreCandidatesLocked(const RexGpuUiMonitorSnapshotMirror& gpu, uint64_t now_ms,
                                   bool defer_audit = false) {
  g_sw_candidates.clear();
  g_sw_status.has_highlight = false;
  g_sw_status.hl_row = -1;
  g_sw_status.hl_col = -1;
  g_sw_status.hl_gold_frac = 0.0f;
  g_sw_status.hl_name[0] = '\0';

  const bool has_pixels = HasActivePixelFrameLocked();

  if (has_pixels) {
    const PageFingerprint curr_fp = ComputePageFingerprintLocked();
    const uint64_t seq = g_sw_curr_frame.sequence;
    if (s_fp_last_seq == ~0ull) {
      s_fp_baseline = curr_fp;
      s_fp_last_seq = seq;
    } else if (seq != s_fp_last_seq) {
      if (curr_fp != s_fp_baseline) {
        if (curr_fp == s_fp_pending) {
          if (s_fp_change_streak < 255) ++s_fp_change_streak;
        } else {
          s_fp_pending = curr_fp;
          s_fp_change_streak = 1;
        }
        if (s_fp_change_streak >= 2) {
          s_page_watch_active = true;
          if (!s_transition_pending) {
            s_pending_action_mask = 0;
            s_page_watch_used_fallback = false;
          }
          s_page_watch_started_ms = now_ms;
          s_page_watch_until_ms = now_ms + 1500ull;
          s_page_watch_source = s_current_screen;
          s_fp_baseline = curr_fp;
          s_fp_change_streak = 0;
        }
      } else {
        s_fp_change_streak = 0;
      }
      s_fp_last_seq = seq;
    }

    const bool gameplay_ticking = IsInGameplay();
    if (IsPauseOverlayTreeLocked()) {
      if (gameplay_ticking && !s_transition_pending && !HasPauseMenuStackLocked() &&
          g_gameplay_tick_count.load(std::memory_order_relaxed) > s_paused_turret_tick_count + 2) {
        s_transition_pending = false;
        s_page_watch_active = false;
        SetLockedScreenLocked(SCREEN_GAMEPLAY);
        s_fp_baseline = curr_fp;
      }
    } else if (gameplay_ticking) {
      if (s_current_screen != SCREEN_GAMEPLAY) {
        s_transition_pending = false;
        s_page_watch_active = false;
        SetLockedScreenLocked(SCREEN_GAMEPLAY);
        s_fp_baseline = curr_fp;
      }
    } else if (s_current_screen == SCREEN_BOOT) {
      if (LooksLikeTitleScreenLocked()) {
        s_page_watch_active = false;
        SetLockedScreenLocked(SCREEN_TITLE);
        s_fp_baseline = curr_fp;
      }
    } else if (s_current_screen == SCREEN_GAMEPLAY && s_last_gameplay_start_ms != 0 &&
               now_ms >= s_last_gameplay_start_ms && (now_ms - s_last_gameplay_start_ms) <= 1500 &&
               HasPauseMenuStackLocked()) {
      s_transition_pending = false;
      s_page_watch_active = false;
      SetLockedScreenLocked(SCREEN_PAUSE_MENU);
      s_fp_baseline = curr_fp;
    }

    // Rule B: Post-Click & Structural Page-Change Verification (1.5s watch window; wait while Motion > 12%).
    const bool settled =
        (g_sw_status.motion_pct <= 12.0f) && !IsFullScreenFadeLocked() &&
        !IsWhiteSplashCornersLocked();
    if (s_page_watch_active && !gameplay_ticking) {
      if (!settled) {
        // Extend watch window during camera pans / fades (capped at 3.5s) so transition settles cleanly.
        const uint64_t ext = std::min(s_page_watch_started_ms + 3500ull, now_ms + 800ull);
        if (ext > s_page_watch_until_ms) {
          s_page_watch_until_ms = ext;
        }
      }
      if (settled && (!s_transition_pending || now_ms >= s_transition_settle_ms)) {
        const bool had_pending = s_transition_pending;
        const RexScreenId vis = ClassifyPageFromPixelsLocked(curr_fp);
        s_transition_pending = false;
        if (vis != SCREEN_UNKNOWN && vis != s_page_watch_source) {
          if (vis == SCREEN_MODAL_OK_POPUP) {
            SetLockedScreenLocked(EnterModalPopupLocked(s_current_screen, DetectModalDialogLocked()));
          } else {
            SetLockedScreenLocked(vis);
          }
          s_fp_baseline = curr_fp;
          s_page_watch_active = false;
        } else if (vis == s_page_watch_source) {
          if (now_ms >= s_page_watch_until_ms) {
            s_page_watch_active = false;
            s_fp_baseline = curr_fp;
          }
        } else {
          if (s_current_screen == SCREEN_MODAL_OK_POPUP && IsModalOkDismissedLocked()) {
            SetLockedScreenLocked(s_prev_screen);
            s_fp_baseline = curr_fp;
            s_page_watch_active = false;
          } else if ((had_pending || s_pending_action_mask != 0) && !s_page_watch_used_fallback) {
            s_page_watch_used_fallback = true;
            const RexScreenId fb = ResolvePendingTransitionLocked(true);
            SetLockedScreenLocked(fb);
            s_fp_baseline = curr_fp;
            if (fb != s_page_watch_source || now_ms >= s_page_watch_until_ms) {
              s_page_watch_active = false;
            }
          } else if (now_ms >= s_page_watch_until_ms) {
            s_page_watch_active = false;
            s_fp_baseline = curr_fp;
          }
        }
      } else if (now_ms >= s_page_watch_until_ms) {
        if (s_transition_pending) {
          SetLockedScreenLocked(ResolvePendingTransitionLocked(true));
          s_transition_pending = false;
        }
        s_page_watch_active = false;
        s_fp_baseline = curr_fp;
      } else if (s_transition_pending && now_ms >= s_transition_settle_ms &&
                 (!IsFullScreenFadeLocked() || now_ms >= s_transition_settle_ms + 550ull)) {
        // Animated backgrounds (e.g. Mountain Highs particles) can keep motion > 12% and extend
        // s_page_watch_until_ms up to 3.5s; unblock user input once settle window elapses.
        const RexScreenId vis = ClassifyPageFromPixelsLocked(curr_fp);
        if (vis != SCREEN_UNKNOWN && vis != s_page_watch_source) {
          SetLockedScreenLocked(
              (vis == SCREEN_MODAL_OK_POPUP)
                  ? EnterModalPopupLocked(s_current_screen, DetectModalDialogLocked())
                  : vis);
          s_page_watch_active = false;
        } else if (now_ms >= s_transition_settle_ms + 300ull) {
          SetLockedScreenLocked(ResolvePendingTransitionLocked(true));
        }
        s_transition_pending = false;
        s_fp_baseline = curr_fp;
      }
    } else if (s_transition_pending && now_ms >= s_transition_settle_ms) {
      SetLockedScreenLocked(ResolvePendingTransitionLocked(true));
      s_transition_pending = false;
      s_fp_baseline = curr_fp;
    }

    // Never allow MASTER_SELECT wireframes when bottom bar is missing on settled non-title/pause frames.
    if (!gameplay_ticking && s_current_screen == SCREEN_MASTER_SELECT && settled &&
        !curr_fp.has_bottom_bar && !curr_fp.is_bottom_bar_dimmed &&
        !LooksLikeTitleScreenLocked() && !HasPauseMenuStackLocked()) {
      s_transition_pending = false;
      s_page_watch_active = false;
      SetLockedScreenLocked(SCREEN_GAMEPLAY);
      s_fp_baseline = curr_fp;
    }

    // Any-tick delayed MODAL OK / Yes-No popup auto-enter & auto-exit (when not in active click watch).
    if (!gameplay_ticking && !s_transition_pending && s_current_screen != SCREEN_BOOT) {
      if (s_current_screen != SCREEN_MODAL_OK_POPUP) {
        if (const int mk = DetectModalDialogLocked(); mk != 0) {
          SetLockedScreenLocked(EnterModalPopupLocked(s_current_screen, mk));
          s_fp_baseline = curr_fp;
        }
      } else if (!s_page_watch_active && IsModalOkDismissedLocked()) {
        SetLockedScreenLocked(s_prev_screen);
        s_fp_baseline = curr_fp;
      }
    }

    // 2-consecutive-tick (~0.2s) Title Screen auto self-heal after idle reset / fade desync.
    if (!gameplay_ticking && !s_transition_pending && s_current_screen != SCREEN_TITLE &&
        s_current_screen != SCREEN_BOOT && s_current_screen != SCREEN_GAMEPLAY &&
        (s_last_title_exit_ms == 0 || now_ms >= s_last_title_exit_ms + 2500ull)) {
      if (LooksLikeTitleSelfHealLocked()) {
        if (seq != s_title_heal_last_seq) {
          if (s_title_heal_streak == 0) {
            s_title_heal_first_ms = now_ms;
          }
          ++s_title_heal_streak;
          s_title_heal_last_seq = seq;
          if (s_title_heal_streak >= 2 && (now_ms - s_title_heal_first_ms) >= 90) {
            SetLockedScreenLocked(SCREEN_TITLE);
            s_fp_baseline = curr_fp;
            s_title_heal_streak = 0;
          }
        }
      } else {
        s_title_heal_streak = 0;
      }
    } else {
      s_title_heal_streak = 0;
    }

    // 2. Emit hitboxes strictly for the locked active screen (ZERO layout flicker on mouse move).
    g_sw_status.screen_id = s_current_screen;
    std::snprintf(g_sw_status.screen_name, sizeof(g_sw_status.screen_name), "%s",
                  (s_current_screen == SCREEN_MODAL_OK_POPUP && s_modal_is_yesno)
                      ? "MODAL YES/NO"
                      : ScreenIdToName(s_current_screen));

    switch (s_current_screen) {
      case SCREEN_TITLE:
        if (LooksLikeTitleScreenLocked()) {
          AppendCanonicalBoxesLocked(kTitleBoxes, std::size(kTitleBoxes));
        }
        break;
      case SCREEN_MAIN_MENU: {
        AppendCanonicalBoxesLocked(kMainMenuBoxes, std::size(kMainMenuBoxes));
        // Sample Store icon (MENU BTN 2) below its teal top header border.
        const RegionColorStats store_lower =
            SampleRegionColorsLocked(0.528f, 0.640f, 0.648f, 0.768f);
        for (auto& c : g_sw_candidates) {
          if (c.row == 2 && c.col == 2) {
            c.gold_frac = store_lower.gold_frac;
            c.total_score = c.gold_frac * 100.0f;
          }
        }
        AppendFooterBoxesLocked();
        break;
      }
      case SCREEN_ADVENTURE_CAROUSEL:
        AppendDynamicAdventureBoxesLocked();
        AppendFooterBoxesLocked();
        break;
      case SCREEN_MULTIPLAYER_MENU:
        AppendCanonicalBoxesLocked(kMultiplayerMenuBoxes, std::size(kMultiplayerMenuBoxes));
        AppendFooterBoxesLocked();
        break;
      case SCREEN_MP_MODE_SELECT:
        AppendCanonicalBoxesLocked(kMpModeSelectBoxes, std::size(kMpModeSelectBoxes));
        AppendFooterBoxesLocked();
        break;
      case SCREEN_MP_LOCAL_DUEL:
        AppendCanonicalBoxesLocked(kMpLocalDuelBoxes, std::size(kMpLocalDuelBoxes));
        AppendFooterBoxesLocked(true);
        break;
      case SCREEN_LEVEL_SELECT: {
        AppendCanonicalBoxesLocked(kLevelSelectBoxes, std::size(kLevelSelectBoxes));
        g_sw_candidates.push_back(MakeScoredBoxLocked(
            0.235f, 0.425f, 0.765f, 0.765f, -1, -1, kCandImmediateA, "PLAY_PREVIEW"));
        AppendFooterBoxesLocked();
        break;
      }
      case SCREEN_MASTER_SELECT:
        AppendCanonicalBoxesLocked(kMasterSelectBoxes, std::size(kMasterSelectBoxes));
        AppendFooterBoxesLocked(true);
        break;
      case SCREEN_HELP_OPTIONS:
        AppendCanonicalBoxesLocked(kHelpOptionsBoxes, std::size(kHelpOptionsBoxes));
        AppendFooterBoxesLocked();
        break;
      case SCREEN_SETTINGS_SUBMENU:
        AppendCanonicalBoxesLocked(kSettingsRowBoxes, std::size(kSettingsRowBoxes));
        AppendFooterBoxesLocked();
        break;
      case SCREEN_INFO_SUBMENU:
        AppendFooterBoxesLocked();
        break;
      case SCREEN_MODAL_OK_POPUP:
        if (s_modal_is_yesno) {
          AppendCanonicalBoxesLocked(kModalYesNoBoxes, std::size(kModalYesNoBoxes));
        } else {
          AppendCanonicalBoxesLocked(kModalOkBoxes, std::size(kModalOkBoxes));
        }
        break;
      case SCREEN_PAUSE_MENU:
        AppendCanonicalBoxesLocked(kPauseMenuBoxes, std::size(kPauseMenuBoxes));
        break;
      case SCREEN_BOOT:
      case SCREEN_GAMEPLAY:
      default:
        break;
    }

    const int best_hl_idx = DetectHighlightIndexLocked(s_current_screen);
    if (best_hl_idx >= 0) {
      const auto& hl = g_sw_candidates[size_t(best_hl_idx)];
      g_sw_status.has_highlight = true;
      g_sw_status.hl_x0 = hl.x0;
      g_sw_status.hl_y0 = hl.y0;
      g_sw_status.hl_x1 = hl.x1;
      g_sw_status.hl_y1 = hl.y1;
      g_sw_status.hl_u = hl.cu;
      g_sw_status.hl_v = hl.cv;
      g_sw_status.hl_row = hl.row;
      g_sw_status.hl_col = hl.col;
      g_sw_status.hl_gold_frac = hl.gold_frac;
      if (hl.row >= 0 && hl.col >= 0) {
        g_sw_stepped_row = hl.row;
        g_sw_stepped_col = hl.col;
      }
      std::snprintf(g_sw_status.hl_name, sizeof(g_sw_status.hl_name), "%s", hl.name);
    }
    if (!defer_audit) {
      MaybeAuditStateLocked(now_ms, 0, false);
    }
    s_built_seq = g_sw_curr_frame.sequence;
    s_built_screen = s_current_screen;
    s_built_gameplay_ticking = gameplay_ticking;
    s_candidates_dirty = false;
    return;
  }

  // Fallback path for unit self-test synthetic GPU quad snapshots (--test_colliders).
  if (s_transition_pending && now_ms >= s_transition_settle_ms) {
    SetLockedScreenLocked(ResolvePendingTransitionLocked(true));
    s_transition_pending = false;
    s_page_watch_active = false;
  }
  if (!g_has_synthetic_snap) {
    g_sw_status.screen_id = s_current_screen;
    std::snprintf(g_sw_status.screen_name, sizeof(g_sw_status.screen_name), "%s",
                  ScreenIdToName(s_current_screen));
    return;
  }
  g_sw_status.screen_id = SCREEN_UNKNOWN;
  std::snprintf(g_sw_status.screen_name, sizeof(g_sw_status.screen_name), "SYNTHETIC");
  for (uint32_t i = 0; i < gpu.quad_count; ++i) {
    const auto& q = gpu.quads[i];
    if (!IsValidButtonSize(q.x1 - q.x0, q.y1 - q.y0)) continue;
    RexCandidateButton c{};
    c.x0 = q.x0;
    c.y0 = q.y0;
    c.x1 = q.x1;
    c.y1 = q.y1;
    c.cu = 0.5f * (q.x0 + q.x1);
    c.cv = 0.5f * (q.y0 + q.y1);
    c.row = -1;
    c.col = -1;
    c.flags = kCandNavigable;
    c.from_gpu = true;
    std::snprintf(c.name, sizeof(c.name), "QUAD %u", i);
    g_sw_candidates.push_back(c);
  }
  if (gpu.has_highlight) {
    g_sw_status.has_highlight = true;
    g_sw_status.hl_x0 = gpu.highlight_rect.x0;
    g_sw_status.hl_y0 = gpu.highlight_rect.y0;
    g_sw_status.hl_x1 = gpu.highlight_rect.x1;
    g_sw_status.hl_y1 = gpu.highlight_rect.y1;
    g_sw_status.hl_u = 0.5f * (gpu.highlight_rect.x0 + gpu.highlight_rect.x1);
    g_sw_status.hl_v = 0.5f * (gpu.highlight_rect.y0 + gpu.highlight_rect.y1);
  }
}

}  // namespace

void Init() {
  std::lock_guard<std::mutex> lock(g_mutex);
  EnsureLogOpenLocked();
  ResolveGpuProcs();
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_log_file) {
    std::fflush(g_log_file);
    std::fclose(g_log_file);
    g_log_file = nullptr;
  }
  if (g_audit_file) {
    std::fflush(g_audit_file);
    std::fclose(g_audit_file);
    g_audit_file = nullptr;
  }
}

void EnableUiLog(bool enable) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_ui_log_enabled = enable;
  if (enable) {
    EnsureLogOpenLocked();
  }
}

void LogLine(const char* category, const char* msg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  WriteLogLocked(category, msg);
}

void FlushLog() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_log_file) std::fflush(g_log_file);
  if (g_audit_file) std::fflush(g_audit_file);
}

void EnableReplayHitboxAudit(bool enable, const char* path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_audit_enabled = enable;
  if (path && path[0]) {
    g_audit_path = path;
  }
  if (enable) {
    if (g_audit_file) {
      std::fclose(g_audit_file);
      g_audit_file = nullptr;
    }
    g_audit_file = OpenStdFile(g_audit_path.c_str(), "w");
    if (g_audit_file) {
      std::fprintf(g_audit_file, "=== Peggle 2 Replay Hitbox Audit ===\n");
      std::fflush(g_audit_file);
    }
    g_audit_last_seq = ~0ull;
    g_audit_last_screen = SCREEN_UNKNOWN;
    g_audit_last_target.clear();
    g_audit_start_ms = NowMs();
  }
}

void FlushReplayHitboxAudit() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_audit_file) {
    std::fflush(g_audit_file);
  }
}

void ReportGuestWidget(uint32_t guest_ptr, uint32_t vtable, float x0, float y0, float x1,
                       float y1, bool has_bounds, bool is_focused, const char* label) {
  uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  RexGuestWidgetBox& box = g_guest_widgets[guest_ptr];
  box.guest_ptr = guest_ptr;
  box.vtable = vtable;
  box.x0 = x0;
  box.y0 = y0;
  box.x1 = x1;
  box.y1 = y1;
  box.has_bounds = has_bounds;
  box.is_focused = is_focused;
  box.last_seen_ms = now;
  if (label && label[0]) {
    std::snprintf(box.label, sizeof(box.label), "%s", label);
  } else if (!box.label[0]) {
    std::snprintf(box.label, sizeof(box.label), "vt_%08X", vtable);
  }
}

void RecordGuestInputCall(const char* func_name, uint32_t pc_hint, uint32_t r3, uint32_t r4,
                          uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r9,
                          uint32_t r10, double f1, double f2, double f3, double f4,
                          const uint8_t* guest_base, uint32_t dump_ea, uint16_t dpad_mask,
                          bool force_log) {
  uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string key = std::string(func_name ? func_name : "?") + "_" + std::to_string(r4);
  uint64_t& last = g_last_log_ms_by_key[key];
  if (!force_log && (now - last < 400)) {
    return;
  }
  last = now;

  char dump_buf[192]{};
  if (guest_base && dump_ea >= 0x10000000u && dump_ea < 0x8FFFF000u) {
    int pos = 0;
    for (int off = 0; off < 64 && pos + 10 < int(sizeof(dump_buf)); off += 4) {
      const uint8_t* p = guest_base + (dump_ea + uint32_t(off));
      uint32_t be_word = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      pos += std::snprintf(dump_buf + pos, sizeof(dump_buf) - pos, "%08X ", be_word);
    }
  }

  char line[640];
  std::snprintf(line, sizeof(line),
                "%s @0x%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X "
                "f=(%.3f,%.3f,%.3f,%.3f) dpad=%04X dump[0x%08X]=[%s]",
                func_name ? func_name : "sub_?", pc_hint, r3, r4, r5, r6, r7, r8, r9, r10,
                f1, f2, f3, f4, dpad_mask, dump_ea, dump_buf);
  WriteLogLocked("GUEST_INPUT", line);
}

void NotifyGameplayTurretTick(uint32_t gun_ptr, float cur_angle_rad, float new_angle_rad,
                              bool locked_by_mouse) {
  uint64_t now = NowMs();
  g_last_turret_tick_ms.store(now, std::memory_order_relaxed);
  g_gameplay_tick_count.fetch_add(1, std::memory_order_relaxed);
  g_last_turret_ptr.store(gun_ptr, std::memory_order_relaxed);
  float deg = new_angle_rad * (180.0f / 3.14159265f);
  g_last_turret_angle_deg.store(deg, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (std::fabs(deg - g_logged_turret_angle_deg) > 1.5f) {
    g_logged_turret_angle_deg = deg;
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "gun_ptr=0x%08X (+536/+540 BE dir) cur=%.2f deg -> write=%.2f deg (lock=%d)",
                  gun_ptr, cur_angle_rad * (180.0f / 3.14159265f), deg, locked_by_mouse ? 1 : 0);
    WriteLogLocked("TURRET_RAM", msg);
  }
}

bool IsInGameplay() {
  uint64_t last = g_last_turret_tick_ms.load(std::memory_order_relaxed);
  return last != 0 && (NowMs() - last) <= 100;
}

uint64_t GetGameplayFrameCounter() {
  return g_gameplay_tick_count.load(std::memory_order_relaxed);
}

uint32_t GetLastTurretPtr() { return g_last_turret_ptr.load(std::memory_order_relaxed); }

float GetLastTurretAngleDeg() { return g_last_turret_angle_deg.load(std::memory_order_relaxed); }

bool PollGpuSnapshot(RexGpuUiMonitorSnapshotMirror& out_snap) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_has_synthetic_snap) {
    out_snap = g_synthetic_snap;
    g_last_gpu_snap = out_snap;
    return true;
  }
  ResolveGpuProcs();
  if (!g_fn_get_snapshot) return false;
  g_fn_get_snapshot(&out_snap);
  g_last_gpu_snap = out_snap;

  uint64_t now = NowMs();
  std::string sig(out_snap.screen_signature);
  uint64_t& last_gpu_ms = g_last_log_ms_by_key["gpu_sig"];
  if ((sig != g_last_gpu_sig && (now - last_gpu_ms >= 350)) || (now - last_gpu_ms >= 3000)) {
    g_last_gpu_sig = sig;
    last_gpu_ms = now;
    std::string full = sig;
    if (out_snap.quad_count > 0) {
      full += " boxes=[";
      uint32_t show = std::min<uint32_t>(5, out_snap.quad_count);
      for (uint32_t i = 0; i < show; ++i) {
        const auto& q = out_snap.quads[i];
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s(%.2f,%.2f-%.2f,%.2f)", i ? " " : "", q.x0, q.y0, q.x1,
                      q.y1);
        full += buf;
      }
      full += "]";
    }
    WriteLogLocked("GPU_SNAP", full.c_str());
  }
  if (g_pending_post_dpad_mask != 0 && (now - g_last_dpad_log_ms >= 50)) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "AFTER DPAD 0x%04X -> %s", g_pending_post_dpad_mask,
                  out_snap.screen_signature);
    WriteLogLocked("DPAD_DELTA", msg);
    g_pending_post_dpad_mask = 0;
  }
  return out_snap.quad_count > 0 || out_snap.has_highlight;
}

void InjectSyntheticSnapshotForTest(const RexGpuUiMonitorSnapshotMirror& snap) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_has_synthetic_snap = true;
  g_has_synthetic_frame = false;
  g_synthetic_snap = snap;
  g_last_gpu_snap = snap;
}

void InjectSyntheticFrameForTest(const uint8_t* pixels_rgbl, uint32_t width, uint32_t height,
                                 uint64_t seq) {
  if (!pixels_rgbl || width == 0 || height == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_has_synthetic_snap = false;
  g_has_synthetic_frame = true;
  g_sw_prev_frame = std::move(g_sw_curr_frame);
  g_sw_curr_frame.width = width;
  g_sw_curr_frame.height = height;
  g_sw_curr_frame.sequence = seq;
  g_sw_curr_frame.timestamp_ms = NowMs();
  g_sw_curr_frame.pixels.assign(pixels_rgbl, pixels_rgbl + size_t(width) * size_t(height) * 4);
  g_sw_status.last_frame_seq = seq;
  g_sw_status.last_update_ms = g_sw_curr_frame.timestamp_ms;
  g_sw_status.last_hash_change_ms = g_sw_curr_frame.timestamp_ms;
  g_sw_status.has_pixel_frame = true;
  g_sw_status.motion_pct = 0.0f;
  g_sw_status.in_transition = false;
}

void ClearTestState() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_has_synthetic_snap = false;
  g_has_synthetic_frame = false;
  g_synthetic_snap = {};
  g_last_gpu_snap = {};
  g_last_servo = {};
  g_guest_widgets.clear();
  g_sw_prev_frame = {};
  g_sw_curr_frame = {};
  g_sw_candidates.clear();
  g_sw_status = {};
  g_last_turret_tick_ms.store(0, std::memory_order_relaxed);
  g_gameplay_tick_count.store(0, std::memory_order_relaxed);
  s_current_screen = SCREEN_TITLE;
  g_atomic_screen_id.store(uint8_t(SCREEN_TITLE), std::memory_order_relaxed);
  s_prev_screen = SCREEN_MAIN_MENU;
  s_help_parent_screen = SCREEN_MAIN_MENU;
  s_modal_yes_screen = SCREEN_MAIN_MENU;
  s_modal_is_yesno = false;
  s_transition_pending = false;
  s_transition_settle_ms = 0;
  s_pending_source_screen = SCREEN_TITLE;
  s_pending_target_name[0] = '\0';
  s_pending_target_gold = 0.0f;
  s_pending_action_mask = 0;
  s_page_watch_active = false;
  s_page_watch_used_fallback = false;
  s_page_watch_started_ms = 0;
  s_page_watch_until_ms = 0;
  s_page_watch_source = SCREEN_TITLE;
  s_fp_baseline = {};
  s_fp_pending = {};
  s_fp_change_streak = 0;
  s_fp_last_seq = ~0ull;
  s_paused_turret_tick_count = 0;
  s_last_gameplay_start_ms = 0;
  s_last_title_exit_ms = 0;
  g_last_step_now_ms = 0;
  g_last_servo_dpad_ms = 0;
  g_last_carousel_scroll_ms = 0;
  g_last_carousel_scroll_dir = 0;
  g_carousel_hover_side = 0;
  g_carousel_pending_steps = 0;
  g_carousel_pending_dir = 0;
  g_carousel_next_pulse_ms = 0;
  g_carousel_next_scroll_ms = 0;
  s_adv_world_idx = 0;
  s_level_idx = 0;
  s_master_row = 0;
  s_master_col = 0;
  s_lobby_row = 0;
  s_lobby_col = 0;
  s_settings_row = 0;
  s_pause_row = 0;
  rex::ui::Presenter::SetScreenWatcherFastCadence(false);
  g_prev_classified_screen = SCREEN_TITLE;
  g_sw_stepped_row = 0;
  g_sw_stepped_col = 0;
  g_sw_has_stepped_focus = false;
  g_pending_click_until_ms = 0;
  g_sw_has_cursor_pos = false;
  g_sw_prev_pulse_valid = false;
  g_sw_used_orthogonal = false;
  g_sw_last_pulse_frame_seq = 0;
  g_sw_last_pulse_axis = 0;
  g_sw_last_dpad_mask = 0;
  g_sw_last_dpad_time_ms = 0;
  g_sw_last_pulse_frame_seq = 0;
  g_sw_pre_pulse_row = -1;
  g_sw_pre_pulse_col = -1;
  s_title_heal_streak = 0;
  s_title_heal_last_seq = ~0ull;
  s_title_heal_first_ms = 0;
  g_audit_last_seq = ~0ull;
  g_audit_last_screen = SCREEN_UNKNOWN;
  g_audit_last_target.clear();
}

void SetScreenForTest(RexScreenId screen_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  s_transition_pending = false;
  s_page_watch_active = false;
  s_fp_change_streak = 0;
  SetLockedScreenLocked(screen_id);
  if (HasActivePixelFrameLocked()) {
    s_fp_baseline = ComputePageFingerprintLocked();
    s_fp_last_seq = g_sw_curr_frame.sequence;
  }
}

RexScreenId GetCurrentScreenId() {
  return RexScreenId(g_atomic_screen_id.load(std::memory_order_relaxed));
}

void NotifyMenuActionInput(uint16_t action_mask, uint64_t now_ms) {
  const uint16_t relevant = action_mask & (0x1000 | 0x2000 | 0x0010 | 0x8000);
  if (relevant == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_has_synthetic_frame && now_ms > g_last_step_now_ms) {
    g_last_step_now_ms = now_ms;
  }
  BuildAndScoreCandidatesLocked(g_last_gpu_snap, now_ms);
  const bool frame_fresh =
      HasActivePixelFrameLocked() &&
      (g_has_synthetic_frame || g_sw_status.last_update_ms == 0 ||
       now_ms < g_sw_status.last_update_ms ||
       (now_ms - g_sw_status.last_update_ms) <= 1000ull);
  if ((s_transition_pending && now_ms < s_transition_settle_ms + 300ull) ||
      (frame_fresh && IsFullScreenFadeLocked())) {
    return;  // Ignore rapid double-clicks / fade clicks until 0.45s settle completes
  }
  if (s_current_screen == SCREEN_TITLE && g_sw_candidates.empty()) {
    return;  // Never skip boot splash before PLAY_BTN appears
  }
  const char* tgt_name = "";
  float tgt_gold = 0.0f;
  if ((relevant & 0x1000) != 0) {
    if (s_current_screen != SCREEN_GAMEPLAY) {
      if (g_sw_candidates.empty()) return;
      if (g_sw_status.has_highlight && g_sw_status.hl_name[0]) {
        tgt_name = g_sw_status.hl_name;
        tgt_gold = g_sw_status.hl_gold_frac;
      } else if (s_current_screen == SCREEN_MODAL_OK_POPUP) {
        tgt_name = "OK_BTN";
        tgt_gold = 1.0f;
      } else if ((relevant & (0x2000 | 0x0010 | 0x8000)) == 0) {
        return;
      }
    }
  }
  ArmMenuTransitionLocked(relevant, tgt_name, tgt_gold, now_ms);
}

void NotifyHostDpadSent(uint16_t dpad_mask) {
  uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  ResolveGpuProcs();
  if (g_fn_notify_dpad) {
    g_fn_notify_dpad(dpad_mask, now);
  }
  if (now - g_last_dpad_log_ms >= 80) {
    g_last_dpad_log_ms = now;
    g_pending_post_dpad_mask = dpad_mask;
    char msg[256];
    std::snprintf(msg, sizeof(msg), "BEFORE DPAD 0x%04X -> %s", dpad_mask,
                  g_last_gpu_snap.screen_signature);
    WriteLogLocked("DPAD_DELTA", msg);
  }
}

void UpdateCursorSample(float norm_x, float norm_y) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sw_last_cursor_u = norm_x;
  g_sw_last_cursor_v = norm_y;
  g_sw_has_cursor_pos = true;
  SampleCursorRgbLocked(norm_x, norm_y);
}

void RefreshCandidatesIfStaleLocked(uint64_t now_ms) {
  UpdateScreenWatcherFrameLocked(now_ms);
  const bool cur_gp = IsInGameplay();
  const bool cache_valid =
      !g_has_synthetic_snap && !g_has_synthetic_frame && !s_candidates_dirty &&
      !s_transition_pending && !s_page_watch_active &&
      s_built_seq != ~0ull && g_sw_curr_frame.sequence == s_built_seq &&
      s_current_screen == s_built_screen && cur_gp == s_built_gameplay_ticking;
  if (!cache_valid) {
    BuildAndScoreCandidatesLocked(g_last_gpu_snap,
                                  g_has_synthetic_frame ? g_last_step_now_ms : now_ms);
  }
}

void GetSnapshotForOverlay(RexGpuUiMonitorSnapshotMirror& out_gpu,
                           std::vector<RexGuestWidgetBox>& out_guest_widgets,
                           RexMonitorServoOutput& out_last_servo) {
  uint64_t now = NowMs();
  PollGpuSnapshot(out_gpu);
  std::lock_guard<std::mutex> lock(g_mutex);
  RefreshCandidatesIfStaleLocked(now);
  out_gpu = g_last_gpu_snap;
  out_last_servo = g_last_servo;
  out_guest_widgets.clear();
  for (auto it = g_guest_widgets.begin(); it != g_guest_widgets.end();) {
    if (now - it->second.last_seen_ms > 200) {
      it = g_guest_widgets.erase(it);
    } else {
      out_guest_widgets.push_back(it->second);
      ++it;
    }
  }
}

void GetScreenWatcherState(RexScreenWatcherStatus& out_status,
                           std::vector<RexCandidateButton>& out_candidates) {
  uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  RefreshCandidatesIfStaleLocked(now);
  out_status = g_sw_status;
  out_candidates = g_sw_candidates;
}

RexMonitorServoOutput StepMenuFocusServoLocked(const RexGpuUiMonitorSnapshotMirror& gpu,
                                               float norm_x, float norm_y, bool allow_hover_servo,
                                               bool click_trigger, bool is_touch_tap,
                                               uint64_t now_ms) {
  g_last_step_now_ms = now_ms;
  g_sw_last_cursor_u = norm_x;
  g_sw_last_cursor_v = norm_y;
  g_sw_has_cursor_pos = true;
  UpdateScreenWatcherFrameLocked(now_ms);
  SampleCursorRgbLocked(norm_x, norm_y);
  BuildAndScoreCandidatesLocked(gpu, now_ms, true);
  const bool frame_fresh =
      HasActivePixelFrameLocked() &&
      (g_has_synthetic_frame || g_sw_status.last_update_ms == 0 ||
       now_ms < g_sw_status.last_update_ms ||
       (now_ms - g_sw_status.last_update_ms) <= 1000ull);
  const bool transition_blocking =
      s_transition_pending && (now_ms < s_transition_settle_ms + 300ull);
  const bool input_blocked =
      transition_blocking || (frame_fresh && IsFullScreenFadeLocked());
  const bool allow_hover_eff = allow_hover_servo && !input_blocked;
  const bool click_eff = click_trigger && !input_blocked;

  RexMonitorServoOutput out{};

  if (input_blocked) {
    g_pending_click_until_ms = 0;
  } else if (click_eff) {
    g_pending_click_until_ms = now_ms + (is_touch_tap ? 1800 : 1800);
    g_pending_click_x = norm_x;
    g_pending_click_y = norm_y;
  } else if (now_ms <= g_pending_click_until_ms &&
             std::hypotf(norm_x - g_pending_click_x, norm_y - g_pending_click_y) > 0.04f) {
    g_pending_click_until_ms = 0;
  }
  const bool pending_click = !input_blocked && (now_ms <= g_pending_click_until_ms);
  const float qx = pending_click ? g_pending_click_x : norm_x;
  const float qy = pending_click ? g_pending_click_y : norm_y;

  // 1. Identify HoveredButton under (qx, qy): smallest-area CandidateButton (exact menu rects).
  int tgt_idx = -1;
  float best_area = 1e9f;
  for (size_t i = 0; i < g_sw_candidates.size(); ++i) {
    const auto& c = g_sw_candidates[i];
    if (PointInRect(qx, qy, c.x0, c.y0, c.x1, c.y1)) {
      const float area = RectArea(c.x0, c.y0, c.x1, c.y1);
      if (area < best_area) {
        best_area = area;
        tgt_idx = int(i);
      }
    }
  }

  g_sw_status.has_target = false;
  g_sw_status.target_u = -1.0f;
  g_sw_status.target_v = -1.0f;
  std::snprintf(g_sw_status.target_name, sizeof(g_sw_status.target_name), "None");
  std::snprintf(out.target_name, sizeof(out.target_name), "None");

  if (tgt_idx >= 0) {
    const auto& c = g_sw_candidates[size_t(tgt_idx)];
    out.has_target = true;
    out.target_x0 = c.x0;
    out.target_y0 = c.y0;
    out.target_x1 = c.x1;
    out.target_y1 = c.y1;
    std::snprintf(out.target_name, sizeof(out.target_name), "%s", c.name);
    g_sw_status.has_target = true;
    g_sw_status.target_x0 = c.x0;
    g_sw_status.target_y0 = c.y0;
    g_sw_status.target_x1 = c.x1;
    g_sw_status.target_y1 = c.y1;
    g_sw_status.target_u = c.cu;
    g_sw_status.target_v = c.cv;
    std::snprintf(g_sw_status.target_name, sizeof(g_sw_status.target_name), "%s", c.name);
  }

  // 2. Populate ActiveHighlightBox from Yellow/Gold (> 4%) detection.
  out.has_highlight = g_sw_status.has_highlight;
  out.hl_x0 = g_sw_status.hl_x0;
  out.hl_y0 = g_sw_status.hl_y0;
  out.hl_x1 = g_sw_status.hl_x1;
  out.hl_y1 = g_sw_status.hl_y1;

  // Reset carousel continuous scroll state when cursor leaves side zones or changes screen.
  if (g_sw_status.screen_id != SCREEN_ADVENTURE_CAROUSEL || tgt_idx < 0 ||
      !(g_sw_candidates[size_t(tgt_idx)].flags &
        (kCandCarouselScrollLeft | kCandCarouselScrollRight))) {
    g_carousel_hover_side = 0;
    g_carousel_pending_steps = 0;
    g_carousel_pending_dir = 0;
    g_carousel_next_pulse_ms = 0;
    g_carousel_next_scroll_ms = 0;
  }

  if (tgt_idx < 0) {
    g_sw_status.aligned = false;
    out.aligned = false;
    g_pending_click_until_ms = 0;
    g_sw_prev_pulse_valid = false;
    const bool fade_locked = HasActivePixelFrameLocked() && IsFullScreenFadeLocked();
    if ((g_sw_status.screen_id == SCREEN_GAMEPLAY && click_eff) ||
        (is_touch_tap && click_trigger && !fade_locked)) {
      out.buttons_or_mask |= 0x1000;  // XINPUT_GAMEPAD_A advances intro speech bubbles / dialogue
    }
    g_last_servo = out;
    return out;
  }

  const auto& tgt = g_sw_candidates[size_t(tgt_idx)];

  // Direct footer / modal / shoulder buttons (BACK B, CHANGE COSTUME Y, SELECT A, RB, PLAY_PREVIEW / OK_BTN A).
  if (tgt.flags & kCandBackBtn) {
    out.aligned = true;
    g_sw_status.aligned = true;
    if (click_eff) {
      out.buttons_or_mask |= 0x2000;  // XINPUT_GAMEPAD_B
      g_pending_click_until_ms = 0;
      ArmMenuTransitionLocked(0x2000, tgt.name, tgt.gold_frac, now_ms);
    }
    g_last_servo = out;
    return out;
  }
  if (tgt.flags & kCandCostumeBtn) {
    out.aligned = true;
    g_sw_status.aligned = true;
    if (click_eff) {
      out.buttons_or_mask |= 0x8000;  // XINPUT_GAMEPAD_Y
      g_pending_click_until_ms = 0;
      ArmMenuTransitionLocked(0x8000, tgt.name, tgt.gold_frac, now_ms);
    }
    g_last_servo = out;
    return out;
  }
  if (tgt.flags & kCandRightShoulder) {
    out.aligned = true;
    g_sw_status.aligned = true;
    if (click_eff) {
      out.buttons_or_mask |= 0x0200;  // XINPUT_GAMEPAD_RIGHT_SHOULDER (RB)
      g_pending_click_until_ms = 0;
      ArmMenuTransitionLocked(0x0200, tgt.name, tgt.gold_frac, now_ms);
    }
    g_last_servo = out;
    return out;
  }
  if (tgt.flags & (kCandSelectBtn | kCandImmediateA)) {
    out.aligned = true;
    g_sw_status.aligned = true;
    if (click_eff) {
      out.buttons_or_mask |= 0x1000;  // XINPUT_GAMEPAD_A
      g_pending_click_until_ms = 0;
      const bool use_hl = (tgt.flags & kCandSelectBtn) && g_sw_status.has_highlight &&
                          g_sw_status.hl_name[0];
      ArmMenuTransitionLocked(0x1000, use_hl ? g_sw_status.hl_name : tgt.name,
                              use_hl ? g_sw_status.hl_gold_frac : tgt.gold_frac, now_ms);
    }
    g_last_servo = out;
    return out;
  }

  // Continuous 0.55s multi-character scrolling on Adventure Carousel side cards + immediate click step.
  if (tgt.flags & (kCandCarouselScrollLeft | kCandCarouselScrollRight)) {
    const int8_t dir = (tgt.flags & kCandCarouselScrollRight) ? 1 : -1;
    const uint16_t dpad_bit = (dir > 0) ? 0x0008 : 0x0004;
    out.aligned = false;
    g_sw_status.aligned = false;
    if (g_carousel_hover_side != dir) {
      g_carousel_hover_side = dir;
      g_carousel_pending_steps = 0;
      g_carousel_pending_dir = 0;
      g_carousel_next_pulse_ms = 0;
      g_carousel_next_scroll_ms = 0;
    }
    if (g_carousel_pending_steps > 0 && now_ms >= g_carousel_next_pulse_ms && !input_blocked) {
      out.buttons_or_mask |= dpad_bit;
      --g_carousel_pending_steps;
      g_last_carousel_scroll_ms = now_ms;
      g_last_carousel_scroll_dir = g_carousel_pending_dir;
      g_last_servo_dpad_ms = now_ms;
      g_carousel_next_scroll_ms = now_ms + 650;
      s_adv_world_idx = std::clamp(s_adv_world_idx + int(g_carousel_pending_dir), 0, 5);
      g_sw_stepped_col = (g_carousel_pending_dir > 0) ? 0 : 1;
      g_sw_status.last_dpad_mask = dpad_bit;
      g_pending_click_until_ms = 0;
    } else if (g_carousel_pending_steps == 0 && !input_blocked && g_sw_status.motion_pct < 25.0f) {
      const bool click_ready =
          click_eff &&
          (g_last_carousel_scroll_ms == 0 || now_ms - g_last_carousel_scroll_ms >= 200);
      const uint64_t hover_cd = (g_last_carousel_scroll_dir == dir) ? 650ull : 150ull;
      const bool hover_ready =
          allow_hover_eff && (now_ms >= g_carousel_next_scroll_ms) &&
          (g_last_carousel_scroll_ms == 0 || now_ms - g_last_carousel_scroll_ms >= hover_cd ||
           g_carousel_next_scroll_ms != 0);
      if (click_ready || hover_ready) {
        const bool need_two_steps =
            (dir > 0) ? (g_sw_status.hl_col <= 0) : (g_sw_status.hl_col == 1);
        out.buttons_or_mask |= dpad_bit;
        g_last_servo_dpad_ms = now_ms;
        if (need_two_steps) {
          g_carousel_pending_steps = 1;
          g_carousel_pending_dir = dir;
          g_carousel_next_pulse_ms = now_ms + 60;
          g_sw_stepped_col = (dir > 0) ? 1 : 0;
        } else {
          g_carousel_pending_steps = 0;
          g_last_carousel_scroll_ms = now_ms;
          g_last_carousel_scroll_dir = dir;
          g_carousel_next_scroll_ms = now_ms + 650;
          s_adv_world_idx = std::clamp(s_adv_world_idx + int(dir), 0, 5);
          g_sw_stepped_col = (dir > 0) ? 0 : 1;
        }
        g_sw_status.last_dpad_mask = dpad_bit;
        g_pending_click_until_ms = 0;
      }
    }
    g_last_servo = out;
    return out;
  }

  const float target_u = g_sw_status.target_u;
  const float target_v = g_sw_status.target_v;
  const bool grid_aligned =
      (out.has_highlight && tgt.row >= 0 && tgt.col >= 0 &&
       tgt.row == g_sw_status.hl_row && tgt.col == g_sw_status.hl_col);
  const bool pos_aligned =
      (out.has_highlight &&
       std::hypotf(target_u - g_sw_status.hl_u, target_v - g_sw_status.hl_v) < 0.04f);

  if (!out.has_highlight) {
    out.aligned = (tgt.gold_frac > 0.18f);
    g_sw_status.aligned = out.aligned;
    if (click_eff) {
      out.buttons_or_mask |= 0x1000;  // XINPUT_GAMEPAD_A over verified button
      out.aligned = true;
      g_sw_status.aligned = true;
      g_pending_click_until_ms = 0;
      ArmMenuTransitionLocked(0x1000, tgt.name, tgt.gold_frac, now_ms);
    }
    g_last_servo = out;
    return out;
  }

  const float hl_u = g_sw_status.hl_u;
  const float hl_v = g_sw_status.hl_v;
  out.dx = target_u - hl_u;
  out.dy = target_v - hl_v;

  out.aligned = grid_aligned || pos_aligned;
  g_sw_status.aligned = out.aligned;

  const float eff_motion_pct = frame_fresh ? g_sw_status.motion_pct : 0.0f;
  const bool eff_in_transition = frame_fresh && g_sw_status.in_transition;

  if (out.aligned ||
      (!frame_fresh && pending_click &&
       !(tgt.flags & (kCandCarouselScrollLeft | kCandCarouselScrollRight)))) {
    g_sw_prev_pulse_valid = false;
    g_sw_used_orthogonal = false;
    if (pending_click) {
      if (tgt.flags & kCandSettingsRow) {
        // Slider track spans [0.50..0.745] (midpoint 0.6225): left half + label -> DPAD_LEFT, right half -> DPAD_RIGHT.
        out.buttons_or_mask |= (qx < 0.6225f) ? 0x0004 : 0x0008;
      } else {
        out.buttons_or_mask |= 0x1000;  // XINPUT_GAMEPAD_A
        out.aligned = true;
        g_sw_status.aligned = true;
      }
      const bool in_place_toggle =
          (g_sw_status.screen_id == SCREEN_SETTINGS_SUBMENU && tgt.row == 0) ||
          (g_sw_status.screen_id == SCREEN_MP_LOCAL_DUEL && tgt.row >= 1);
      if ((out.buttons_or_mask & 0x1000) && !(tgt.flags & kCandSettingsRow) && !in_place_toggle) {
        ArmMenuTransitionLocked(0x1000, tgt.name, tgt.gold_frac, now_ms);
      }
      g_pending_click_until_ms = 0;
    }
  } else if (!input_blocked && !eff_in_transition && eff_motion_pct < 25.0f &&
             (allow_hover_eff || pending_click)) {
    const bool fast_deterministic =
        (g_sw_status.screen_id == SCREEN_LEVEL_SELECT ||
         g_sw_status.screen_id == SCREEN_PAUSE_MENU);
    const uint64_t base_cd =
        (!fast_deterministic && HasActivePixelFrameLocked() && !g_has_synthetic_frame) ? 190ull
                                                                                       : 95ull;
    const uint64_t dpad_cooldown_ms =
        (g_sw_status.screen_id == SCREEN_ADVENTURE_CAROUSEL) ? 220ull : base_cd;
    const bool waiting_new_frame =
        frame_fresh && !fast_deterministic && HasActivePixelFrameLocked() &&
        g_sw_last_pulse_frame_seq != 0 && g_sw_curr_frame.sequence == g_sw_last_pulse_frame_seq &&
        (now_ms - g_last_servo_dpad_ms < 260ull);
    if ((now_ms - g_last_servo_dpad_ms < dpad_cooldown_ms) || waiting_new_frame) {
      g_last_servo = out;
      return out;
    }
    if (g_sw_status.screen_id == SCREEN_LEVEL_SELECT && IsLevelHexLockedPadlockLocked(tgt)) {
      g_pending_click_until_ms = 0;
      g_last_servo = out;
      return out;  // Locked grey padlock hex
    }
    if (std::hypotf(target_u - g_sw_last_target_u, target_v - g_sw_last_target_v) > 0.03f) {
      g_sw_prev_pulse_valid = false;
      g_sw_used_orthogonal = false;
    }

    int drow = 0, dcol = 0;
    bool use_grid = (tgt.row >= 0 && tgt.col >= 0 && g_sw_status.hl_row >= 0 &&
                     g_sw_status.hl_col >= 0);
    if (use_grid) {
      drow = int(tgt.row) - int(g_sw_status.hl_row);
      dcol = int(tgt.col) - int(g_sw_status.hl_col);
    }

    const bool lock_row2_horizontal =
        (g_sw_status.screen_id == SCREEN_MAIN_MENU && g_sw_status.hl_row == 2 && tgt.row == 2);
    const bool strict_grid_screen =
        lock_row2_horizontal || (g_sw_status.screen_id == SCREEN_MASTER_SELECT) ||
        (g_sw_status.screen_id == SCREEN_MP_LOCAL_DUEL) ||
        (g_sw_status.screen_id == SCREEN_LEVEL_SELECT) ||
        (g_sw_status.screen_id == SCREEN_PAUSE_MENU);

    uint8_t axis = use_grid ? ((std::abs(dcol) >= std::abs(drow)) ? 1 : 2)
                            : ((std::fabs(out.dx) >= std::fabs(out.dy)) ? 1 : 2);
    if (lock_row2_horizontal || g_sw_status.screen_id == SCREEN_LEVEL_SELECT) {
      axis = 1;  // Strictly horizontal on MAIN_MENU row 2 and zig-zag HEX_1..10
    } else if (g_sw_status.screen_id == SCREEN_PAUSE_MENU) {
      axis = 2;  // Strictly vertical across PAUSE_RESUME..PAUSE_QUIT (rows 0..3)
    } else if (g_sw_status.screen_id == SCREEN_MASTER_SELECT) {
      if (g_sw_status.hl_row == 1 && tgt.row == 0) {
        axis = 2;  // Exit MASTER_RANDOM upward onto Row 0 (col 2)
      } else if (g_sw_status.hl_row == 0 && tgt.row == 0) {
        axis = 1;  // Strictly horizontal across MASTER_0..4
      } else if (g_sw_status.hl_row == 0 && tgt.row == 1) {
        axis = (g_sw_status.hl_col == 2) ? 2 : 1;  // Center on col 2 before DPAD_DOWN
      }
    } else if (g_sw_status.screen_id == SCREEN_MP_LOCAL_DUEL) {
      axis = (g_sw_status.hl_row != tgt.row) ? 2 : 1;
    } else if (g_sw_status.screen_id == SCREEN_MAIN_MENU && g_sw_status.hl_row < 2 && tgt.row == 2) {
      axis = 2;  // Step down onto row 2 before moving horizontally across row-2 icons
    }
    const bool new_frame_arrived =
        (!g_sw_status.has_pixel_frame) || (g_sw_curr_frame.sequence > g_sw_last_pulse_frame_seq);
    if (!strict_grid_screen && g_sw_prev_pulse_valid && new_frame_arrived) {
      const bool moved_grid =
          use_grid && (g_sw_status.hl_row != g_sw_pre_pulse_row || g_sw_status.hl_col != g_sw_pre_pulse_col);
      const float moved_uv = std::hypotf(hl_u - g_sw_pre_pulse_hl_u, hl_v - g_sw_pre_pulse_hl_v);
      const bool moved = use_grid ? moved_grid : (moved_uv >= 0.01f);
      if (!moved && !g_sw_used_orthogonal) {
        axis = (g_sw_last_pulse_axis == 1) ? 2 : 1;
        g_sw_used_orthogonal = true;
      } else if (moved) {
        g_sw_used_orthogonal = false;
      }
    }

    uint16_t dpad = 0;
    if (lock_row2_horizontal || (g_sw_status.screen_id == SCREEN_LEVEL_SELECT && axis == 1)) {
      dpad = (dcol > 0) ? 0x0008 : 0x0004;
    } else if (g_sw_status.screen_id == SCREEN_LEVEL_SELECT && axis == 2) {
      dpad = (out.dy >= 0.0f) ? 0x0002 : 0x0001;
    } else if (use_grid) {
      if (axis == 1) {
        dpad = (dcol != 0) ? ((dcol > 0) ? 0x0008 : 0x0004)
                           : ((drow >= 0) ? 0x0002 : 0x0001);
      } else {
        dpad = (drow != 0) ? ((drow > 0) ? 0x0002 : 0x0001)
                           : ((dcol >= 0) ? 0x0008 : 0x0004);
      }
    } else {
      dpad = (axis == 1) ? ((out.dx >= 0.0f) ? 0x0008 : 0x0004)
                         : ((out.dy >= 0.0f) ? 0x0002 : 0x0001);
    }

    // Update fallback stepped grid index for screens using tracked focus.
    if (g_sw_status.screen_id == SCREEN_LEVEL_SELECT) {
      if (dcol > 0) {
        s_level_idx = std::min<int8_t>(9, g_sw_status.hl_col + 1);
      } else if (dcol < 0) {
        s_level_idx = std::max<int8_t>(0, g_sw_status.hl_col - 1);
      }
      g_sw_stepped_col = s_level_idx;
    } else if (g_sw_status.screen_id == SCREEN_PAUSE_MENU) {
      if (dpad == 0x0002) s_pause_row = std::min<int8_t>(3, g_sw_status.hl_row + 1);
      else if (dpad == 0x0001) s_pause_row = std::max<int8_t>(0, g_sw_status.hl_row - 1);
      g_sw_stepped_row = s_pause_row;
    } else if (g_sw_status.screen_id == SCREEN_MASTER_SELECT) {
      if (dpad == 0x0008 && g_sw_status.hl_row == 0) {
        s_master_row = 0;
        s_master_col = std::min<int8_t>(4, g_sw_status.hl_col + 1);
      } else if (dpad == 0x0004 && g_sw_status.hl_row == 0) {
        s_master_row = 0;
        s_master_col = std::max<int8_t>(0, g_sw_status.hl_col - 1);
      } else if (dpad == 0x0002 && g_sw_status.hl_row == 0) {
        s_master_row = 1;
        s_master_col = 2;
      } else if (dpad == 0x0001 && g_sw_status.hl_row == 1) {
        s_master_row = 0;
        s_master_col = 2;
      }
      g_sw_stepped_row = s_master_row;
      g_sw_stepped_col = s_master_col;
    } else if (g_sw_status.screen_id == SCREEN_MP_LOCAL_DUEL) {
      if (dpad == 0x0002) {
        s_lobby_row = std::min<int8_t>(3, g_sw_status.hl_row + 1);
        s_lobby_col = 0;
      } else if (dpad == 0x0001) {
        s_lobby_row = std::max<int8_t>(0, g_sw_status.hl_row - 1);
        if (s_lobby_row > 0) s_lobby_col = 0;
      } else if (dpad == 0x0008 && g_sw_status.hl_row == 0) {
        s_lobby_col = std::min<int8_t>(5, g_sw_status.hl_col + 1);
      } else if (dpad == 0x0004 && g_sw_status.hl_row == 0) {
        s_lobby_col = std::max<int8_t>(0, g_sw_status.hl_col - 1);
      }
      g_sw_stepped_row = s_lobby_row;
      g_sw_stepped_col = s_lobby_col;
    } else if (g_sw_status.screen_id == SCREEN_SETTINGS_SUBMENU) {
      if (dpad == 0x0002) s_settings_row = std::min<int8_t>(4, g_sw_status.hl_row + 1);
      else if (dpad == 0x0001) s_settings_row = std::max<int8_t>(0, g_sw_status.hl_row - 1);
      g_sw_stepped_row = s_settings_row;
    } else if (g_sw_status.screen_id == SCREEN_ADVENTURE_CAROUSEL) {
      if (dpad == 0x0008) g_sw_stepped_col = 1;
      else if (dpad == 0x0004) g_sw_stepped_col = 0;
    }

    out.buttons_or_mask |= dpad;
    g_sw_prev_pulse_valid = true;
    g_sw_last_pulse_axis = axis;
    g_sw_last_dpad_mask = dpad;
    g_sw_last_dpad_time_ms = now_ms;
    g_sw_last_pulse_frame_seq = g_sw_curr_frame.sequence;
    g_sw_pre_pulse_row = g_sw_status.hl_row;
    g_sw_pre_pulse_col = g_sw_status.hl_col;
    g_sw_pre_pulse_hl_u = hl_u;
    g_sw_pre_pulse_hl_v = hl_v;
    g_sw_last_target_u = target_u;
    g_sw_last_target_v = target_v;
    g_sw_status.last_dpad_mask = dpad;
    g_last_servo_dpad_ms = now_ms;
  }

  if (out.buttons_or_mask != 0) {
    s_candidates_dirty = true;
  }
  g_last_servo = out;
  return out;
}

RexMonitorServoOutput StepMenuFocusServo(float norm_x, float norm_y, bool allow_hover_servo,
                                         bool click_trigger, bool is_touch_tap,
                                         uint64_t now_ms) {
  RexGpuUiMonitorSnapshotMirror gpu{};
  PollGpuSnapshot(gpu);

  std::lock_guard<std::mutex> lock(g_mutex);
  RexMonitorServoOutput out = StepMenuFocusServoLocked(
      gpu, norm_x, norm_y, allow_hover_servo, click_trigger, is_touch_tap, now_ms);
  MaybeAuditStateLocked(now_ms, out.buttons_or_mask, true);
  return out;
}

bool SaveScreenWatcherPng(const char* path, RexScreenWatcherStatus* out_status,
                          size_t* out_cand_count) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t now = NowMs();
  UpdateScreenWatcherFrameLocked(now);
  BuildAndScoreCandidatesLocked(g_last_gpu_snap, g_has_synthetic_frame ? g_last_step_now_ms : now);
  if (out_status) *out_status = g_sw_status;
  if (out_cand_count) *out_cand_count = g_sw_candidates.size();

  const uint32_t W = g_sw_curr_frame.width;
  const uint32_t H = g_sw_curr_frame.height;
  if (!path || !path[0] || W == 0 || H == 0 ||
      g_sw_curr_frame.pixels.size() < size_t(W) * H * 4) {
    return false;
  }

  const uint32_t scale = (W <= 480) ? 2u : 1u;
  const uint32_t W2 = W * scale;
  const uint32_t H2 = H * scale;
  std::vector<uint8_t> rgb(size_t(W2) * H2 * 3);
  for (uint32_t y = 0; y < H2; ++y) {
    const uint32_t sy = y / scale;
    for (uint32_t x = 0; x < W2; ++x) {
      const uint32_t sx = x / scale;
      const size_t src = (size_t(sy) * W + sx) * 4;
      const size_t dst = (size_t(y) * W2 + x) * 3;
      rgb[dst + 0] = g_sw_curr_frame.pixels[src + 0];
      rgb[dst + 1] = g_sw_curr_frame.pixels[src + 1];
      rgb[dst + 2] = g_sw_curr_frame.pixels[src + 2];
    }
  }

  auto put_px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= int(W2) || y >= int(H2)) return;
    const size_t dst = (size_t(y) * W2 + size_t(x)) * 3;
    rgb[dst + 0] = r;
    rgb[dst + 1] = g;
    rgb[dst + 2] = b;
  };
  auto draw_box = [&](float x0, float y0, float x1, float y1, int thick,
                      uint8_t r, uint8_t g, uint8_t b) {
    const int px0 = std::clamp(int(x0 * float(W2)), 0, int(W2) - 1);
    const int py0 = std::clamp(int(y0 * float(H2)), 0, int(H2) - 1);
    const int px1 = std::clamp(int(x1 * float(W2)), 0, int(W2) - 1);
    const int py1 = std::clamp(int(y1 * float(H2)), 0, int(H2) - 1);
    for (int t = 0; t < thick; ++t) {
      for (int x = px0; x <= px1; ++x) {
        put_px(x, py0 + t, r, g, b);
        put_px(x, py1 - t, r, g, b);
      }
      for (int y = py0; y <= py1; ++y) {
        put_px(px0 + t, y, r, g, b);
        put_px(px1 - t, y, r, g, b);
      }
    }
  };
  for (const auto& c : g_sw_candidates) {
    draw_box(c.x0, c.y0, c.x1, c.y1, 1, 0, 235, 245);
  }
  if (g_sw_status.has_highlight) {
    draw_box(g_sw_status.hl_x0, g_sw_status.hl_y0, g_sw_status.hl_x1, g_sw_status.hl_y1,
             2, 255, 215, 0);
  }
  if (g_sw_status.has_target) {
    draw_box(g_sw_status.target_x0, g_sw_status.target_y0, g_sw_status.target_x1,
             g_sw_status.target_y1, 2, 255, 60, 220);
  }
  if (g_sw_has_cursor_pos) {
    const int cx = int(std::clamp(g_sw_last_cursor_u, 0.0f, 0.999f) * float(W2));
    const int cy = int(std::clamp(g_sw_last_cursor_v, 0.0f, 0.999f) * float(H2));
    for (int d = -6; d <= 6; ++d) {
      put_px(cx + d, cy, 255, 50, 50);
      put_px(cx, cy + d, 255, 50, 50);
    }
  }

  FILE* f = OpenStdFile(path, "wb");
  if (!f) return false;

  auto write_be32 = [&](uint32_t v) {
    uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    std::fwrite(b, 1, 4, f);
  };
  auto crc32_update = [](uint32_t crc, const uint8_t* data, size_t len) -> uint32_t {
    for (size_t i = 0; i < len; ++i) {
      crc ^= data[i];
      for (int k = 0; k < 8; ++k) {
        crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
      }
    }
    return crc;
  };
  auto write_chunk = [&](const char tag[4], const uint8_t* data, size_t len) {
    write_be32(uint32_t(len));
    std::fwrite(tag, 1, 4, f);
    if (len > 0) std::fwrite(data, 1, len, f);
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, reinterpret_cast<const uint8_t*>(tag), 4);
    if (len > 0) crc = crc32_update(crc, data, len);
    write_be32(crc ^ 0xFFFFFFFFu);
  };

  static constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(kPngSig, 1, 8, f);

  uint8_t ihdr[13] = {
      uint8_t(W2 >> 24), uint8_t(W2 >> 16), uint8_t(W2 >> 8), uint8_t(W2),
      uint8_t(H2 >> 24), uint8_t(H2 >> 16), uint8_t(H2 >> 8), uint8_t(H2),
      8, 2, 0, 0, 0};
  write_chunk("IHDR", ihdr, 13);

  const size_t stride = size_t(W2) * 3 + 1;
  std::vector<uint8_t> raw(stride * H2);
  uint32_t adler_s1 = 1, adler_s2 = 0;
  for (uint32_t y = 0; y < H2; ++y) {
    uint8_t* row = raw.data() + size_t(y) * stride;
    row[0] = 0;
    std::memcpy(row + 1, rgb.data() + size_t(y) * W2 * 3, size_t(W2) * 3);
    for (size_t i = 0; i < stride; ++i) {
      adler_s1 = (adler_s1 + row[i]) % 65521u;
      adler_s2 = (adler_s2 + adler_s1) % 65521u;
    }
  }

  std::vector<uint8_t> zbuf;
  zbuf.reserve(raw.size() + (raw.size() / 65535u + 1u) * 5u + 6u);
  zbuf.push_back(0x78);
  zbuf.push_back(0x01);
  for (size_t off = 0; off < raw.size();) {
    const size_t blen = std::min<size_t>(65535u, raw.size() - off);
    const bool is_last = (off + blen == raw.size());
    const uint16_t len16 = uint16_t(blen);
    const uint16_t nlen16 = uint16_t(~len16);
    zbuf.push_back(is_last ? 1 : 0);
    zbuf.push_back(uint8_t(len16 & 0xFF));
    zbuf.push_back(uint8_t(len16 >> 8));
    zbuf.push_back(uint8_t(nlen16 & 0xFF));
    zbuf.push_back(uint8_t(nlen16 >> 8));
    zbuf.insert(zbuf.end(), raw.data() + off, raw.data() + off + blen);
    off += blen;
  }
  const uint32_t adler = (adler_s2 << 16) | adler_s1;
  zbuf.push_back(uint8_t(adler >> 24));
  zbuf.push_back(uint8_t(adler >> 16));
  zbuf.push_back(uint8_t(adler >> 8));
  zbuf.push_back(uint8_t(adler));
  write_chunk("IDAT", zbuf.data(), zbuf.size());
  write_chunk("IEND", nullptr, 0);
  std::fclose(f);
  return true;
}

bool MeasureVisualTurretAngleFromPixels(const uint8_t* pixels_rgbl, uint32_t width, uint32_t height,
                                        float& out_angle_rad, uint32_t* out_dot_count) {
  if (out_dot_count) *out_dot_count = 0;
  if (!pixels_rgbl || width == 0 || height == 0) return false;
  const uint32_t u0 = uint32_t(0.34f * float(width));
  const uint32_t u1 = std::min(width, uint32_t(0.82f * float(width)));
  const uint32_t v0 = uint32_t(0.08f * float(height));
  const uint32_t v1 = std::min(height, uint32_t(0.32f * float(height)));
  const float inv_w = 1.0f / float(width);
  const float inv_h = 1.0f / float(height);
  double sum_dx = 0.0, sum_dy = 0.0;
  uint32_t dot_count = 0;
  for (uint32_t sy = v0; sy < v1; ++sy) {
    const float v = (float(sy) + 0.5f) * inv_h;
    const float dy = v * 720.0f - 45.0f;
    for (uint32_t sx = u0; sx < u1; ++sx) {
      const size_t off = (size_t(sy) * width + sx) * 4;
      const uint8_t r = pixels_rgbl[off + 0];
      const uint8_t g = pixels_rgbl[off + 1];
      const uint8_t b = pixels_rgbl[off + 2];
      const uint8_t luma = pixels_rgbl[off + 3];
      if (r > 165 && b > 200 && luma > 155 && int(b) > int(g) + 18 && int(r) > int(g) + 10) {
        const float u = (float(sx) + 0.5f) * inv_w;
        const float dx = u * 1280.0f - 741.0f;
        const float dist_norm = std::hypotf(dx, dy) / 720.0f;
        if (dist_norm >= 0.05f && dist_norm <= 0.24f) {
          sum_dx += double(dx);
          sum_dy += double(dy);
          ++dot_count;
        }
      }
    }
  }
  if (out_dot_count) *out_dot_count = dot_count;
  const uint32_t min_dots = (width >= 640) ? 4u : 2u;
  if (dot_count < min_dots || sum_dy <= 1e-3) return false;
  out_angle_rad = std::clamp(float(std::atan2(sum_dx, sum_dy)), -1.48f, 1.48f);
  return true;
}

bool MeasureLatestVisualTurretAngle(float& out_angle_rad, uint32_t* out_dot_count) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!HasActivePixelFrameLocked()) {
    if (out_dot_count) *out_dot_count = 0;
    return false;
  }
  return MeasureVisualTurretAngleFromPixels(g_sw_curr_frame.pixels.data(),
                                            g_sw_curr_frame.width, g_sw_curr_frame.height,
                                            out_angle_rad, out_dot_count);
}

}  // namespace rex_monitor
