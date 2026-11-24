// Native iOS UIKit shell, CAMetalLayer view, 120Hz ProMotion display link, Documents/Bundle asset
// paths, GCController input, and UITouch -> ScreenWatcher / Balljector aim bridge.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "ios_bridge.h"

#include <rex/cvar.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/device_assignment.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#if defined(__has_include) && __has_include(<ptrauth.h>)
#include <ptrauth.h>
#endif

namespace {

using rex::X_RESULT;
using rex::X_STATUS;

// Normalized 16:9 viewport math mirroring src/main.cpp Compute16x9Viewport.
static void ComputeNormalized16x9(float px, float py, float vw, float vh, float& out_u,
                                   float& out_v, bool& out_in_vp) {
  out_u = 0.5f;
  out_v = 0.5f;
  out_in_vp = false;
  if (vw <= 1.0f || vh <= 1.0f) return;
  constexpr float kAspect = 16.0f / 9.0f;
  float vpx = 0.0f, vpy = 0.0f, vpw = vw, vph = vh;
  const float aspect = vw / vh;
  if (aspect > kAspect) {
    vpw = vh * kAspect;
    vpx = 0.5f * (vw - vpw);
  } else if (aspect < kAspect) {
    vph = vw / kAspect;
    vpy = 0.5f * (vh - vph);
  }
  out_u = (px - vpx) / std::max(1.0f, vpw);
  out_v = (py - vpy) / std::max(1.0f, vph);
  out_in_vp = (out_u >= 0.0f && out_u <= 1.0f && out_v >= 0.0f && out_v <= 1.0f);
  out_u = std::clamp(out_u, 0.0f, 1.0f);
  out_v = std::clamp(out_v, 0.0f, 1.0f);
}

std::atomic<bool> g_display_link_active{false};
std::atomic<uint64_t> g_ui_redraw_until_ms{0};

uint64_t TouchNowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint64_t SteadyNowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

static std::atomic<uint32_t> g_ios_target_fps{60};

void BumpUiRedrawWindow(uint64_t duration_ms = 250) {
  const uint64_t deadline = TouchNowMs() + duration_ms;
  uint64_t cur = g_ui_redraw_until_ms.load(std::memory_order_relaxed);
  while (cur < deadline && !g_ui_redraw_until_ms.compare_exchange_weak(cur, deadline, std::memory_order_relaxed)) {}
}

struct SharedTouchState {
  std::mutex mutex;
  bool down = false;
  bool pressed_pending = false;
  bool released_pending = false;
  bool quick_tap_pending = false;
  uint64_t began_ms = 0;
  float start_pt_x = 0.0f, start_pt_y = 0.0f;
  float u = 0.5f, v = 0.5f;
  float start_u = 0.5f, start_v = 0.5f;
  bool in_vp = false;
} g_touch_state;

class IOSCAMetalLayerSurface final : public rex::ui::Surface {
 public:
  IOSCAMetalLayerSurface(CAMetalLayer* layer, UIView* view) : layer_(layer), view_(view) {}
  TypeIndex GetType() const override { return kTypeIndex_CAMetalLayer; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    if (!view_ || !layer_) {
      width_out = 0;
      height_out = 0;
      return false;
    }
    const CGFloat scale = layer_.contentsScale > 0 ? layer_.contentsScale : 1.0;
    const CGSize sz = view_.bounds.size;
    width_out = uint32_t(std::max<CGFloat>(1.0, sz.width * scale));
    height_out = uint32_t(std::max<CGFloat>(1.0, sz.height * scale));
    return width_out > 0 && height_out > 0;
  }

 private:
  CAMetalLayer* layer_ = nil;
  UIView* view_ = nil;
};

class IOSWindowedAppContext;
class IOSWindow;

static IOSWindowedAppContext* g_ios_app_ctx = nullptr;
static IOSWindow* g_ios_window = nullptr;
static __weak UIView* g_metal_view = nil;
static __weak CAMetalLayer* g_metal_layer = nil;
static __weak UIViewController* g_root_vc = nil;

static NSString* const kPeggle2GameBookmarkKey = @"Peggle2GameBookmark";
static NSURL* g_active_security_scoped_url = nil;
static BOOL g_active_security_scoped_active = NO;
static std::atomic<bool> g_picker_overlay_active{false};

class IOSWindowedAppContext final : public rex::ui::WindowedAppContext {
 public:
  void SetMainThread(void) { ui_thread_id_ = std::this_thread::get_id(); }

 protected:
  void NotifyUILoopOfPendingFunctions() override {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (g_ios_app_ctx) {
        g_ios_app_ctx->ExecutePendingFunctionsFromUIThread();
      }
    });
  }

  void PlatformQuitFromUIThread() override {}
};

class IOSWindow final : public rex::ui::Window {
 public:
  explicit IOSWindow(rex::ui::WindowedAppContext& ctx, std::string_view title, uint32_t w,
                     uint32_t h)
      : Window(ctx, title, w, h) {
    g_ios_window = this;
  }

  ~IOSWindow() override {
    EnterDestructor();
    if (g_ios_window == this) g_ios_window = nullptr;
  }

  void NotifyResizeFromView(uint32_t pw, uint32_t ph) {
    WindowDestructionReceiver rec(this);
    OnActualSizeUpdate(pw, ph, rec);
  }

  void DispatchMouseFromTouch(int kind, float px, float py) {
    WindowDestructionReceiver rec(this);
    const CGFloat scale = g_metal_layer ? g_metal_layer.contentsScale : 1.0;
    const int32_t x = int32_t(px * scale);
    const int32_t y = int32_t(py * scale);
    if (kind == 0) {
      rex::ui::MouseEvent mv(this, rex::ui::MouseEvent::Button::kNone, x, y, 0);
      OnMouseMove(mv, rec);
      rex::ui::MouseEvent md(this, rex::ui::MouseEvent::Button::kLeft, x, y, 0);
      OnMouseDown(md, rec);
    } else if (kind == 1) {
      rex::ui::MouseEvent mv(this, rex::ui::MouseEvent::Button::kLeft, x, y, 0);
      OnMouseMove(mv, rec);
    } else {
      rex::ui::MouseEvent mu(this, rex::ui::MouseEvent::Button::kLeft, x, y, 0);
      OnMouseUp(mu, rec);
    }
  }

  void PaintNow() { OnPaint(true); }

 protected:
  bool OpenImpl() override {
    UIView* view = g_metal_view;
    CAMetalLayer* layer = g_metal_layer;
    if (!view || !layer) return false;
    const CGFloat scale = layer.contentsScale > 0 ? layer.contentsScale : 1.0;
    const uint32_t pw = uint32_t(std::max<CGFloat>(1.0, view.bounds.size.width * scale));
    const uint32_t ph = uint32_t(std::max<CGFloat>(1.0, view.bounds.size.height * scale));
    layer.drawableSize = CGSizeMake(pw, ph);
    WindowDestructionReceiver rec(this);
    OnActualSizeUpdate(pw, ph, rec);
    OnFocusUpdate(true, rec);
    return true;
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver rec(this);
    OnBeforeClose(rec);
    OnAfterClose();
  }

  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags allowed) override {
    if (!(allowed & rex::ui::Surface::kTypeFlag_CAMetalLayer)) return nullptr;
    if (!g_metal_layer || !g_metal_view) return nullptr;
    return std::make_unique<IOSCAMetalLayerSurface>(g_metal_layer, g_metal_view);
  }

  void RequestPaintImpl() override {
    BumpUiRedrawWindow(250);
    if (g_display_link_active.load(std::memory_order_relaxed)) {
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (g_ios_window && !rex::IsAppBackgroundPaused()) g_ios_window->PaintNow();
    });
  }
};

class IOSGCInputDriver final : public rex::input::InputDriver {
 public:
  IOSGCInputDriver() : InputDriver(nullptr, 0) {}

  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    out.clear();
    NSArray<GCController*>* ctrls = [GCController controllers];
    for (NSUInteger i = 0; i < ctrls.count; ++i) {
      if (ctrls[i].extendedGamepad) {
        rex::input::DeviceInfo info{};
        info.id = static_cast<rex::input::DeviceId>(i);
        info.name = ctrls[i].vendorName ? ctrls[i].vendorName.UTF8String : "iOS Gamepad";
        out.push_back(info);
      }
    }
  }

  rex::X_RESULT GetDeviceState(rex::input::DeviceId id,
                               rex::input::X_INPUT_STATE* out_state) override {
    if (!out_state) return X_ERROR_INVALID_PARAMETER;
    NSArray<GCController*>* ctrls = [GCController controllers];
    const NSUInteger idx = static_cast<NSUInteger>(id);
    if (idx >= ctrls.count || !ctrls[idx].extendedGamepad) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    GCExtendedGamepad* gp = ctrls[idx].extendedGamepad;
    uint16_t btns = 0;
    if (gp.dpad.up.pressed) btns |= 0x0001;
    if (gp.dpad.down.pressed) btns |= 0x0002;
    if (gp.dpad.left.pressed) btns |= 0x0004;
    if (gp.dpad.right.pressed) btns |= 0x0008;
    if (gp.buttonMenu.pressed) btns |= 0x0010;
    if (gp.buttonOptions && gp.buttonOptions.pressed) btns |= 0x0020;
    if (gp.leftThumbstickButton && gp.leftThumbstickButton.pressed) btns |= 0x0040;
    if (gp.rightThumbstickButton && gp.rightThumbstickButton.pressed) btns |= 0x0080;
    if (gp.leftShoulder.pressed) btns |= 0x0100;
    if (gp.rightShoulder.pressed) btns |= 0x0200;
    if (gp.buttonA.pressed) btns |= 0x1000;
    if (gp.buttonB.pressed) btns |= 0x2000;
    if (gp.buttonX.pressed) btns |= 0x4000;
    if (gp.buttonY.pressed) btns |= 0x8000;

    out_state->packet_number = ++packet_;
    out_state->gamepad.buttons = btns;
    out_state->gamepad.left_trigger = uint8_t(std::clamp(gp.leftTrigger.value * 255.0f, 0.0f, 255.0f));
    out_state->gamepad.right_trigger = uint8_t(std::clamp(gp.rightTrigger.value * 255.0f, 0.0f, 255.0f));
    out_state->gamepad.thumb_lx = int16_t(std::clamp(gp.leftThumbstick.xAxis.value * 32767.0f, -32768.0f, 32767.0f));
    out_state->gamepad.thumb_ly = int16_t(std::clamp(gp.leftThumbstick.yAxis.value * 32767.0f, -32768.0f, 32767.0f));
    out_state->gamepad.thumb_rx = int16_t(std::clamp(gp.rightThumbstick.xAxis.value * 32767.0f, -32768.0f, 32767.0f));
    out_state->gamepad.thumb_ry = int16_t(std::clamp(gp.rightThumbstick.yAxis.value * 32767.0f, -32768.0f, 32767.0f));
    return X_ERROR_SUCCESS;
  }

  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId, uint32_t,
                                      rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                                   rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }

 private:
  uint32_t packet_ = 0;
};

}  // namespace

static void UpdatePowerAndThermalState();

namespace rex::ui {
std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<IOSWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}
}  // namespace rex::ui

extern "C" void* rex_ios_get_metal_layer_ptr() {
  return (__bridge void*)g_metal_layer;
}

std::string rex_ios_get_documents_dir() {
  @autoreleasepool {
    NSArray<NSString*>* dirs =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return dirs.count > 0 ? std::string(dirs.firstObject.UTF8String) : std::string();
  }
}

std::string rex_ios_get_bundle_resource_dir() {
  @autoreleasepool {
    NSString* p = [[NSBundle mainBundle] resourcePath];
    return p ? std::string(p.UTF8String) : std::string();
  }
}

static BOOL CanOpenReadableFile(NSString* path) {
  if (!path || path.length == 0) return NO;
  const int fd = open(path.UTF8String, O_RDONLY);
  if (fd < 0) return NO;
  uint8_t byte = 0;
  const ssize_t n = read(fd, &byte, 1);
  close(fd);
  return n >= 0;
}

static BOOL ValidateGameFolderPath(NSString* dirPath, NSString** outError) {
  if (!dirPath || dirPath.length == 0) {
    if (outError) *outError = @"Invalid folder path.";
    return NO;
  }
  NSFileManager* fm = [NSFileManager defaultManager];
  BOOL isDir = NO;
  if (![fm fileExistsAtPath:dirPath isDirectory:&isDir] || !isDir) {
    if (outError) *outError = @"Selected item is not a folder.";
    return NO;
  }
  NSError* dirErr = nil;
  NSArray<NSString*>* items = [fm contentsOfDirectoryAtPath:dirPath error:&dirErr];
  if (!items) {
    if (outError) {
      *outError = dirErr ? dirErr.localizedDescription
                         : @"iOS App Sandbox denied permission to read the selected folder.";
    }
    return NO;
  }
  NSString* xexPath = nil;
  NSString* rsbPath = nil;
  for (NSString* name in items) {
    NSString* lower = name.lowercaseString;
    if ([lower isEqualToString:@"default.xex"]) {
      xexPath = [dirPath stringByAppendingPathComponent:name];
    } else if ([lower isEqualToString:@"main.rsb"]) {
      rsbPath = [dirPath stringByAppendingPathComponent:name];
    }
  }
  if (!xexPath) {
    if (outError) {
      *outError = @"Selected folder does not contain Default.xex.\n\n"
                  @"Please pick the extracted Peggle 2 game folder containing Default.xex.";
    }
    return NO;
  }
  BOOL xexDir = NO;
  if (![fm fileExistsAtPath:xexPath isDirectory:&xexDir] || xexDir || !CanOpenReadableFile(xexPath)) {
    if (outError) *outError = @"Default.xex inside the selected folder cannot be opened for reading.";
    return NO;
  }
  if (rsbPath && !CanOpenReadableFile(rsbPath)) {
    if (outError) *outError = @"main.rsb inside the selected folder cannot be opened for reading.";
    return NO;
  }
  return YES;
}

static BOOL ValidateXboxIsoFile(NSString* filePath, NSString** outError) {
  if (!filePath || filePath.length == 0) {
    if (outError) *outError = @"Invalid .iso file path.";
    return NO;
  }
  const int fd = open(filePath.UTF8String, O_RDONLY);
  if (fd < 0) {
    if (outError) *outError = @"Cannot open selected .iso file for reading.";
    return NO;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    if (outError) *outError = @"Selected .iso is not a regular readable file.";
    return NO;
  }
  static constexpr uint64_t kGdfxOffsets[] = {
      0x00000000ULL, 0x0000FB20ULL, 0x00020600ULL, 0x02080000ULL, 0x0FD90000ULL,
  };
  static constexpr char kGdfxMagic[] = "MICROSOFT*XBOX*MEDIA";
  BOOL matched = NO;
  char magic[20]{};
  for (uint64_t base_off : kGdfxOffsets) {
    const uint64_t sec32 = base_off + 32ULL * 2048ULL;
    if (sec32 + sizeof(magic) > static_cast<uint64_t>(st.st_size)) continue;
    if (pread(fd, magic, sizeof(magic), static_cast<off_t>(sec32)) == sizeof(magic) &&
        std::memcmp(magic, kGdfxMagic, sizeof(magic)) == 0) {
      matched = YES;
      break;
    }
  }
  close(fd);
  if (!matched && outError) {
    *outError = @"Selected .iso is not a valid Xbox 360 GDFX disc image.";
  }
  return matched;
}

static BOOL ValidateStfsContainerFile(NSString* filePath, bool* outNeedsFolder, NSString** outError) {
  if (outNeedsFolder) *outNeedsFolder = false;
  if (!filePath || filePath.length == 0 || !filePath.UTF8String) return NO;
  const auto fsPath = rex::to_path(filePath.UTF8String);
  auto hdr = rex::filesystem::StfsContainerDevice::ReadPackageHeader(fsPath);
  if (!hdr) return NO;
  if (hdr->metadata.volume_type == rex::filesystem::XContentVolumeType::kSvod) {
    NSString* dataDir = [filePath stringByAppendingString:@".data"];
    NSError* dirErr = nil;
    NSArray<NSString*>* frag =
        [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dataDir error:&dirErr];
    if (!frag || dirErr) {
      if (outNeedsFolder) *outNeedsFolder = true;
      if (outError) {
        *outError = @"This SVOD package requires sibling .data files restricted by iOS sandbox.\n\n"
                    @"Please select the containing Game Folder instead.";
      }
      return NO;
    }
  }
  rex::filesystem::StfsContainerDevice probe("", fsPath);
  if (!probe.Initialize() || !probe.ResolvePath("default.xex")) {
    if (outError) {
      *outError = @"Selected XBLA STFS container failed to mount or does not contain Default.xex.";
    }
    return NO;
  }
  return YES;
}

static BOOL IsValidGameSourcePathNS(NSString* path) {
  if (!path || path.length == 0) return NO;
  BOOL isDir = NO;
  if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir]) return NO;
  if (isDir) {
    return ValidateGameFolderPath(path, nil);
  }
  if ([path.pathExtension.lowercaseString isEqualToString:@"iso"]) {
    return ValidateXboxIsoFile(path, nil);
  }
  return ValidateStfsContainerFile(path, nullptr, nil);
}

static NSString* FindGameSourceInDir(NSString* dirPath) {
  if (!dirPath || dirPath.length == 0) return nil;
  if (ValidateGameFolderPath(dirPath, nil)) return dirPath;
  NSFileManager* fm = [NSFileManager defaultManager];
  NSError* err = nil;
  NSArray<NSString*>* rawItems = [fm contentsOfDirectoryAtPath:dirPath error:&err];
  if (!rawItems || err) return nil;
  NSArray<NSString*>* items =
      [rawItems sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
  for (NSString* name in items) {
    NSString* full = [dirPath stringByAppendingPathComponent:name];
    BOOL isDir = NO;
    if ([fm fileExistsAtPath:full isDirectory:&isDir] && !isDir) {
      if ([name.pathExtension.lowercaseString isEqualToString:@"iso"] &&
          ValidateXboxIsoFile(full, nil)) {
        return full;
      }
      if (ValidateStfsContainerFile(full, nullptr, nil)) {
        return full;
      }
    }
  }
  for (NSString* name in items) {
    NSString* full = [dirPath stringByAppendingPathComponent:name];
    BOOL isDir = NO;
    if ([fm fileExistsAtPath:full isDirectory:&isDir] && isDir) {
      if (ValidateGameFolderPath(full, nil)) {
        return full;
      }
    }
  }
  return nil;
}

static void SaveSecurityScopedBookmark(NSURL* url) {
  if (!url) return;
  NSError* err = nil;
  NSData* bm = [url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark
                   includingResourceValuesForKeys:nil
                                    relativeToURL:nil
                                            error:&err];
  if (!bm) {
    bm = [url bookmarkDataWithOptions:0
         includingResourceValuesForKeys:nil
                          relativeToURL:nil
                                  error:&err];
  }
  if (bm) {
    [[NSUserDefaults standardUserDefaults] setObject:bm forKey:kPeggle2GameBookmarkKey];
    [[NSUserDefaults standardUserDefaults] synchronize];
  }
}

static void ActivateSessionSecurityScopedURL(NSURL* url, BOOL scoped, BOOL stopPrevious) {
  if (stopPrevious && g_active_security_scoped_url && g_active_security_scoped_active &&
      ![g_active_security_scoped_url isEqual:url]) {
    [g_active_security_scoped_url stopAccessingSecurityScopedResource];
  }
  g_active_security_scoped_url = url;
  g_active_security_scoped_active = scoped;
}

static NSString* ResolveSavedGameBookmark() {
  NSData* bm = [[NSUserDefaults standardUserDefaults] dataForKey:kPeggle2GameBookmarkKey];
  if (!bm || bm.length == 0) return nil;
  BOOL isStale = NO;
  NSError* err = nil;
  NSURL* url = [NSURL URLByResolvingBookmarkData:bm
                                         options:0
                                   relativeToURL:nil
                             bookmarkDataIsStale:&isStale
                                           error:&err];
  if (!url || !url.path) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kPeggle2GameBookmarkKey];
    return nil;
  }
  const BOOL scoped = [url startAccessingSecurityScopedResource];
  if (!IsValidGameSourcePathNS(url.path)) {
    if (scoped) [url stopAccessingSecurityScopedResource];
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kPeggle2GameBookmarkKey];
    return nil;
  }
  ActivateSessionSecurityScopedURL(url, scoped, YES);
  if (isStale) {
    SaveSecurityScopedBookmark(url);
  }
  return url.path;
}

static void EnsureDocumentsSubdirs() {
  const std::string docs = rex_ios_get_documents_dir();
  if (docs.empty()) return;
  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* docs_ns = @(docs.c_str());
  for (NSString* sub in @[ @"UserData", @"cache", @"game" ]) {
    [fm createDirectoryAtPath:[docs_ns stringByAppendingPathComponent:sub]
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
  }
}

std::string rex_ios_resolve_startup_game_source() {
  @autoreleasepool {
    EnsureDocumentsSubdirs();
    NSFileManager* fm = [NSFileManager defaultManager];

    // (a) Documents/game/ (copied via iOS Files app)
    const std::string docs = rex_ios_get_documents_dir();
    if (!docs.empty()) {
      NSString* docsGame = [@(docs.c_str()) stringByAppendingPathComponent:@"game"];
      if (NSString* found = FindGameSourceInDir(docsGame)) {
        return std::string(found.UTF8String);
      }
    }

    // (b) Saved security-scoped bookmark (folder, .iso, or XBLA container)
    if (NSString* bookmarked = ResolveSavedGameBookmark()) {
      return std::string(bookmarked.UTF8String);
    }

    // (c) Optional fallback to app bundle assets/ if present locally
    NSString* bundlePath = [[NSBundle mainBundle] resourcePath];
    if (bundlePath && bundlePath.length > 0) {
      NSString* bundleAssets = [bundlePath stringByAppendingPathComponent:@"assets"];
      if (ValidateGameFolderPath(bundleAssets, nil)) {
        return std::string(bundleAssets.UTF8String);
      }
      NSString* bundleAssetsGame = [bundleAssets stringByAppendingPathComponent:@"game"];
      if (ValidateGameFolderPath(bundleAssetsGame, nil)) {
        return std::string(bundleAssetsGame.UTF8String);
      }
      BOOL isDir = NO;
      if ([fm fileExistsAtPath:bundleAssets isDirectory:&isDir] && isDir) {
        if (NSString* found = FindGameSourceInDir(bundleAssets)) {
          return std::string(found.UTF8String);
        }
      }
    }
    return {};
  }
}

void rex_ios_init_vfs_case_map_for_path(const std::string& game_source_path) {
  @autoreleasepool {
    rex::ClearCaseInsensitivePathMap();
    EnsureDocumentsSubdirs();

    if (game_source_path.empty()) {
      rex::SetBundleAssetCount(0);
      return;
    }

    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* root_ns = @(game_source_path.c_str());
    BOOL is_dir = NO;
    if (![fm fileExistsAtPath:root_ns isDirectory:&is_dir]) {
      rex::SetBundleAssetCount(0);
      return;
    }

    if (!is_dir) {
      // .iso or STFS XBLA container mounts via DiscImageDevice / StfsContainerDevice.
      rex::SetBundleAssetCount(1);
      REXLOG_INFO("iOS VFS source configured (container/ISO): {}", game_source_path);
      return;
    }

    uint32_t file_count = 0;
    NSDirectoryEnumerator<NSString*>* enumerator = [fm enumeratorAtPath:root_ns];
    for (NSString* rel_ns in enumerator) {
      NSString* full_ns = [root_ns stringByAppendingPathComponent:rel_ns];
      const std::string rel_utf8(rel_ns.UTF8String ? rel_ns.UTF8String : "");
      const std::string full_utf8(full_ns.UTF8String ? full_ns.UTF8String : "");
      if (rel_utf8.empty() || full_utf8.empty()) continue;
      rex::RegisterCaseInsensitivePath(rel_utf8, full_utf8);
      BOOL entry_dir = NO;
      if ([fm fileExistsAtPath:full_ns isDirectory:&entry_dir] && !entry_dir) {
        ++file_count;
      }
    }

    const uint32_t indexed_count =
        file_count > 0 ? file_count : rex::GetCaseInsensitivePathMapCount();
    rex::SetBundleAssetCount(indexed_count);
    REXLOG_INFO("iOS VFS case map initialized for '{}': Files={} (mapEntries={})",
                game_source_path, indexed_count, rex::GetCaseInsensitivePathMapCount());
  }
}

void rex_ios_init_vfs_case_map() {
  rex_ios_init_vfs_case_map_for_path(rex_ios_resolve_startup_game_source());
}

std::unique_ptr<rex::input::InputSystem> rex_ios_create_input_system() {
  auto sys = std::make_unique<rex::input::InputSystem>(nullptr);
  sys->AddDriver(std::make_unique<IOSGCInputDriver>());
  sys->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  sys->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  return sys;
}

void rex_ios_on_touch_began(float pt_x, float pt_y, float view_w, float view_h) {
  float u = 0.5f, v = 0.5f;
  bool in_vp = false;
  ComputeNormalized16x9(pt_x, pt_y, view_w, view_h, u, v, in_vp);
  const uint64_t now_ms = TouchNowMs();
  std::lock_guard<std::mutex> lock(g_touch_state.mutex);
  g_touch_state.down = true;
  g_touch_state.pressed_pending = true;
  g_touch_state.quick_tap_pending = false;
  g_touch_state.began_ms = now_ms;
  g_touch_state.start_pt_x = pt_x;
  g_touch_state.start_pt_y = pt_y;
  g_touch_state.u = u;
  g_touch_state.v = v;
  g_touch_state.start_u = u;
  g_touch_state.start_v = v;
  g_touch_state.in_vp = in_vp;
}

void rex_ios_on_touch_moved(float pt_x, float pt_y, float view_w, float view_h) {
  float u = 0.5f, v = 0.5f;
  bool in_vp = false;
  ComputeNormalized16x9(pt_x, pt_y, view_w, view_h, u, v, in_vp);
  std::lock_guard<std::mutex> lock(g_touch_state.mutex);
  g_touch_state.down = true;
  g_touch_state.u = u;
  g_touch_state.v = v;
  g_touch_state.in_vp = in_vp;
}

void rex_ios_on_touch_ended(float pt_x, float pt_y, float view_w, float view_h) {
  float u = 0.5f, v = 0.5f;
  bool in_vp = false;
  ComputeNormalized16x9(pt_x, pt_y, view_w, view_h, u, v, in_vp);
  const uint64_t now_ms = TouchNowMs();
  std::lock_guard<std::mutex> lock(g_touch_state.mutex);
  const uint64_t duration_ms =
      (now_ms >= g_touch_state.began_ms) ? (now_ms - g_touch_state.began_ms) : 0;
  if (in_vp && RexIosIsQuickTap(g_touch_state.start_pt_x, g_touch_state.start_pt_y, pt_x, pt_y,
                                duration_ms)) {
    g_touch_state.quick_tap_pending = true;
  }
  g_touch_state.down = false;
  g_touch_state.released_pending = true;
  g_touch_state.u = u;
  g_touch_state.v = v;
  g_touch_state.in_vp = in_vp;
}

void rex_ios_on_touch_cancelled() {
  std::lock_guard<std::mutex> lock(g_touch_state.mutex);
  g_touch_state.down = false;
  g_touch_state.pressed_pending = false;
  g_touch_state.released_pending = false;
  g_touch_state.quick_tap_pending = false;
}

RexIosTouchSnapshot rex_ios_consume_touch_for_guest() {
  std::lock_guard<std::mutex> lock(g_touch_state.mutex);
  RexIosTouchSnapshot snap{};
  snap.down = g_touch_state.down;
  snap.pressed = g_touch_state.pressed_pending;
  snap.released = g_touch_state.released_pending;
  snap.quick_tap = g_touch_state.quick_tap_pending;
  snap.u = g_touch_state.u;
  snap.v = g_touch_state.v;
  snap.start_u = g_touch_state.start_u;
  snap.start_v = g_touch_state.start_v;
  snap.in_viewport = g_touch_state.in_vp;
  g_touch_state.pressed_pending = false;
  g_touch_state.released_pending = false;
  g_touch_state.quick_tap_pending = false;
  return snap;
}

// ============================================================================
// UIKit AppDelegate, ViewController, & CAMetalLayer View
// ============================================================================
@interface PeggleMetalView : UIView
@end

@implementation PeggleMetalView
+ (Class)layerClass {
  return [CAMetalLayer class];
}
@end

@interface PeggleViewController : UIViewController {
  PeggleMetalView* _metalView;
  CADisplayLink* _displayLink;
  uint64_t _lastPresentedGuestSeq;
  uint64_t _lastPresentedMs;
  uint64_t _next40HzDeadlineNs;
}
- (void)setRenderPaused:(BOOL)paused;
- (void)updateDisplayLinkFrameRate:(uint32_t)hz;
@end

@implementation PeggleViewController

- (BOOL)prefersStatusBarHidden {
  return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
  return YES;
}

- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
  return UIRectEdgeAll;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskLandscape;
}

- (void)loadView {
  _metalView = [[PeggleMetalView alloc] initWithFrame:[UIScreen mainScreen].bounds];
  _metalView.backgroundColor = [UIColor blackColor];
  _metalView.multipleTouchEnabled = NO;
  _metalView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  CAMetalLayer* layer = (CAMetalLayer*)_metalView.layer;
  layer.contentsScale = [UIScreen mainScreen].nativeScale;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = YES;
  layer.presentsWithTransaction = NO;
  g_metal_view = _metalView;
  g_metal_layer = layer;
  g_root_vc = self;
  self.view = _metalView;
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  if (!_displayLink) {
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(onFrame:)];
    [self updateDisplayLinkFrameRate:g_ios_target_fps.load(std::memory_order_relaxed)];
    _displayLink.paused = rex::IsAppBackgroundPaused() ? YES : NO;
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    g_display_link_active.store(true, std::memory_order_release);
  }
}

- (void)updateDisplayLinkFrameRate:(uint32_t)hz {
  if (!_displayLink) {
    return;
  }
  const NSInteger screen_max_hz = std::max<NSInteger>(60, [UIScreen mainScreen].maximumFramesPerSecond);
  const float max_hz = float(std::min<NSInteger>(120, screen_max_hz));
  const float clamped_hz = (hz <= 40 && hz > 0) ? 40.0f : std::min<float>(hz == 0 ? 120.0f : float(hz), max_hz);
  if (@available(iOS 15.0, *)) {
    if (clamped_hz <= 40.0f) {
      _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(40, 40, 40);
    } else {
      _displayLink.preferredFrameRateRange = CAFrameRateRangeMake(60, max_hz, clamped_hz);
    }
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    _displayLink.preferredFramesPerSecond = NSInteger(clamped_hz);
#pragma clang diagnostic pop
  }
}

- (void)setRenderPaused:(BOOL)paused {
  if (_displayLink) {
    _displayLink.paused = paused;
  }
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  if (g_metal_layer && _metalView) {
    const CGFloat scale = g_metal_layer.contentsScale > 0 ? g_metal_layer.contentsScale : 1.0;
    const uint32_t pw = uint32_t(std::max<CGFloat>(1.0, _metalView.bounds.size.width * scale));
    const uint32_t ph = uint32_t(std::max<CGFloat>(1.0, _metalView.bounds.size.height * scale));
    g_metal_layer.drawableSize = CGSizeMake(pw, ph);
    if (g_ios_window) g_ios_window->NotifyResizeFromView(pw, ph);
    BumpUiRedrawWindow(250);
  }
}

- (void)onFrame:(CADisplayLink*)link {
  (void)link;
  @autoreleasepool {
    if (rex::IsAppBackgroundPaused()) return;
    UpdatePowerAndThermalState();
    if (g_ios_app_ctx) g_ios_app_ctx->ExecutePendingFunctionsFromUIThread();
    const uint32_t target_hz = g_ios_target_fps.load(std::memory_order_relaxed);
    if (target_hz == 40) {
      // Smooth 25.0 ms host gate so ProMotion locks to 40 Hz and 60 Hz panels pace 3:2 cleanly.
      const uint64_t now_ns = SteadyNowNs();
      constexpr uint64_t kInterval40HzNs = 25000000ull;
      if (_next40HzDeadlineNs != 0 && now_ns + 2000000ull < _next40HzDeadlineNs) {
        return;
      }
      if (_next40HzDeadlineNs == 0 || now_ns > _next40HzDeadlineNs + kInterval40HzNs * 2ull) {
        _next40HzDeadlineNs = now_ns + kInterval40HzNs;
      } else {
        _next40HzDeadlineNs += kInterval40HzNs;
      }
    } else {
      _next40HzDeadlineNs = 0;
    }
    const uint64_t guest_seq = rex_ios_get_guest_frame_seq();
    const uint64_t now_ms = TouchNowMs();
    const bool new_guest_frame = (guest_seq != _lastPresentedGuestSeq);
    const bool ui_dirty = (now_ms <= g_ui_redraw_until_ms.load(std::memory_order_relaxed)) ||
                          ((now_ms - _lastPresentedMs) >= 250);
    if (!new_guest_frame && !ui_dirty) {
      return;
    }
    _lastPresentedGuestSeq = guest_seq;
    _lastPresentedMs = now_ms;
    if (g_ios_window) g_ios_window->PaintNow();
  }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  if (g_picker_overlay_active.load(std::memory_order_acquire)) return;
  BumpUiRedrawWindow(300);
  UITouch* t = [touches anyObject];
  if (!t || !_metalView) return;
  const CGPoint p = [t locationInView:_metalView];
  const CGSize sz = _metalView.bounds.size;
  if (g_ios_window) g_ios_window->DispatchMouseFromTouch(0, float(p.x), float(p.y));
  rex_ios_on_touch_began(float(p.x), float(p.y), float(sz.width), float(sz.height));
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  if (g_picker_overlay_active.load(std::memory_order_acquire)) return;
  BumpUiRedrawWindow(300);
  UITouch* t = [touches anyObject];
  if (!t || !_metalView) return;
  const CGPoint p = [t locationInView:_metalView];
  const CGSize sz = _metalView.bounds.size;
  if (g_ios_window) g_ios_window->DispatchMouseFromTouch(1, float(p.x), float(p.y));
  rex_ios_on_touch_moved(float(p.x), float(p.y), float(sz.width), float(sz.height));
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  if (g_picker_overlay_active.load(std::memory_order_acquire)) return;
  BumpUiRedrawWindow(300);
  UITouch* t = [touches anyObject];
  if (!t || !_metalView) return;
  const CGPoint p = [t locationInView:_metalView];
  const CGSize sz = _metalView.bounds.size;
  if (g_ios_window) g_ios_window->DispatchMouseFromTouch(2, float(p.x), float(p.y));
  rex_ios_on_touch_ended(float(p.x), float(p.y), float(sz.width), float(sz.height));
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  (void)event;
  if (g_picker_overlay_active.load(std::memory_order_acquire)) return;
  BumpUiRedrawWindow(300);
  UITouch* t = [touches anyObject];
  if (t && _metalView && g_ios_window) {
    const CGPoint p = [t locationInView:_metalView];
    g_ios_window->DispatchMouseFromTouch(2, float(p.x), float(p.y));
  }
  rex_ios_on_touch_cancelled();
}

@end

enum class PegglePickerMode : int {
  kFolder = 0,
  kFile = 1,
};

@interface PeggleGamePickerCoordinator : NSObject <UIDocumentPickerDelegate>
+ (instancetype)shared;
- (void)presentWithAllowCancel:(BOOL)allowCancel
                      callback:(std::function<void(const std::string&)>)callback;
@end

@implementation PeggleGamePickerCoordinator {
  UIView* _overlayView;
  BOOL _allowCancel;
  PegglePickerMode _mode;
  std::function<void(const std::string&)> _callback;
}

+ (instancetype)shared {
  static PeggleGamePickerCoordinator* s_shared = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    s_shared = [[PeggleGamePickerCoordinator alloc] init];
  });
  return s_shared;
}

- (UIViewController*)topPresenter {
  UIViewController* vc = g_root_vc;
  while (vc.presentedViewController && !vc.presentedViewController.isBeingDismissed) {
    vc = vc.presentedViewController;
  }
  return vc;
}

- (void)dismissOverlay {
  [_overlayView removeFromSuperview];
  _overlayView = nil;
  g_picker_overlay_active.store(false, std::memory_order_release);
  BumpUiRedrawWindow(350);
}

- (void)presentWithAllowCancel:(BOOL)allowCancel
                      callback:(std::function<void(const std::string&)>)callback {
  _allowCancel = allowCancel;
  _callback = std::move(callback);
  UIViewController* vc = g_root_vc;
  if (!vc || !vc.view) return;

  if (_overlayView) {
    [_overlayView removeFromSuperview];
    _overlayView = nil;
  }

  UIView* overlay = [[UIView alloc] initWithFrame:vc.view.bounds];
  overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  overlay.backgroundColor = [UIColor colorWithRed:0.04 green:0.06 blue:0.10 alpha:0.94];
  overlay.userInteractionEnabled = YES;

  UIView* card = [[UIView alloc] init];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [UIColor colorWithRed:0.09 green:0.12 blue:0.20 alpha:1.0];
  card.layer.cornerRadius = 16.0;
  card.layer.borderWidth = 1.0;
  card.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.16].CGColor;
  [overlay addSubview:card];

  UILabel* title = [[UILabel alloc] init];
  title.text = @"Peggle 2 — Select Game Source";
  title.font = [UIFont boldSystemFontOfSize:20.0];
  title.textColor = [UIColor whiteColor];
  title.textAlignment = NSTextAlignmentCenter;

  UILabel* subtitle = [[UILabel alloc] init];
  subtitle.text =
      @"Select your Xbox 360 copy of Peggle 2.\n"
      @"Pick an extracted Game Folder (containing Default.xex), a .iso / XBLA file, "
      @"or copy files into Files → On My iPhone → Peggle 2 → game.";
  subtitle.font = [UIFont systemFontOfSize:13.0];
  subtitle.textColor = [UIColor colorWithWhite:0.82 alpha:1.0];
  subtitle.textAlignment = NSTextAlignmentCenter;
  subtitle.numberOfLines = 0;

  auto make_btn = ^UIButton*(NSString* label, UIColor* bg, SEL action) {
    UIButton* btn = [UIButton buttonWithType:UIButtonTypeSystem];
    [btn setTitle:label forState:UIControlStateNormal];
    [btn setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    btn.titleLabel.font = [UIFont boldSystemFontOfSize:16.0];
    btn.backgroundColor = bg;
    btn.layer.cornerRadius = 10.0;
    [btn.heightAnchor constraintEqualToConstant:44.0].active = YES;
    [btn addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return btn;
  };

  UIButton* folderBtn = make_btn(@"Select Game Folder",
                                 [UIColor colorWithRed:0.14 green:0.46 blue:0.88 alpha:1.0],
                                 @selector(onTapFolder));
  UIButton* fileBtn = make_btn(@"Select .ISO / .XEX File",
                               [UIColor colorWithRed:0.18 green:0.32 blue:0.50 alpha:1.0],
                               @selector(onTapFile));

  NSMutableArray<UIView*>* arranged =
      [NSMutableArray arrayWithObjects:title, subtitle, folderBtn, fileBtn, nil];
  if (allowCancel) {
    UIButton* cancelBtn = make_btn(@"Cancel",
                                   [UIColor colorWithWhite:0.22 alpha:1.0],
                                   @selector(onTapCancel));
    [arranged addObject:cancelBtn];
  }

  UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:arranged];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 12.0;
  stack.alignment = UIStackViewAlignmentFill;
  [card addSubview:stack];

  [NSLayoutConstraint activateConstraints:@[
    [card.centerXAnchor constraintEqualToAnchor:overlay.centerXAnchor],
    [card.centerYAnchor constraintEqualToAnchor:overlay.centerYAnchor],
    [card.widthAnchor constraintLessThanOrEqualToConstant:460.0],
    [card.leadingAnchor constraintGreaterThanOrEqualToAnchor:overlay.leadingAnchor constant:24.0],
    [card.trailingAnchor constraintLessThanOrEqualToAnchor:overlay.trailingAnchor constant:-24.0],
    [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:22.0],
    [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-22.0],
    [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:22.0],
    [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-22.0],
  ]];

  [vc.view addSubview:overlay];
  _overlayView = overlay;
  g_picker_overlay_active.store(true, std::memory_order_release);
}

- (void)presentOnTopWhenReady:(UIViewController*)vcToPresent {
  UIViewController* root = g_root_vc;
  if (!root || !vcToPresent) return;
  UIViewController* presented = root.presentedViewController;
  if (presented && presented.isBeingDismissed) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
                     [self presentOnTopWhenReady:vcToPresent];
                   });
    return;
  }
  UIViewController* top = [self topPresenter];
  if (top) {
    [top presentViewController:vcToPresent animated:YES completion:nil];
  }
}

- (void)openFolderPicker {
  _mode = PegglePickerMode::kFolder;
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.modalPresentationStyle = UIModalPresentationFormSheet;
  [self presentOnTopWhenReady:picker];
}

- (void)openFilePicker {
  _mode = PegglePickerMode::kFile;
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeItem ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.modalPresentationStyle = UIModalPresentationFormSheet;
  [self presentOnTopWhenReady:picker];
}

- (void)onTapFolder {
  [self openFolderPicker];
}

- (void)onTapFile {
  [self openFilePicker];
}

- (void)onTapCancel {
  _callback = nullptr;
  [self dismissOverlay];
}

- (void)showErrorAlertWithTitle:(NSString*)title
                        message:(NSString*)message
              offerFolderButton:(BOOL)offerFolder {
  dispatch_async(dispatch_get_main_queue(), ^{
    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:title
                                            message:message
                                     preferredStyle:UIAlertControllerStyleAlert];
    if (offerFolder) {
      [alert addAction:[UIAlertAction actionWithTitle:@"Select Game Folder"
                                                style:UIAlertActionStyleDefault
                                              handler:^(UIAlertAction*) {
                                                [self openFolderPicker];
                                              }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:offerFolder ? @"Cancel" : @"OK"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [self presentOnTopWhenReady:alert];
  });
}

- (void)finishSelectionWithPath:(NSString*)chosenPath {
  const std::string path_str(chosenPath.UTF8String ? chosenPath.UTF8String : "");
  const BOOL wasMidSession = _allowCancel;
  auto cb = std::move(_callback);
  _callback = nullptr;
  [self dismissOverlay];
  if (!wasMidSession) {
    rex_ios_init_vfs_case_map_for_path(path_str);
  }
  if (cb) {
    cb(path_str);
  }
  if (wasMidSession) {
    const std::string docs = rex_ios_get_documents_dir();
    NSString* docsGame =
        !docs.empty() ? [@(docs.c_str()) stringByAppendingPathComponent:@"game"] : nil;
    const BOOL docsGameActive = docsGame && (FindGameSourceInDir(docsGame) != nil);
    NSString* msg = docsGameActive
        ? @"Game source bookmark saved.\n\nNote: files currently inside Files → Peggle 2 → game "
          @"take startup priority. Remove them in the Files app to boot your bookmarked source, "
          @"then reopen Peggle 2."
        : @"Game source saved.\n\nClose and reopen Peggle 2 to boot from the newly selected "
          @"game source.";
    dispatch_async(dispatch_get_main_queue(), ^{
      UIAlertController* alert =
          [UIAlertController alertControllerWithTitle:@"Game Source Updated"
                                              message:msg
                                       preferredStyle:UIAlertControllerStyleAlert];
      [alert addAction:[UIAlertAction actionWithTitle:@"Quit App Now"
                                                style:UIAlertActionStyleDestructive
                                              handler:^(UIAlertAction*) {
                                                std::exit(0);
                                              }]];
      [alert addAction:[UIAlertAction actionWithTitle:@"Continue Playing"
                                                style:UIAlertActionStyleCancel
                                              handler:nil]];
      [self presentOnTopWhenReady:alert];
    });
  }
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  (void)controller;
  NSURL* url = urls.firstObject;
  if (!url) return;

  const BOOL scoped = [url startAccessingSecurityScopedResource];
  NSString* path = url.path;
  if (!path || path.length == 0) {
    if (scoped) [url stopAccessingSecurityScopedResource];
    [self showErrorAlertWithTitle:@"Unavailable File"
                          message:@"Selected item does not expose a readable local path."
                offerFolderButton:NO];
    return;
  }

  if (_mode == PegglePickerMode::kFolder) {
    NSString* errMsg = nil;
    if (!ValidateGameFolderPath(path, &errMsg)) {
      if (scoped) [url stopAccessingSecurityScopedResource];
      [self showErrorAlertWithTitle:@"Invalid Game Folder"
                            message:errMsg ? errMsg : @"Selected folder is missing Default.xex."
                  offerFolderButton:YES];
      return;
    }
    SaveSecurityScopedBookmark(url);
    ActivateSessionSecurityScopedURL(url, scoped, !_allowCancel);
    [self finishSelectionWithPath:path];
    return;
  }

  // PegglePickerMode::kFile (.iso, XBLA STFS container, or .xex)
  NSString* ext = path.pathExtension.lowercaseString;
  if ([ext isEqualToString:@"iso"]) {
    NSString* errMsg = nil;
    if (!ValidateXboxIsoFile(path, &errMsg)) {
      if (scoped) [url stopAccessingSecurityScopedResource];
      [self showErrorAlertWithTitle:@"Invalid .ISO Image"
                            message:errMsg ? errMsg : @"Selected .iso could not be validated."
                  offerFolderButton:NO];
      return;
    }
    SaveSecurityScopedBookmark(url);
    ActivateSessionSecurityScopedURL(url, scoped, !_allowCancel);
    [self finishSelectionWithPath:path];
    return;
  }

  bool stfsNeedsFolder = false;
  NSString* stfsErr = nil;
  if (ValidateStfsContainerFile(path, &stfsNeedsFolder, &stfsErr)) {
    SaveSecurityScopedBookmark(url);
    ActivateSessionSecurityScopedURL(url, scoped, !_allowCancel);
    [self finishSelectionWithPath:path];
    return;
  }
  if (stfsNeedsFolder || stfsErr) {
    if (scoped) [url stopAccessingSecurityScopedResource];
    [self showErrorAlertWithTitle:stfsNeedsFolder ? @"Game Folder Required" : @"Invalid XBLA Container"
                          message:stfsErr
                offerFolderButton:stfsNeedsFolder];
    return;
  }

  if ([ext isEqualToString:@"xex"]) {
    NSURL* parentURL = [url URLByDeletingLastPathComponent];
    NSString* parentPath = parentURL.path;
    NSError* dirErr = nil;
    NSArray<NSString*>* siblings =
        parentPath ? [[NSFileManager defaultManager] contentsOfDirectoryAtPath:parentPath
                                                                         error:&dirErr]
                   : nil;
    BOOL hasReadableSibling = NO;
    BOOL mainRsbBlocked = NO;
    if (siblings && !dirErr) {
      for (NSString* name in siblings) {
        NSString* lower = name.lowercaseString;
        if ([lower isEqualToString:@"default.xex"]) continue;
        NSString* sibPath = [parentPath stringByAppendingPathComponent:name];
        BOOL sibDir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:sibPath isDirectory:&sibDir] &&
            !sibDir) {
          const BOOL ok = CanOpenReadableFile(sibPath);
          if ([lower isEqualToString:@"main.rsb"]) {
            if (!ok) {
              mainRsbBlocked = YES;
              hasReadableSibling = NO;
              break;
            }
            hasReadableSibling = YES;
          } else if (ok) {
            hasReadableSibling = YES;
          }
        }
      }
    }
    if (hasReadableSibling && !mainRsbBlocked && ValidateGameFolderPath(parentPath, nil)) {
      SaveSecurityScopedBookmark(parentURL);
      ActivateSessionSecurityScopedURL(url, scoped, !_allowCancel);
      [self finishSelectionWithPath:parentPath];
      return;
    }
    if (scoped) [url stopAccessingSecurityScopedResource];
    [self showErrorAlertWithTitle:@"Game Folder Required"
                          message:@"iOS App Sandbox restricts access to sibling game assets "
                                  @"(such as main.rsb) when selecting a single .xex file.\n\n"
                                  @"Please select the containing Game Folder (or a single .iso "
                                  @"file) instead."
                offerFolderButton:YES];
    return;
  }

  if (scoped) [url stopAccessingSecurityScopedResource];
  [self showErrorAlertWithTitle:@"Unsupported File"
                        message:@"Please select a Peggle 2 .iso image, XBLA STFS container, or tap "
                                @"\"Select Game Folder\" to pick an extracted game directory."
              offerFolderButton:YES];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  (void)controller;
}

@end

void rex_ios_present_game_picker(bool allow_cancel,
                                 std::function<void(const std::string&)> on_selected) {
  g_picker_overlay_active.store(true, std::memory_order_release);
  auto shared_cb = std::make_shared<std::function<void(const std::string&)>>(std::move(on_selected));
  dispatch_async(dispatch_get_main_queue(), ^{
    [[PeggleGamePickerCoordinator shared]
        presentWithAllowCancel:(allow_cancel ? YES : NO)
                      callback:[shared_cb](const std::string& p) {
                        if (*shared_cb) (*shared_cb)(p);
                      }];
  });
}

bool rex_ios_is_game_picker_active() {
  return g_picker_overlay_active.load(std::memory_order_acquire);
}

static char g_ios_crash_path[1024]{};
static std::atomic<bool> g_host_crash_written{false};
static std::atomic<bool> g_crash_handlers_installed{false};
static NSUncaughtExceptionHandler* g_prev_exception_handler = nullptr;

static void WriteRawToCrashFile(int oflags, const char* data, size_t len) {
  if (g_ios_crash_path[0] == '\0' || !data || len == 0) return;
  const int fd = open(g_ios_crash_path, oflags, 0644);
  if (fd < 0) return;
  size_t written = 0;
  while (written < len) {
    const ssize_t rc = write(fd, data + written, len - written);
    if (rc > 0) {
      written += static_cast<size_t>(rc);
    } else if (rc < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  close(fd);
}

static size_t SigAppStr(char* dst, size_t pos, size_t cap, const char* s, size_t max_chars = 512) {
  if (!s || pos + 1 >= cap) return pos;
  size_t i = 0;
  while (s[i] != '\0' && i < max_chars && pos + 1 < cap) {
    dst[pos++] = s[i++];
  }
  dst[pos] = '\0';
  return pos;
}

static size_t SigAppHex(char* dst, size_t pos, size_t cap, uint64_t val) {
  char tmp[16];
  int len = 0;
  do {
    const uint64_t d = val & 0xFULL;
    tmp[len++] = static_cast<char>(d < 10 ? ('0' + d) : ('A' + (d - 10)));
    val >>= 4;
  } while (val != 0 && len < 16);
  for (int i = len - 1; i >= 0 && pos + 1 < cap; --i) {
    dst[pos++] = tmp[i];
  }
  dst[pos] = '\0';
  return pos;
}

static size_t SigAppU64(char* dst, size_t pos, size_t cap, uint64_t val) {
  char tmp[24];
  int len = 0;
  do {
    tmp[len++] = static_cast<char>('0' + (val % 10));
    val /= 10;
  } while (val != 0 && len < 24);
  for (int i = len - 1; i >= 0 && pos + 1 < cap; --i) {
    dst[pos++] = tmp[i];
  }
  dst[pos] = '\0';
  return pos;
}

static size_t SigAppDlSym(char* dst, size_t pos, size_t cap, uint64_t addr, const char* prefix = nullptr) {
  if (!addr) return pos;
  Dl_info dli{};
  if (dladdr(reinterpret_cast<const void*>(addr), &dli) == 0 || !dli.dli_fbase) {
    return pos;
  }
  if (prefix) {
    pos = SigAppStr(dst, pos, cap, prefix, 16);
  }
  if (dli.dli_sname && dli.dli_sname[0] != '\0') {
    const uint64_t base = dli.dli_saddr ? reinterpret_cast<uintptr_t>(dli.dli_saddr)
                                        : reinterpret_cast<uintptr_t>(dli.dli_fbase);
    pos = SigAppStr(dst, pos, cap, dli.dli_sname, 360);
    pos = SigAppStr(dst, pos, cap, "+0x", 3);
    pos = SigAppHex(dst, pos, cap, addr >= base ? (addr - base) : 0ULL);
  } else {
    const char* img = dli.dli_fname ? dli.dli_fname : "?";
    for (const char* p = img; *p != '\0'; ++p) {
      if (*p == '/') img = p + 1;
    }
    const uint64_t fbase = reinterpret_cast<uintptr_t>(dli.dli_fbase);
    pos = SigAppStr(dst, pos, cap, img, 360);
    pos = SigAppStr(dst, pos, cap, "+0x", 3);
    pos = SigAppHex(dst, pos, cap, addr >= fbase ? (addr - fbase) : 0ULL);
  }
  return pos;
}

static uint64_t StripPtrAuthAddr(uint64_t addr) {
#if defined(__aarch64__) || defined(__arm64__)
#if defined(__has_feature)
#if __has_feature(ptrauth_calls) || __has_feature(ptrauth_returns)
  addr = reinterpret_cast<uintptr_t>(
      ptrauth_strip(reinterpret_cast<void*>(addr), ptrauth_key_function_pointer));
#endif
#endif
#endif
  return addr;
}

static void IosUncaughtExceptionHandler(NSException* ex) {
  g_host_crash_written.store(true, std::memory_order_release);
  const rex::RexBootTelemetrySnapshot tel = rex::GetBootTelemetrySnapshot();
  std::string report = RexIosFormatCrashReport(tel);
  report += "\nHostException: ";
  report += (ex && ex.name && ex.name.UTF8String) ? ex.name.UTF8String : "Unknown";
  report += ": ";
  report += (ex && ex.reason && ex.reason.UTF8String) ? ex.reason.UTF8String : "-";
  report += "\nHostStack:\n";
  if (ex) {
    for (NSString* sym in ex.callStackSymbols) {
      report += "  ";
      report += sym.UTF8String ? sym.UTF8String : "?";
      report += '\n';
    }
  }
  WriteRawToCrashFile(O_WRONLY | O_CREAT | O_TRUNC, report.data(), report.size());
  if (g_prev_exception_handler && g_prev_exception_handler != &IosUncaughtExceptionHandler) {
    g_prev_exception_handler(ex);
  }
}

static void IosHostSignalHandler(int sig, siginfo_t* info, void* uctx) {
  uint64_t host_pc = 0, host_lr = 0, host_x0 = 0, host_x1 = 0, host_x19 = 0, host_x20 = 0;
  uint32_t fault_insn = 0;
#if defined(__aarch64__) || defined(__arm64__)
  if (uctx) {
    mcontext_t mc = reinterpret_cast<ucontext_t*>(uctx)->uc_mcontext;
    if (mc) {
      host_x0 = mc->__ss.__x[0];
      host_x1 = mc->__ss.__x[1];
      host_x19 = mc->__ss.__x[19];
      host_x20 = mc->__ss.__x[20];
#if defined(__DARWIN_OPAQUE_ARM_THREAD_STATE64) && __DARWIN_OPAQUE_ARM_THREAD_STATE64
      host_pc = reinterpret_cast<uintptr_t>(mc->__ss.__opaque_pc);
      host_lr = reinterpret_cast<uintptr_t>(mc->__ss.__opaque_lr);
#else
      host_pc = mc->__ss.__pc;
      host_lr = mc->__ss.__lr;
#endif
      host_pc = StripPtrAuthAddr(host_pc);
      host_lr = StripPtrAuthAddr(host_lr);
    }
  }
#else
  (void)uctx;
#endif

  char thr_name[32]{};
  pthread_getname_np(pthread_self(), thr_name, sizeof(thr_name));

  Dl_info pc_dl{};
  const bool have_pc_dl = (host_pc != 0) &&
                          (dladdr(reinterpret_cast<const void*>(host_pc), &pc_dl) != 0) &&
                          (pc_dl.dli_fbase != nullptr);
  if (have_pc_dl && (host_pc & 3ULL) == 0 && host_pc >= reinterpret_cast<uintptr_t>(pc_dl.dli_fbase)) {
    fault_insn = *reinterpret_cast<const uint32_t*>(host_pc);
  }

  char host_sym[512]{};
  if (have_pc_dl) {
    SigAppDlSym(host_sym, 0, sizeof(host_sym), host_pc);
  } else if (host_lr != 0) {
    SigAppDlSym(host_sym, 0, sizeof(host_sym), host_lr, "LR:");
  }

  if (!g_host_crash_written.exchange(true, std::memory_order_acq_rel)) {
    rex::RecordGuestSignalFault(
        sig, info ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info->si_addr)) : 0ULL,
        host_pc, host_lr, fault_insn, rex::GetThreadGuestPC(), rex::GetThreadGuestLR(),
        host_sym[0] ? host_sym : nullptr, thr_name[0] ? thr_name : nullptr,
        host_x0, host_x1, host_x19, host_x20);

    void* frames[32]{};
    const int nframes = backtrace(frames, 32);
    char stack_buf[4096];
    size_t spos = 0;
    const size_t scap = sizeof(stack_buf);
    spos = SigAppStr(stack_buf, spos, scap, "HostStack:\n");
    for (int i = 0; i < nframes && spos + 16 < scap; ++i) {
      const uint64_t addr = StripPtrAuthAddr(reinterpret_cast<uintptr_t>(frames[i]));
      spos = SigAppStr(stack_buf, spos, scap, "  [");
      spos = SigAppU64(stack_buf, spos, scap, static_cast<uint64_t>(i));
      spos = SigAppStr(stack_buf, spos, scap, "] 0x");
      spos = SigAppHex(stack_buf, spos, scap, addr);
      spos = SigAppStr(stack_buf, spos, scap, " ");
      spos = SigAppDlSym(stack_buf, spos, scap, addr);
      spos = SigAppStr(stack_buf, spos, scap, "\n");
    }
    WriteRawToCrashFile(O_WRONLY | O_CREAT | O_APPEND, stack_buf, spos);
  } else {
    char tail[768];
    size_t pos = 0;
    const size_t cap = sizeof(tail);
    pos = SigAppStr(tail, pos, cap, "\nFollowupSignal: Sig=");
    pos = SigAppU64(tail, pos, cap, static_cast<uint64_t>(sig));
    pos = SigAppStr(tail, pos, cap, " Thr=");
    pos = SigAppStr(tail, pos, cap, thr_name[0] ? thr_name : "-", 32);
    pos = SigAppStr(tail, pos, cap, " PC=0x");
    pos = SigAppHex(tail, pos, cap, host_pc);
    pos = SigAppStr(tail, pos, cap, " Sym=");
    pos = SigAppStr(tail, pos, cap, host_sym[0] ? host_sym : "-", 400);
    pos = SigAppStr(tail, pos, cap, "\n");
    WriteRawToCrashFile(O_WRONLY | O_CREAT | O_APPEND, tail, pos);
  }

  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  sigset_t unblk;
  sigemptyset(&unblk);
  sigaddset(&unblk, sig);
  sigprocmask(SIG_UNBLOCK, &unblk, nullptr);
  if (sig == SIGABRT) {
    raise(sig);
  }
}

static void InstallIosHostCrashHandlers(const std::string& crash_path) {
  if (!crash_path.empty()) {
    const size_t copy_len = std::min(crash_path.size(), sizeof(g_ios_crash_path) - 1);
    std::memcpy(g_ios_crash_path, crash_path.data(), copy_len);
    g_ios_crash_path[copy_len] = '\0';
  }
  if (!g_crash_handlers_installed.exchange(true, std::memory_order_acq_rel)) {
    g_prev_exception_handler = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(&IosUncaughtExceptionHandler);
    struct sigaction sa{};
    sa.sa_sigaction = &IosHostSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
  }
}

static std::atomic<bool> g_low_power_auto_scale_enabled{true};
static std::atomic<bool> g_low_power_mode_active{false};
static std::atomic<bool> g_thermal_throttle_active{false};

static void UpdatePowerAndThermalState() {
  NSProcessInfo* pi = [NSProcessInfo processInfo];
  g_low_power_mode_active.store(pi.isLowPowerModeEnabled ? true : false, std::memory_order_relaxed);
  g_thermal_throttle_active.store(pi.thermalState >= NSProcessInfoThermalStateSerious,
                                  std::memory_order_relaxed);
}

void rex_ios_set_target_fps(uint32_t hz) {
  const uint32_t clamped = (hz > 0 && hz <= 40) ? 40u : ((hz == 0 || hz >= 120) ? 120u : 60u);
  if (g_ios_target_fps.exchange(clamped, std::memory_order_relaxed) == clamped) {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    UIViewController* root = g_root_vc;
    if ([root isKindOfClass:[PeggleViewController class]]) {
      [(PeggleViewController*)root updateDisplayLinkFrameRate:clamped];
    }
  });
}

void rex_ios_set_low_power_auto_scale_enabled(bool enabled) {
  g_low_power_auto_scale_enabled.store(enabled, std::memory_order_relaxed);
}

bool rex_ios_is_low_power_075x_active() {
  if (!g_low_power_auto_scale_enabled.load(std::memory_order_relaxed)) {
    return false;
  }
  UpdatePowerAndThermalState();
  return g_low_power_mode_active.load(std::memory_order_relaxed) ||
         g_thermal_throttle_active.load(std::memory_order_relaxed);
}

@interface PeggleAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) PeggleViewController* viewController;
@end

@implementation PeggleAppDelegate {
  std::unique_ptr<IOSWindowedAppContext> _appCtx;
  std::unique_ptr<rex::ui::WindowedApp> _app;
}

- (void)onPowerOrThermalChanged:(NSNotification*)note {
  (void)note;
  UpdatePowerAndThermalState();
}

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  (void)application;
  (void)launchOptions;
  UpdatePowerAndThermalState();
  NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
  [nc addObserver:self
         selector:@selector(onPowerOrThermalChanged:)
             name:NSProcessInfoPowerStateDidChangeNotification
           object:nil];
  [nc addObserver:self
         selector:@selector(onPowerOrThermalChanged:)
             name:NSProcessInfoThermalStateDidChangeNotification
           object:nil];

  // Ensure Documents folder exists so it appears immediately in Files -> On My iPhone -> Peggle 2.
  const std::string docs = rex_ios_get_documents_dir();
  if (!docs.empty()) {
    [[NSFileManager defaultManager] createDirectoryAtPath:@(docs.c_str())
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    const std::string crash_path = docs + "/last_crash.txt";
    rex::SetCrashReportPath(crash_path.c_str());
    InstallIosHostCrashHandlers(crash_path);
  } else {
    InstallIosHostCrashHandlers(std::string());
  }
  rex_ios_init_vfs_case_map();

  self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
  self.viewController = [[PeggleViewController alloc] init];
  self.window.rootViewController = self.viewController;
  [self.window makeKeyAndVisible];

  _appCtx = std::make_unique<IOSWindowedAppContext>();
  _appCtx->SetMainThread();
  g_ios_app_ctx = _appCtx.get();

  _app = rex::ui::GetWindowedAppCreator()(*_appCtx);
  if (!_app || !_app->OnInitialize()) {
    REXLOG_ERROR("PeggleAppDelegate: ReXApp OnInitialize failed");
    rex::SetBootError(!_app ? "WindowedAppCreator null" : "OnInitialize failed");
  }
  return YES;
}

- (void)pauseRuntime {
  [self.viewController setRenderPaused:YES];
  rex_ios_audio_set_paused(true);
  rex::SetAppBackgroundPaused(true);
}

- (void)resumeRuntime {
  rex::SetAppBackgroundPaused(false);
  rex_ios_audio_set_paused(false);
  BumpUiRedrawWindow(350);
  [self.viewController setRenderPaused:NO];
}

- (void)applicationWillResignActive:(UIApplication*)application {
  (void)application;
  [self pauseRuntime];
}

- (void)applicationDidEnterBackground:(UIApplication*)application {
  (void)application;
  [self pauseRuntime];
  rex_ios_wait_for_metal_idle();
}

- (void)applicationWillEnterForeground:(UIApplication*)application {
  (void)application;
  [self resumeRuntime];
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
  (void)application;
  [self resumeRuntime];
}

- (void)applicationWillTerminate:(UIApplication*)application {
  (void)application;
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [self.viewController setRenderPaused:YES];
  rex_ios_audio_set_paused(true);
  rex_ios_wait_for_metal_idle();
  // Unpark workers and guest threads before InvokeOnDestroy joins CommandProcessor.
  rex::SetAppBackgroundPaused(false);
  if (_app) {
    _app->InvokeOnDestroy();
    _app.reset();
  }
  g_ios_app_ctx = nullptr;
  _appCtx.reset();
}

@end

const char* rex_ios_get_thermal_state_name() {
  switch ([NSProcessInfo processInfo].thermalState) {
    case NSProcessInfoThermalStateNominal: return "Nominal";
    case NSProcessInfoThermalStateFair: return "Fair";
    case NSProcessInfoThermalStateSerious: return "Serious";
    case NSProcessInfoThermalStateCritical: return "Critical";
    default: return "Unknown";
  }
}

int main(int argc, char* argv[]) {
  rex::ResetStartupTrace();
#ifdef REX_BUILD_SHA
  rex::SetBuildSha(REX_BUILD_SHA);
#endif
  rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  @autoreleasepool {
    const std::string early_docs = rex_ios_get_documents_dir();
    if (!early_docs.empty()) {
      const std::string early_crash = early_docs + "/last_crash.txt";
      rex::SetCrashReportPath(early_crash.c_str());
      InstallIosHostCrashHandlers(early_crash);
    } else {
      InstallIosHostCrashHandlers(std::string());
    }
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([PeggleAppDelegate class]));
  }
}
