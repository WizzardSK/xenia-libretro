/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Audio Driver
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * Implements xe::apu::AudioDriver and xe::apu::AudioSystem to capture
 * audio samples from Xenia and forward them to libretro's audio callback.
 */

#ifndef LIBRETRO_AUDIO_DRIVER_H
#define LIBRETRO_AUDIO_DRIVER_H

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"

namespace xe {
namespace apu {
namespace libretro {

// Ring buffer: Xenia float samples, interleaved in the output's channels.
class LibretroAudioRingBuffer {
 public:
  static constexpr size_t kCapacity = 48000 * 6;  // ~1 s stereo, ~170 ms 5.1

  void Push(const float* data, size_t count);
  size_t Pop(float* out, size_t max_samples);
  size_t Available() const;
  void Clear();

 private:
  float buffer_[kCapacity] = {};
  std::atomic<size_t> write_pos_{0};
  std::atomic<size_t> read_pos_{0};
};

class LibretroAudioSystem;

// AudioDriver: one per audio client (and one for the media player), each with
// a ring of its own. The frontend side mixes the rings (MixInto), as a sound
// device mixes its streams - pushing every driver into one shared ring played
// the clients one after another instead of together.
class LibretroAudioDriver : public AudioDriver {
 public:
  // An audio client's frames are 6 channels, big-endian and channel after
  // channel (need_format_conversion); the media player's are interleaved
  // little-endian floats with its own channel count and sample rate, as
  // upstream's SDL driver takes them.
  LibretroAudioDriver(LibretroAudioSystem* system,
                      xe::threading::Semaphore* semaphore, uint32_t frequency,
                      uint32_t channels, bool need_format_conversion);
  ~LibretroAudioDriver() override;

  bool Initialize() override;
  void Shutdown() override;

  void SubmitFrame(float* samples) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override;

  // Adds up to `samples` interleaved output samples of this driver's ring to
  // `mix`; returns how many it had.
  size_t MixInto(float* mix, size_t samples);

  // The audio system is going away before this driver.
  void Detach() { system_ = nullptr; }

 private:
  void Resample(const float* in, size_t frames);

  LibretroAudioSystem* system_ = nullptr;
  xe::threading::Semaphore* semaphore_ = nullptr;
  uint32_t frequency_ = 48000;
  uint32_t channels_ = 6;
  // The output's channels, 2 or 6, as the audio system's when made.
  uint32_t out_channels_ = 2;
  uint32_t channel_samples_ = kChannelSamplesDefault;
  bool need_format_conversion_ = true;
  LibretroAudioRingBuffer ring_;
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> paused_{false};
  // Linear resampling to 48 kHz: position between the last two input frames.
  double resample_pos_ = 0.0;
  float last_[6] = {};
};

// AudioSystem that creates LibretroAudioDriver instances.
class LibretroAudioSystem : public AudioSystem {
 public:
  explicit LibretroAudioSystem(cpu::Processor* processor);
  ~LibretroAudioSystem() override;

  static bool IsAvailable() { return true; }

  // Channels the frontend is given, 2 (stereo) or 6 (5.1 as FL FR C LFE SL
  // SR); set before the emulator starts, drivers take it when made.
  static void set_output_channels(uint32_t channels) {
    output_channels_ = channels == 6 ? 6 : 2;
  }
  static uint32_t output_channels() { return output_channels_; }

  std::string name() const override { return "libretro"; }

  AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                             uint32_t frequency, uint32_t channels,
                             bool need_format_conversion) override;
  void DestroyDriver(AudioDriver* driver) override;

  // Mixes every driver's queued audio into `out` (interleaved int16 in the
  // output's channels),
  // at most `max_samples` samples; returns how many it wrote.
  size_t Mix(int16_t* out, size_t max_samples);

  void Register(LibretroAudioDriver* driver);
  void Unregister(LibretroAudioDriver* driver);

 protected:
  X_STATUS CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                         AudioDriver** out_driver) override;

 private:
  std::mutex drivers_mutex_;
  std::vector<LibretroAudioDriver*> drivers_;
  std::vector<float> mix_;
  static inline std::atomic<uint32_t> output_channels_{2};
};

}  // namespace libretro
}  // namespace apu
}  // namespace xe

#endif  // LIBRETRO_AUDIO_DRIVER_H
