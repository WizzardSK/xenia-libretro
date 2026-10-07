/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_zpd_visibility_pool.h"

#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

bool MetalZPDVisibilityPool::EnsureInitialized(MTL::Device* device,
                                               uint32_t requested_capacity) {
  if (is_initialized() && capacity_ == requested_capacity) {
    return true;
  }

  if (is_initialized()) {
    // Can't resize while resolves may be in flight. Caller must ensure
    // the pool is idle before requesting a different capacity.
    assert_true(free_indices_.size() == capacity_);
    Shutdown();
  }

  if (requested_capacity == 0) {
    return false;
  }

  size_t buffer_size =
      static_cast<size_t>(requested_capacity) * sizeof(uint64_t);
  visibility_buffer_ =
      device->newBuffer(buffer_size, MTL::ResourceStorageModeShared);
  if (!visibility_buffer_) {
    XELOGW(
        "MetalZPDVisibilityPool: Failed to allocate the ZPD visibility buffer "
        "(size={}), falling back to fake sample counts.",
        buffer_size);
    return false;
  }
  visibility_buffer_->setLabel(
      NS::String::string("XeniaZPDVisibility", NS::UTF8StringEncoding));

  visibility_mapping_ =
      reinterpret_cast<uint64_t*>(visibility_buffer_->contents());
  std::memset(visibility_mapping_, 0, buffer_size);

  capacity_ = requested_capacity;

  free_indices_.clear();
  free_indices_.reserve(requested_capacity);
  for (uint32_t i = requested_capacity; i > 0; --i) {
    free_indices_.push_back(i - 1);
  }
  generations_.assign(requested_capacity, 0);

  return true;
}

void MetalZPDVisibilityPool::Shutdown() {
  free_indices_.clear();
  generations_.clear();

  capacity_ = 0;
  visibility_mapping_ = nullptr;

  if (visibility_buffer_) {
    visibility_buffer_->release();
    visibility_buffer_ = nullptr;
  }
}

bool MetalZPDVisibilityPool::Acquire(uint32_t& index, uint32_t& generation,
                                     size_t& offset) {
  if (free_indices_.empty()) {
    index = UINT32_MAX;
    generation = 0;
    offset = 0;
    return false;
  }

  index = free_indices_.back();
  free_indices_.pop_back();

  assert_true(index < generations_.size());
  generation = generations_[index];
  offset = static_cast<size_t>(index) * sizeof(uint64_t);
  visibility_mapping_[index] = 0;
  return true;
}

void MetalZPDVisibilityPool::Release(uint32_t index, uint32_t generation) {
  if (index >= capacity_) {
    return;
  }

  if (!IsGenerationCurrent(index, generation)) {
    XELOGW("MetalZPDVisibilityPool: stale release index={} gen={}", index,
           generation);
    return;
  }

  ++generations_[index];
  free_indices_.push_back(index);
}

bool MetalZPDVisibilityPool::IsGenerationCurrent(uint32_t index,
                                                 uint32_t generation) const {
  return index < generations_.size() && generations_[index] == generation;
}

uint64_t MetalZPDVisibilityPool::Read(uint32_t index) const {
  if (index >= capacity_ || !visibility_mapping_) {
    return 0;
  }
  return visibility_mapping_[index];
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
