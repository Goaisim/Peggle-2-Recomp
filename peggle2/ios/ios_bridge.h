#pragma once

#include <TargetConditionals.h>
#if defined(__APPLE__)
#include <cxxabi.h>
#include <rex/logging/api.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

inline std::string RexIosDemangleSymbol(const char* raw_sym) {
  if (!raw_sym || raw_sym[0] == '\0') return "-";
  std::string_view sv(raw_sym);
  std::string_view prefix;
  if (sv.starts_with("LR:")) {
    prefix = "LR:";
    sv.remove_prefix(3);
  }
  std::string_view suffix;
  const size_t plus_pos = sv.rfind("+0x");
  if (plus_pos != std::string_view::npos) {
    suffix = sv.substr(plus_pos);
    sv = sv.substr(0, plus_pos);
  }
  if (sv.starts_with("__Z")) {
    sv.remove_prefix(1);
  }
  if (sv.starts_with("_Z")) {
    std::string mangled(sv);
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled) {
      std::string out;
      out.reserve(prefix.size() + std::strlen(demangled) + suffix.size());
      out.append(prefix);
      out.append(demangled);
      out.append(suffix);
      std::free(demangled);
      return out;
    }
    std::free(demangled);
  }
  return std::string(raw_sym);
}

inline std::string RexIosFormatCrashReport(const rex::RexBootTelemetrySnapshot& tel,
                                           std::string* out_demangled_sym = nullptr) {
  std::string sym = RexIosDemangleSymbol(tel.fault_sym);
  if (out_demangled_sym) *out_demangled_sym = sym;
  const char* stage_name = rex::GetBootStageName(tel.boot_stage);
  const char* cpu_state = rex::GetGuestThreadStateName(tel.guest_state);
  char hdr[1024];
  std::snprintf(
      hdr, sizeof(hdr),
      "Build=%s\n"
      "BootStage: %d:%s (0:Init|1:ArenaOK|2:XexOK|3:VFS(%u files)|4:ThreadRun|5:VdSwap) "
      "Base=0x%llX%s%s\n"
      "GuestCPU:  Ticks=%llu | PC=0x%08X | LR=0x%08X | State=%s%s | VBlanks=%llu\n"
      "Fault:     Sig=%u FaultAddr=0x%llX Thr=%s GPC=0x%08X GLR=0x%08X\n"
      "FaultHost: PC=0x%llX LR=0x%llX Insn=0x%08X ",
      tel.build_sha[0] ? tel.build_sha : "unknown",
      tel.boot_stage, stage_name, tel.bundle_assets,
      static_cast<unsigned long long>(tel.guest_base),
      tel.boot_error[0] ? " Err=" : "",
      tel.boot_error[0] ? tel.boot_error : "",
      static_cast<unsigned long long>(tel.guest_ticks),
      tel.guest_pc, tel.guest_lr, cpu_state,
      tel.gpu_fence_wait ? "+GPUWait" : "",
      static_cast<unsigned long long>(tel.vblanks),
      tel.fault_signal,
      static_cast<unsigned long long>(tel.fault_addr),
      tel.fault_thread[0] ? tel.fault_thread : "-",
      tel.fault_guest_pc, tel.fault_guest_lr,
      static_cast<unsigned long long>(tel.fault_host_pc),
      static_cast<unsigned long long>(tel.fault_host_lr),
      tel.fault_insn);
  std::string out(hdr);
  out += "Sym=";
  out += sym;
  out += '\n';
  char regs[192];
  std::snprintf(regs, sizeof(regs),
                "Regs: x0=0x%llx x1=0x%llx x19=0x%llx x20=0x%llx | g_r3=0x%x g_r4=0x%x\n",
                static_cast<unsigned long long>(tel.fault_host_x0),
                static_cast<unsigned long long>(tel.fault_host_x1),
                static_cast<unsigned long long>(tel.fault_host_x19),
                static_cast<unsigned long long>(tel.fault_host_x20),
                tel.fault_guest_r3, tel.fault_guest_r4);
  out += regs;
  char gprs_buf[320];
  const auto& g = tel.fault_guest_gprs;
  std::snprintf(
      gprs_buf, sizeof(gprs_buf),
      "GPRs: r1=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x\n"
      "      r10=0x%08x r11=0x%08x r13=0x%08x r28=0x%08x r29=0x%08x r30=0x%08x r31=0x%08x\n",
      g.r1, g.r3, g.r4, g.r5, g.r6, g.r7, g.r8, g.r9, g.r10, g.r11, g.r13, g.r28, g.r29, g.r30,
      g.r31);
  out += gprs_buf;
  if (tel.last_import_count > 0) {
    const uint32_t icount =
        std::min(tel.last_import_count, rex::RexBootTelemetrySnapshot::kMaxLastImports);
    out += "LastImports: " + std::to_string(icount) + "\n";
    for (uint32_t i = 0; i < icount; ++i) {
      char imp_buf[192];
      const auto& e = tel.last_imports[i];
      std::snprintf(imp_buf, sizeof(imp_buf), "  %s r3=0x%08x r4=0x%08x -> 0x%08x\n",
                    e.name ? e.name : "-", e.r3, e.r4, e.return_value);
      out += imp_buf;
    }
  }
  const uint32_t count = std::min(tel.recent_fail_count, rex::RexBootTelemetrySnapshot::kMaxRecentFails);
  out += "RecentFails: " + std::to_string(count) + "\n";
  for (uint32_t i = 0; i < count; ++i) {
    out += "  ";
    out += tel.recent_fails[i];
    out += '\n';
  }
  return out;
}

inline bool RexIosWriteCrashReportFile(const std::string& path, const std::string& text) {
  if (path.empty()) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  return out.good();
}

inline constexpr uint64_t kRexIosQuickTapMaxDurationMs = 250;
inline constexpr float kRexIosQuickTapMaxDragPt = 18.0f;

// True when a touch released quickly without significant finger travel (distinct from slingshot drag).
inline bool RexIosIsQuickTap(float start_pt_x, float start_pt_y, float end_pt_x, float end_pt_y,
                             uint64_t duration_ms) {
  const float dx = end_pt_x - start_pt_x;
  const float dy = end_pt_y - start_pt_y;
  return duration_ms < kRexIosQuickTapMaxDurationMs &&
         (dx * dx + dy * dy) < (kRexIosQuickTapMaxDragPt * kRexIosQuickTapMaxDragPt);
}

// 256-byte aligned bump allocation inside a fixed-capacity ring buffer slice.
inline bool RexIosRingBumpAlloc(uint32_t& cursor, uint32_t bytes, uint32_t capacity,
                                uint32_t& out_offset) {
  if (bytes == 0 || bytes > capacity || cursor > capacity) {
    return false;
  }
  const uint32_t aligned_cursor = (cursor + 255u) & ~255u;
  if (aligned_cursor > capacity - bytes) {
    return false;
  }
  out_offset = aligned_cursor;
  cursor = aligned_cursor + bytes;
  return true;
}
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rex::audio {
class AudioSystem;
}
namespace rex::graphics {
class GraphicsSystem;
}
namespace rex::input {
class InputSystem;
}
namespace rex::runtime {
class FunctionDispatcher;
}

struct RexIosTouchSnapshot {
  bool down = false;
  bool pressed = false;
  bool released = false;
  bool quick_tap = false;
  float u = 0.5f;
  float v = 0.5f;
  float start_u = 0.5f;
  float start_v = 0.5f;
  bool in_viewport = false;
};

// Path & subsystem factories implemented in ios/main_ios.mm, rexvideo_metal.mm, rexaudio_ios.mm.
std::string rex_ios_get_documents_dir();
std::string rex_ios_get_bundle_resource_dir();
void rex_ios_init_vfs_case_map();
void rex_ios_init_vfs_case_map_for_path(const std::string& game_source_path);
std::string rex_ios_resolve_startup_game_source();
void rex_ios_present_game_picker(bool allow_cancel,
                                 std::function<void(const std::string&)> on_selected);
bool rex_ios_is_game_picker_active();
std::unique_ptr<rex::graphics::GraphicsSystem> rex_ios_create_graphics_system();
std::unique_ptr<rex::audio::AudioSystem> rex_ios_create_audio_system(
    rex::runtime::FunctionDispatcher* fd);
std::unique_ptr<rex::input::InputSystem> rex_ios_create_input_system();

// Touch state updated from UIKit (main thread) and consumed by the guest input hook & ImGui.
void rex_ios_on_touch_began(float pt_x, float pt_y, float view_w, float view_h);
void rex_ios_on_touch_moved(float pt_x, float pt_y, float view_w, float view_h);
void rex_ios_on_touch_ended(float pt_x, float pt_y, float view_w, float view_h);
void rex_ios_on_touch_cancelled();
RexIosTouchSnapshot rex_ios_consume_touch_for_guest();

struct RexIosPerfSnapshot {
  float cpu_ms = 0.0f;
  float enc_ms = 0.0f;
  float gpu_ms = 0.0f;
  uint32_t draws_per_frame = 0;
  uint32_t tex_uploads_per_frame = 0;
  uint32_t buf_allocs_per_frame = 0;
};

RexIosPerfSnapshot rex_ios_get_perf_snapshot();
const char* rex_ios_get_thermal_state_name();
uint64_t rex_ios_get_guest_frame_seq();
void rex_ios_wait_for_metal_idle();
void rex_ios_audio_set_paused(bool paused);
void rex_ios_set_target_fps(uint32_t hz);
void rex_ios_set_low_power_auto_scale_enabled(bool enabled);
bool rex_ios_is_low_power_075x_active();

#endif
