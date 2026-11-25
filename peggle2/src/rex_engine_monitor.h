#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RexGpuUiQuadMirror {
  float x0, y0, x1, y1;
  uint64_t vs_hash;
  uint64_t ps_hash;
  uint32_t texture_mask;
  uint32_t color_hash;
  uint16_t prim_count;
  uint8_t flags;
  uint8_t reserved;
  float score;
};

struct RexGpuUiMonitorSnapshotMirror {
  uint64_t frame_index;
  uint32_t quad_count;
  uint32_t raw_quad_count;
  uint8_t has_highlight;
  uint8_t reserved[3];
  RexGpuUiQuadMirror highlight_rect;
  RexGpuUiQuadMirror quads[128];
  char screen_signature[128];
};

struct RexGuestWidgetBox {
  uint32_t guest_ptr;
  uint32_t vtable;
  float x0, y0, x1, y1;
  bool has_bounds;
  bool is_focused;
  char label[48];
  uint64_t last_seen_ms;
};

enum RexScreenId : uint8_t {
  SCREEN_UNKNOWN = 0,
  SCREEN_GAMEPLAY = 1,
  SCREEN_TITLE = 2,
  SCREEN_MAIN_MENU = 3,
  SCREEN_ADVENTURE_CAROUSEL = 4,
  SCREEN_ADVENTURE = SCREEN_ADVENTURE_CAROUSEL,
  SCREEN_LEVEL_SELECT = 5,
  SCREEN_MASTER_SELECT = 6,
  SCREEN_MODAL_OK_POPUP = 7,
  SCREEN_PAUSE_MENU = 8,
  SCREEN_HELP_OPTIONS = 9,
  SCREEN_MULTIPLAYER_MENU = 10,
  SCREEN_SETTINGS_SUBMENU = 11,
  SCREEN_BOOT = 12,
  SCREEN_INFO_SUBMENU = 13,
  SCREEN_MP_LOCAL_DUEL = 14,
  SCREEN_MP_LOCAL_LOBBY = SCREEN_MP_LOCAL_DUEL,
  SCREEN_MP_MODE_SELECT = 15,
  SCREEN_MODAL_YES_NO = SCREEN_MODAL_OK_POPUP,
  SCREEN_SETTINGS_MENU = SCREEN_SETTINGS_SUBMENU,
};

enum RexCandidateFlags : uint16_t {
  kCandNavigable = 1 << 0,
  kCandBackBtn = 1 << 1,
  kCandSelectBtn = 1 << 2,
  kCandCarouselScrollLeft = 1 << 3,
  kCandCarouselScrollRight = 1 << 4,
  kCandCostumeBtn = 1 << 5,
  kCandImmediateA = 1 << 6,
  kCandSettingsRow = 1 << 7,
  kCandRightShoulder = 1 << 8,
};

struct RexCandidateButton {
  float x0, y0, x1, y1;
  float cu, cv;
  float pulse_score;
  float border_score;
  float dpad_score;
  float focus_score;
  float total_score;
  float gold_frac;
  float blue_frac;
  int8_t row;
  int8_t col;
  uint16_t flags;
  bool from_gpu;
  char name[32];
};

struct RexScreenWatcherStatus {
  uint64_t last_frame_seq;
  uint64_t last_update_ms;
  uint32_t pixel_hash;
  float motion_pct;
  bool in_transition;
  bool has_pixel_frame;
  bool has_highlight;
  bool has_target;
  bool aligned;
  bool has_cursor_rgb;
  uint8_t cursor_r, cursor_g, cursor_b;
  RexScreenId screen_id;
  int8_t hl_row, hl_col;
  float hl_gold_frac;
  float hl_u, hl_v;
  float hl_x0, hl_y0, hl_x1, hl_y1;
  float target_u, target_v;
  float target_x0, target_y0, target_x1, target_y1;
  uint16_t last_dpad_mask;
  char screen_name[32];
  char hl_name[32];
  char target_name[32];
};

struct RexMonitorServoOutput {
  uint16_t buttons_or_mask;
  bool has_target;
  bool has_highlight;
  bool aligned;
  float target_x0, target_y0, target_x1, target_y1;
  float hl_x0, hl_y0, hl_x1, hl_y1;
  float dx, dy;
  char target_name[32];
};

namespace rex_monitor {

void Init();
void Shutdown();

// Layer 1: Guest instrumentation reporting & turret lock
void ReportGuestWidget(uint32_t guest_ptr, uint32_t vtable, float x0, float y0, float x1,
                       float y1, bool has_bounds, bool is_focused, const char* label);

void RecordGuestInputCall(const char* func_name, uint32_t pc_hint, uint32_t r3, uint32_t r4,
                          uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r9,
                          uint32_t r10, double f1, double f2, double f3, double f4,
                          const uint8_t* guest_base, uint32_t dump_ea, uint16_t dpad_mask,
                          bool force_log);

void NotifyGameplayTurretTick(uint32_t gun_ptr, float cur_angle_rad, float new_angle_rad,
                              bool locked_by_mouse);
bool IsInGameplay();
uint64_t GetGameplayFrameCounter();
uint32_t GetLastTurretPtr();
float GetLastTurretAngleDeg();

// Layer 2: Pull latest GPU 2D UI quad snapshot & notify D-Pad edges
bool PollGpuSnapshot(RexGpuUiMonitorSnapshotMirror& out_snap);
void InjectSyntheticSnapshotForTest(const RexGpuUiMonitorSnapshotMirror& snap);
void InjectSyntheticFrameForTest(const uint8_t* pixels_rgbl, uint32_t width, uint32_t height,
                                 uint64_t seq = 1);
void ClearTestState();
void SetScreenForTest(RexScreenId screen_id);
RexScreenId GetCurrentScreenId();
void NotifyHostDpadSent(uint16_t dpad_mask);
void NotifyMenuActionInput(uint16_t action_mask, uint64_t now_ms);

// Layer 1+2 + 10 Hz (0.1s) Stateless Screen Watcher state & Closed-Loop Focus Servo
void UpdateCursorSample(float norm_x, float norm_y);
void GetSnapshotForOverlay(RexGpuUiMonitorSnapshotMirror& out_gpu,
                           std::vector<RexGuestWidgetBox>& out_guest_widgets,
                           RexMonitorServoOutput& out_last_servo);
void GetScreenWatcherState(RexScreenWatcherStatus& out_status,
                           std::vector<RexCandidateButton>& out_candidates);

// Computes DPAD/A outputs on a 0.1s closed-loop cadence to steer ActiveHighlightBox onto HoveredButton.
RexMonitorServoOutput StepMenuFocusServo(float norm_x, float norm_y, bool allow_hover_servo,
                                         bool click_trigger, bool is_touch_tap,
                                         uint64_t now_ms);
bool SaveScreenWatcherPng(const char* path, RexScreenWatcherStatus* out_status = nullptr,
                          size_t* out_cand_count = nullptr);
bool MeasureVisualTurretAngleFromPixels(const uint8_t* pixels_rgbl, uint32_t width, uint32_t height,
                                        float& out_angle_rad, uint32_t* out_dot_count = nullptr);
bool MeasureLatestVisualTurretAngle(float& out_angle_rad, uint32_t* out_dot_count = nullptr);

void EnableUiLog(bool enable);
void LogLine(const char* category, const char* msg);
void FlushLog();

void EnableReplayHitboxAudit(bool enable, const char* path = "replay_hitbox_audit.log");
void FlushReplayHitboxAudit();

}  // namespace rex_monitor
