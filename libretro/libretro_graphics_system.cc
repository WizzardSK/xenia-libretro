/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Graphics System Implementation
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#include "libretro_graphics_system.h"

#include <chrono>

namespace xe {
namespace gpu {
namespace libretro_gpu {

// ---- LibretroCommandProcessor ---------------------------------------------

LibretroCommandProcessor::LibretroCommandProcessor(
    GraphicsSystem* graphics_system, kernel::KernelState* kernel_state,
    FrameSyncState* sync)
    : CommandProcessor(graphics_system, kernel_state), sync_(sync) {}

LibretroCommandProcessor::~LibretroCommandProcessor() = default;

void LibretroCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                         uint32_t length) {}

void LibretroCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {}

bool LibretroCommandProcessor::SetupContext() {
  return CommandProcessor::SetupContext();
}

void LibretroCommandProcessor::ShutdownContext() {
  CommandProcessor::ShutdownContext();
}

void LibretroCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                          uint32_t frontbuffer_width,
                                          uint32_t frontbuffer_height) {
  if (!sync_) return;

  // Store frame dimensions
  sync_->frame_width.store(frontbuffer_width ? frontbuffer_width : 1280,
                           std::memory_order_relaxed);
  sync_->frame_height.store(frontbuffer_height ? frontbuffer_height : 720,
                            std::memory_order_relaxed);
  sync_->frame_count.fetch_add(1, std::memory_order_relaxed);

  // Signal frame ready
  {
    std::lock_guard<std::mutex> lock(sync_->mutex);
    sync_->frame_pending.store(true, std::memory_order_release);
  }
  sync_->frame_ready_cv.notify_one();

  // Throttle: call the base class throttle so the GPU thread doesn't
  // spin faster than the frontend can consume frames.
  ThrottlePresentation();
}

Shader* LibretroCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                              const uint32_t* host_address,
                                              uint32_t dword_count) {
  return nullptr;
}

bool LibretroCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                          uint32_t index_count,
                                          IndexBufferInfo* index_buffer_info,
                                          bool major_mode_explicit) {
  return true;
}

bool LibretroCommandProcessor::IssueCopy() { return true; }

void LibretroCommandProcessor::InitializeTrace() {}

// ---- LibretroGraphicsSystem -----------------------------------------------

LibretroGraphicsSystem::LibretroGraphicsSystem() {}

LibretroGraphicsSystem::~LibretroGraphicsSystem() {
  sync_state_.shutdown.store(true, std::memory_order_release);
  sync_state_.frame_ready_cv.notify_all();
}

X_STATUS LibretroGraphicsSystem::Setup(cpu::Processor* processor,
                                        kernel::KernelState* kernel_state,
                                        ui::WindowedAppContext* app_context,
                                        bool with_presentation) {
  // Skip Vulkan provider - frontend owns GPU context.
  return GraphicsSystem::Setup(processor, kernel_state, app_context, false);
}

void LibretroGraphicsSystem::Shutdown() {
  sync_state_.shutdown.store(true, std::memory_order_release);
  sync_state_.frame_ready_cv.notify_all();
  GraphicsSystem::Shutdown();
}

std::unique_ptr<CommandProcessor>
LibretroGraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(
      new LibretroCommandProcessor(this, kernel_state_, &sync_state_));
}

bool LibretroGraphicsSystem::WaitForFrame(unsigned timeout_ms) {
  if (sync_state_.shutdown.load(std::memory_order_acquire)) return false;

  // Fast path: frame already pending
  if (sync_state_.frame_pending.load(std::memory_order_acquire)) {
    sync_state_.frame_pending.store(false, std::memory_order_release);
    return true;
  }

  // Wait for GPU thread to signal a swap
  std::unique_lock<std::mutex> lock(sync_state_.mutex);
  bool got = sync_state_.frame_ready_cv.wait_for(
      lock, std::chrono::milliseconds(timeout_ms), [this] {
        return sync_state_.frame_pending.load(std::memory_order_acquire) ||
               sync_state_.shutdown.load(std::memory_order_acquire);
      });

  if (got && sync_state_.frame_pending.load(std::memory_order_acquire)) {
    sync_state_.frame_pending.store(false, std::memory_order_release);
    return true;
  }
  return false;
}

}  // namespace libretro_gpu
}  // namespace gpu
}  // namespace xe
