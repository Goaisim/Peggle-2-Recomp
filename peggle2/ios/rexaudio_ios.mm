// Native iOS CoreAudio (RemoteIO + AVAudioSession) backend implementing rex::audio::AudioSystem.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include "ios_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <rex/ui/presenter.h>

namespace {

using rex::X_RESULT;
using rex::X_STATUS;

class IOSAudioDriver;

static std::mutex g_drivers_mutex;
static std::condition_variable g_drivers_cv;
static std::vector<IOSAudioDriver*> g_drivers;
static std::atomic<bool> g_global_app_paused{false};

constexpr size_t kFrameChannels = 6;
constexpr size_t kChannelSamples = 256;
constexpr size_t kFrameSamples = kFrameChannels * kChannelSamples;
constexpr uint32_t kSampleRate = 48000;

class IOSAudioDriver final : public rex::audio::AudioDriver {
 public:
  IOSAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}

  ~IOSAudioDriver() override { Shutdown(); }

  bool Initialize() {
    @autoreleasepool {
      AVAudioSession* session = [AVAudioSession sharedInstance];
      NSError* err = nil;
      // SoloAmbient respects the hardware silent switch and prevents background audio bleed.
      [session setCategory:AVAudioSessionCategorySoloAmbient error:&err];
      [session setPreferredSampleRate:double(kSampleRate) error:nil];
      [session setPreferredIOBufferDuration:double(kChannelSamples) / double(kSampleRate)
                                      error:nil];
      [session setActive:YES error:&err];
    }

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) {
      REXLOG_WARN("IOSAudioDriver: RemoteIO unavailable; falling back to silent audio pacing");
      unit_ = nullptr;
      silent_fallback_ = true;
      RegisterSelf();
      return true;
    }

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = double(kSampleRate);
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 32;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 8;
    fmt.mBytesPerPacket = 8;

    AURenderCallbackStruct cb{};
    cb.inputProc = &IOSAudioDriver::RenderCallback;
    cb.inputProcRefCon = this;

    const bool start_paused = g_global_app_paused.load(std::memory_order_acquire);
    app_paused_.store(start_paused, std::memory_order_release);
    interrupted_.store(false, std::memory_order_release);
    effective_paused_.store(start_paused, std::memory_order_release);

    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                             sizeof(fmt)) != noErr ||
        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &cb, sizeof(cb)) != noErr ||
        AudioUnitInitialize(unit_) != noErr ||
        (!start_paused && AudioOutputUnitStart(unit_) != noErr)) {
      REXLOG_WARN("IOSAudioDriver: RemoteIO start failed; falling back to silent audio pacing");
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
      silent_fallback_ = true;
      initialized_.store(true, std::memory_order_release);
      RegisterSelf();
      return true;
    }
    silent_fallback_ = false;
    initialized_.store(true, std::memory_order_release);
    RegisterObservers();
    RegisterSelf();
    if (g_global_app_paused.load(std::memory_order_acquire) != start_paused) {
      SetAppPaused(g_global_app_paused.load(std::memory_order_acquire));
    }
    return true;
  }

  void Shutdown() {
    {
      std::unique_lock<std::mutex> lock(g_drivers_mutex);
      initialized_.store(false, std::memory_order_release);
      g_drivers.erase(std::remove(g_drivers.begin(), g_drivers.end(), this), g_drivers.end());
      g_drivers_cv.wait(lock, [this]() {
        return active_callbacks_.load(std::memory_order_acquire) == 0;
      });
    }
    UnregisterObservers();
    silent_fallback_ = false;
    {
      std::lock_guard<std::mutex> ulock(unit_mutex_);
      if (unit_) {
        AudioOutputUnitStop(unit_);
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
      }
    }
    FlushQueuedFrames(false);
  }

  bool TryBeginExternalOp() {
    if (!initialized_.load(std::memory_order_acquire)) return false;
    active_callbacks_.fetch_add(1, std::memory_order_acq_rel);
    if (!initialized_.load(std::memory_order_acquire)) {
      EndExternalOp();
      return false;
    }
    return true;
  }

  void EndExternalOp() {
    if (active_callbacks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      std::lock_guard<std::mutex> lock(g_drivers_mutex);
      g_drivers_cv.notify_all();
    }
  }

  void SetAppPaused(bool paused) {
    if (!initialized_.load(std::memory_order_acquire)) return;
    app_paused_.store(paused, std::memory_order_release);
    UpdatePauseState();
  }

  void SetInterrupted(bool interrupted) {
    if (!initialized_.load(std::memory_order_acquire)) return;
    interrupted_.store(interrupted, std::memory_order_release);
    UpdatePauseState();
  }

  void HandleRouteChange() {
    if (!initialized_.load(std::memory_order_acquire) ||
        effective_paused_.load(std::memory_order_acquire) || silent_fallback_) {
      return;
    }
    RestartOutputUnit(false);
  }

  void SubmitFrame(uint32_t frame_ptr) override {
    if (effective_paused_.load(std::memory_order_acquire)) {
      if (semaphore_) {
        semaphore_->Release(1, nullptr);
      }
      return;
    }
    if (silent_fallback_ || !unit_) {
      rex::ui::Presenter::RecordSubmittedAudioFrame();
      // Pace guest XMA callbacks at ~187.5 Hz (256 samples @ 48 kHz = 5.333 ms) without hardware.
      std::this_thread::sleep_for(std::chrono::microseconds(5333));
      if (semaphore_) {
        semaphore_->Release(1, nullptr);
      }
      return;
    }
    const auto* input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
    std::lock_guard<std::mutex> lock(submit_mutex_);
    if (effective_paused_.load(std::memory_order_acquire)) {
      if (semaphore_) semaphore_->Release(1, nullptr);
      return;
    }
    const uint32_t w = write_idx_.load(std::memory_order_relaxed);
    const uint32_t r = read_idx_.load(std::memory_order_acquire);
    if (w - r >= kQueueCapacity) {
      // Ring full: drop oldest-uncommitted incoming frame rather than blocking or racing read_idx_.
      if (semaphore_) semaphore_->Release(1, nullptr);
      return;
    }

    float* out_buf = frames_[w & kQueueMask].data();
    // Sanitize NaN/Inf and tanhf soft-clip in guest BE float space so heavy bursts never hard-clip.
    for (size_t i = 0; i < kFrameSamples; ++i) {
      float s = rex::byte_swap(input_frame[i]);
      if (!std::isfinite(s)) {
        s = 0.0f;
      } else {
        s = std::tanhf(s * 0.8f);
      }
      out_buf[i] = rex::byte_swap(s);
    }

    rex::ui::Presenter::RecordSubmittedAudioFrame();
    write_idx_.store(w + 1u, std::memory_order_release);
  }

 private:
  void RegisterSelf() {
    std::lock_guard<std::mutex> lock(g_drivers_mutex);
    if (std::find(g_drivers.begin(), g_drivers.end(), this) == g_drivers.end()) {
      g_drivers.push_back(this);
    }
  }

  void RegisterObservers() {
    @autoreleasepool {
      NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
      IOSAudioDriver* self_ptr = this;
      interruption_obs_ = [nc
          addObserverForName:AVAudioSessionInterruptionNotification
                      object:[AVAudioSession sharedInstance]
                       queue:nil
                  usingBlock:^(NSNotification* note) {
                    NSNumber* type_num = note.userInfo[AVAudioSessionInterruptionTypeKey];
                    if (!type_num) return;
                    {
                      std::lock_guard<std::mutex> lock(g_drivers_mutex);
                      if (std::find(g_drivers.begin(), g_drivers.end(), self_ptr) == g_drivers.end() ||
                          !self_ptr->TryBeginExternalOp()) {
                        return;
                      }
                    }
                    if (type_num.unsignedIntegerValue == AVAudioSessionInterruptionTypeBegan) {
                      self_ptr->SetInterrupted(true);
                    } else if (type_num.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded) {
                      self_ptr->SetInterrupted(false);
                    }
                    self_ptr->EndExternalOp();
                  }];
      route_obs_ = [nc
          addObserverForName:AVAudioSessionRouteChangeNotification
                      object:[AVAudioSession sharedInstance]
                       queue:nil
                  usingBlock:^(NSNotification* note) {
                    NSNumber* reason = note.userInfo[AVAudioSessionRouteChangeReasonKey];
                    if (!reason) return;
                    const NSUInteger r = reason.unsignedIntegerValue;
                    if (r == AVAudioSessionRouteChangeReasonOldDeviceUnavailable ||
                        r == AVAudioSessionRouteChangeReasonNewDeviceAvailable ||
                        r == AVAudioSessionRouteChangeReasonCategoryChange) {
                      {
                        std::lock_guard<std::mutex> lock(g_drivers_mutex);
                        if (std::find(g_drivers.begin(), g_drivers.end(), self_ptr) == g_drivers.end() ||
                            !self_ptr->TryBeginExternalOp()) {
                          return;
                        }
                      }
                      self_ptr->HandleRouteChange();
                      self_ptr->EndExternalOp();
                    }
                  }];
    }
  }

  void UnregisterObservers() {
    @autoreleasepool {
      NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
      if (interruption_obs_) {
        [nc removeObserver:interruption_obs_];
        interruption_obs_ = nil;
      }
      if (route_obs_) {
        [nc removeObserver:route_obs_];
        route_obs_ = nil;
      }
    }
  }

  void FlushQueuedFrames(bool release_semaphores) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    const uint32_t r = read_idx_.load(std::memory_order_acquire);
    const uint32_t w = write_idx_.load(std::memory_order_relaxed);
    if (release_semaphores && semaphore_) {
      for (uint32_t i = r; i != w; ++i) {
        semaphore_->Release(1, nullptr);
      }
    }
    read_idx_.store(w, std::memory_order_release);
    current_offset_ = 0;
    downmixed_ready_ = false;
    stereo_scratch_.fill(0.0f);
  }

  void RestartOutputUnit(bool reactivate_session) {
    if (!initialized_.load(std::memory_order_acquire)) return;
    if (reactivate_session) {
      @autoreleasepool {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* err = nil;
        [session setCategory:AVAudioSessionCategorySoloAmbient error:&err];
        [session setPreferredSampleRate:double(kSampleRate) error:nil];
        [session setPreferredIOBufferDuration:double(kChannelSamples) / double(kSampleRate)
                                        error:nil];
        [session setActive:YES error:&err];
      }
    }
    std::lock_guard<std::mutex> ulock(unit_mutex_);
    if (!unit_ || !initialized_.load(std::memory_order_acquire)) return;
    if (AudioOutputUnitStart(unit_) != noErr) {
      AudioOutputUnitStop(unit_);
      AudioUnitUninitialize(unit_);
      if (AudioUnitInitialize(unit_) == noErr) {
        AudioOutputUnitStart(unit_);
      }
    }
  }

  void UpdatePauseState() {
    if (!initialized_.load(std::memory_order_acquire)) return;
    const bool should_pause =
        app_paused_.load(std::memory_order_acquire) || interrupted_.load(std::memory_order_acquire);
    const bool prev = effective_paused_.exchange(should_pause, std::memory_order_acq_rel);
    if (should_pause == prev) return;
    if (should_pause) {
      std::lock_guard<std::mutex> ulock(unit_mutex_);
      if (unit_) {
        AudioOutputUnitStop(unit_);
      }
    } else {
      // Flush stale queued XMA audio before restarting output so sound doesn't crackle or lag.
      FlushQueuedFrames(true);
      RestartOutputUnit(true);
    }
  }

 private:
  static OSStatus RenderCallback(void* refcon, AudioUnitRenderActionFlags*,
                                 const AudioTimeStamp*, UInt32, UInt32 num_frames,
                                 AudioBufferList* io_data) {
    if (!refcon || !io_data || io_data->mNumberBuffers == 0 || !io_data->mBuffers[0].mData) {
      return noErr;
    }
    auto* self = static_cast<IOSAudioDriver*>(refcon);
    auto* out = static_cast<float*>(io_data->mBuffers[0].mData);
    const UInt32 max_frames = io_data->mBuffers[0].mDataByteSize / UInt32(2 * sizeof(float));
    const UInt32 safe_frames = std::min(num_frames, max_frames);
    self->MixStereo(out, safe_frames);
    return noErr;
  }

  void MixStereo(float* out, UInt32 num_frames) {
    if (!out || num_frames == 0) return;
    std::memset(out, 0, size_t(num_frames) * 2 * sizeof(float));
    if (!initialized_.load(std::memory_order_acquire) ||
        effective_paused_.load(std::memory_order_acquire) ||
        stereo_scratch_.empty()) {
      return;
    }
    UInt32 written = 0;
    while (written < num_frames) {
      if (!downmixed_ready_) {
        const uint32_t r = read_idx_.load(std::memory_order_relaxed);
        const uint32_t w = write_idx_.load(std::memory_order_acquire);
        if (r == w) {
          break;
        }
        static const rex::audio::StereoFold kStereo{};
        rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
            stereo_scratch_.data(), frames_[r & kQueueMask].data(), kChannelSamples, kStereo, 1.0f);
        read_idx_.store(r + 1u, std::memory_order_release);
        current_offset_ = 0;
        downmixed_ready_ = true;
        if (semaphore_) semaphore_->Release(1, nullptr);
      }
      const UInt32 avail = UInt32(kChannelSamples - current_offset_);
      const UInt32 take = std::min(avail, num_frames - written);
      if ((size_t(current_offset_) + take) * 2 > stereo_scratch_.size()) break;
      for (UInt32 i = 0; i < take * 2; ++i) {
        const float v = std::tanhf(stereo_scratch_[current_offset_ * 2 + i]);
        out[written * 2 + i] = std::clamp(v, -1.0f, 1.0f);
      }
      written += take;
      current_offset_ += take;
      if (current_offset_ >= kChannelSamples) {
        current_offset_ = 0;
        downmixed_ready_ = false;
      }
    }
  }

  static constexpr uint32_t kQueueCapacity = 32;
  static constexpr uint32_t kQueueMask = kQueueCapacity - 1u;

  rex::thread::Semaphore* semaphore_ = nullptr;
  AudioComponentInstance unit_ = nullptr;
  std::mutex unit_mutex_;
  std::mutex submit_mutex_;
  std::array<std::array<float, kFrameSamples>, kQueueCapacity> frames_{};
  std::atomic<uint32_t> write_idx_{0};
  std::atomic<uint32_t> read_idx_{0};
  size_t current_offset_ = 0;
  bool downmixed_ready_ = false;
  bool silent_fallback_ = false;
  std::atomic<bool> initialized_{false};
  std::atomic<int> active_callbacks_{0};
  std::atomic<bool> app_paused_{false};
  std::atomic<bool> interrupted_{false};
  std::atomic<bool> effective_paused_{false};
  id interruption_obs_ = nil;
  id route_obs_ = nil;
  std::array<float, kChannelSamples * 2> stereo_scratch_{};
};

class IOSAudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit IOSAudioSystem(rex::runtime::FunctionDispatcher* fd) : AudioSystem(fd) {}
  ~IOSAudioSystem() override = default;

 protected:
  X_STATUS CreateDriver([[maybe_unused]] size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    auto* driver = new IOSAudioDriver(memory_, semaphore);
    if (!driver->Initialize()) {
      delete driver;
      return X_STATUS_UNSUCCESSFUL;
    }
    *out_driver = driver;
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    auto* ios_driver = dynamic_cast<IOSAudioDriver*>(driver);
    if (ios_driver) {
      ios_driver->Shutdown();
      delete ios_driver;
    }
  }
};

}  // namespace

std::unique_ptr<rex::audio::AudioSystem> rex_ios_create_audio_system(
    rex::runtime::FunctionDispatcher* fd) {
  return std::make_unique<IOSAudioSystem>(fd);
}

void rex_ios_audio_set_paused(bool paused) {
  g_global_app_paused.store(paused, std::memory_order_release);
  std::vector<IOSAudioDriver*> drivers_copy;
  {
    std::lock_guard<std::mutex> lock(g_drivers_mutex);
    drivers_copy.reserve(g_drivers.size());
    for (IOSAudioDriver* driver : g_drivers) {
      if (driver && driver->TryBeginExternalOp()) {
        drivers_copy.push_back(driver);
      }
    }
  }
  for (IOSAudioDriver* driver : drivers_copy) {
    driver->SetAppPaused(paused);
    driver->EndExternalOp();
  }
}
