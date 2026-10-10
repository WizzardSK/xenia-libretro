/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Audio Driver Implementation
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#include "libretro_audio_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "xenia/apu/conversion.h"

namespace xe {
namespace apu {
namespace libretro {

// ---- Ring buffer ----------------------------------------------------------

void LibretroAudioRingBuffer::Push(const float* data, size_t count) {
  size_t wp = write_pos_.load(std::memory_order_relaxed);
  size_t rp = read_pos_.load(std::memory_order_acquire);

  // Drop oldest samples if buffer would overflow.
  size_t used = wp - rp;
  if (used + count > kCapacity) {
    read_pos_.store(wp + count - kCapacity, std::memory_order_release);
  }

  for (size_t i = 0; i < count; ++i) {
    buffer_[(wp + i) % kCapacity] = data[i];
  }
  write_pos_.store(wp + count, std::memory_order_release);
}

size_t LibretroAudioRingBuffer::Pop(float* out, size_t max_samples) {
  size_t rp = read_pos_.load(std::memory_order_relaxed);
  size_t wp = write_pos_.load(std::memory_order_acquire);
  size_t avail = std::min(wp - rp, kCapacity);
  size_t to_read = std::min(avail, max_samples);

  for (size_t i = 0; i < to_read; ++i) {
    out[i] = buffer_[(rp + i) % kCapacity];
  }
  read_pos_.store(rp + to_read, std::memory_order_release);
  return to_read;
}

size_t LibretroAudioRingBuffer::Available() const {
  size_t wp = write_pos_.load(std::memory_order_acquire);
  size_t rp = read_pos_.load(std::memory_order_relaxed);
  return wp - rp;
}

void LibretroAudioRingBuffer::Clear() {
  read_pos_.store(0, std::memory_order_relaxed);
  write_pos_.store(0, std::memory_order_relaxed);
}

// ---- LibretroAudioDriver --------------------------------------------------

LibretroAudioDriver::LibretroAudioDriver(LibretroAudioSystem* system,
                                         xe::threading::Semaphore* semaphore,
                                         uint32_t frequency, uint32_t channels,
                                         bool need_format_conversion)
    : system_(system),
      semaphore_(semaphore),
      frequency_(frequency ? frequency : 48000),
      channels_(channels == 2 ? 2 : 6),
      // As upstream's SDL driver: a frame is 256 samples of 6 channels or
      // 768 of 2, the same 1536 floats either way.
      channel_samples_(channels == 2 ? 768 : kChannelSamplesDefault),
      out_channels_(LibretroAudioSystem::output_channels()),
      need_format_conversion_(need_format_conversion) {
  if (system_) system_->Register(this);
}

LibretroAudioDriver::~LibretroAudioDriver() {
  if (system_) system_->Unregister(this);
}

bool LibretroAudioDriver::Initialize() { return true; }

void LibretroAudioDriver::Shutdown() {}

void LibretroAudioDriver::Resample(const float* in, size_t frames) {
  const size_t ch = out_channels_;
  if (frequency_ == 48000) {
    ring_.Push(in, frames * ch);
    return;
  }
  // Linear interpolation between consecutive input frames; last_ carries the
  // previous frame over from the last call so frames join without a click.
  const double step = double(frequency_) / 48000.0;
  float out[2048 * 6];
  size_t n = 0;
  while (resample_pos_ < double(frames)) {
    const size_t i = size_t(resample_pos_);
    const float t = float(resample_pos_ - double(i));
    const float* a = i == 0 ? last_ : &in[(i - 1) * ch];
    const float* b = &in[i * ch];
    for (size_t c = 0; c < ch; ++c) {
      out[n++] = a[c] + (b[c] - a[c]) * t;
    }
    if (n == sizeof(out) / sizeof(out[0])) {
      ring_.Push(out, n);
      n = 0;
    }
    resample_pos_ += step;
  }
  resample_pos_ -= double(frames);
  std::memcpy(last_, &in[(frames - 1) * ch], ch * sizeof(float));
  if (n) ring_.Push(out, n);
}

void LibretroAudioDriver::SubmitFrame(float* samples) {
  if (!paused_) {
    // Up to 768 frames (the media player's stereo) in up to 6 channels.
    float out[768 * 6];
    const size_t frames = channel_samples_;
    const size_t out_ch = out_channels_;
    float* stereo = out;
    if (out_ch == 6) {
      // 5.1 as the frontend takes it, FL FR C LFE SL SR interleaved - the
      // order the console's channels already are in.
      if (channels_ == 6 && need_format_conversion_) {
        conversion::sequential_6_BE_to_interleaved_6_LE(out, samples,
                                                        kChannelSamplesDefault);
      } else if (channels_ == 6) {
        std::memcpy(out, samples, frames * 6 * sizeof(float));
      } else {
        // Stereo music on the front pair.
        for (size_t i = 0; i < frames; ++i) {
          float* f = &out[i * 6];
          f[0] = samples[i * 2];
          f[1] = samples[i * 2 + 1];
          f[2] = f[3] = f[4] = f[5] = 0.0f;
        }
      }
    } else if (channels_ == 6 && need_format_conversion_) {
      conversion::sequential_6_BE_to_interleaved_2_LE(stereo, samples,
                                                      kChannelSamplesDefault);
    } else if (channels_ == 6) {
      // Interleaved little-endian 5.1 (FL FR C LFE SL SR), folded to stereo.
      for (size_t i = 0; i < frames; ++i) {
        const float* f = &samples[i * 6];
        const float c = f[2] * 0.7071f;
        stereo[i * 2] = f[0] + c + f[4] * 0.7071f;
        stereo[i * 2 + 1] = f[1] + c + f[5] * 0.7071f;
      }
    } else {
      std::memcpy(stereo, samples, frames * 2 * sizeof(float));
    }

    const float volume = volume_.load(std::memory_order_relaxed);
    if (volume != 1.0f) {
      for (size_t i = 0; i < frames * out_ch; ++i) {
        out[i] *= volume;
      }
    }

    // Wait while about 64 ms is queued: what is queued is latency, and the
    // frontend takes one frame's worth a call, so this is what paces the
    // game's audio. A second is only a safety net for a frontend that stops
    // calling.
    const size_t kThreshold = 48000 * out_ch * 64 / 1000;
    int wait_loops = 0;
    while (ring_.Available() > kThreshold) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (++wait_loops > 1000) break;
    }

    Resample(out, frames);
  }

  // Release the semaphore so Xenia's audio worker can continue
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
}

size_t LibretroAudioDriver::MixInto(float* mix, size_t samples) {
  float buffer[2048];
  size_t total = 0;
  while (total < samples) {
    const size_t want = std::min(samples - total, sizeof(buffer) / sizeof(buffer[0]));
    const size_t got = ring_.Pop(buffer, want);
    for (size_t i = 0; i < got; ++i) {
      mix[total + i] += buffer[i];
    }
    total += got;
    if (got < want) break;
  }
  return total;
}

void LibretroAudioDriver::Pause() { paused_ = true; }
void LibretroAudioDriver::Resume() { paused_ = false; }
void LibretroAudioDriver::SetVolume(float volume) { volume_ = volume; }

// ---- LibretroAudioSystem --------------------------------------------------

LibretroAudioSystem::LibretroAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

// Emulator::Shutdown frees the audio system before the media player, whose
// driver would then unregister from a freed system: drivers still registered
// are let go of here.
LibretroAudioSystem::~LibretroAudioSystem() {
  std::lock_guard<std::mutex> lock(drivers_mutex_);
  for (LibretroAudioDriver* driver : drivers_) {
    driver->Detach();
  }
  drivers_.clear();
}

void LibretroAudioSystem::Register(LibretroAudioDriver* driver) {
  std::lock_guard<std::mutex> lock(drivers_mutex_);
  drivers_.push_back(driver);
}

void LibretroAudioSystem::Unregister(LibretroAudioDriver* driver) {
  std::lock_guard<std::mutex> lock(drivers_mutex_);
  drivers_.erase(std::remove(drivers_.begin(), drivers_.end(), driver),
                 drivers_.end());
}

size_t LibretroAudioSystem::Mix(int16_t* out, size_t max_samples) {
  std::lock_guard<std::mutex> lock(drivers_mutex_);
  mix_.assign(max_samples, 0.0f);
  size_t longest = 0;
  for (LibretroAudioDriver* driver : drivers_) {
    longest = std::max(longest, driver->MixInto(mix_.data(), max_samples));
  }
  for (size_t i = 0; i < longest; ++i) {
    const float s = std::max(-1.0f, std::min(1.0f, mix_[i]));
    out[i] = static_cast<int16_t>(s * 32767.0f);
  }
  return longest;
}

X_STATUS LibretroAudioSystem::CreateDriver(size_t index,
                                            xe::threading::Semaphore* semaphore,
                                            AudioDriver** out_driver) {
  // An audio client: 5.1, big-endian, channel after channel, at 48 kHz.
  auto driver = new LibretroAudioDriver(this, semaphore, 48000, 6, true);
  if (!driver->Initialize()) {
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

AudioDriver* LibretroAudioSystem::CreateDriver(
    xe::threading::Semaphore* semaphore, uint32_t frequency,
    uint32_t channels, bool need_format_conversion) {
  // The media player's (XMP background music), in its own format - taking it
  // as a client's 5.1 big-endian frames was the static in game menus.
  return new LibretroAudioDriver(this, semaphore, frequency, channels,
                                 need_format_conversion);
}

void LibretroAudioSystem::DestroyDriver(AudioDriver* driver) {
  if (driver) {
    driver->Shutdown();
    delete driver;
  }
}

}  // namespace libretro
}  // namespace apu
}  // namespace xe
