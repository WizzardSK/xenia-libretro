/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Staging buffers for reading GPU output back into guest RAM, shared by the
// D3D12 and Vulkan command processors. Like command_processor_memexport.inc,
// this is #included into each backend's class body so each gets its own
// inlinable copy with no inheritance or virtual dispatch.
//
// The memexport fallback for hosts without the guest RAM host buffer (see the
// enable_host_buffer cvar): output stays in the device-local buffer and reaches
// guest RAM by a GPU copy into a host-visible buffer and a CPU copy out.
//
// The include site must already provide memory_ (Memory*), frame_current_,
// AlignReadbackBufferSize, xenia/base/assert.h, xenia/base/logging.h,
// xenia/base/memory.h and <cstdint> <cstring> <unordered_map>. It must also
// define, before including this:
//   struct ReadbackStagingBuffer
//     One host-visible buffer, persistently mapped as `void* mapped`.
//   bool CreateReadbackStagingBuffer(ReadbackStagingBuffer&, uint32_t size)
//   void DestroyReadbackStagingBuffer(ReadbackStagingBuffer&)
//   void PrepareReadbackStagingEviction()
//     Called once before an eviction's first destroy, for backends that drain
//     rather than defer.
// And, anywhere in the class body:
//   std::vector<MemexportStagedRange> memexport_staged_
//     Staged export ranges by slot key, guest address, length and slot offset.
// No include guard: injected into a class body.

// Age eviction alone only runs at frame close, so a title that never swaps
// would grow without bound.
static constexpr uint64_t kMaxReadbackStagingBytes = 64 * 1024 * 1024;

struct ReadbackStagingSlot {
  ReadbackStagingBuffer buffer;
  // 0 until the buffer is created. A key carries its own length, so a buffer
  // is sized once and never resized - a copy recorded into one is never left
  // pointing at a freed buffer.
  uint32_t size = 0;
  uint64_t last_used_frame = 0;
};

// Returns the slot for this key with its buffer ready, or null if the buffer
// could not be created.
ReadbackStagingSlot* AcquireReadbackStagingSlot(uint64_t key, uint32_t length) {
  if (!length) {
    return nullptr;
  }
  ReadbackStagingSlot& slot = readback_staging_slots_[key];
  if (slot.size) {
    assert_true(slot.size >= length);
    slot.last_used_frame = frame_current_;
    return &slot;
  }
  uint32_t size = AlignReadbackBufferSize(length);
  EvictReadbackStagingToFit(size, key);
  if (!CreateReadbackStagingBuffer(slot.buffer, size)) {
    readback_staging_slots_.erase(key);
    return nullptr;
  }
  slot.size = size;
  readback_staging_bytes_ += size;
  slot.last_used_frame = frame_current_;
  return &slot;
}

// Frees least recently used slots until size more bytes fit. Skips keep_key
// and anything used this frame, which may hold a copy waiting to be read out.
void EvictReadbackStagingToFit(uint32_t size, uint64_t keep_key) {
  bool eviction_prepared = false;
  while (readback_staging_bytes_ + size > kMaxReadbackStagingBytes) {
    auto oldest = readback_staging_slots_.end();
    for (auto it = readback_staging_slots_.begin();
         it != readback_staging_slots_.end(); ++it) {
      if (it->first == keep_key ||
          it->second.last_used_frame == frame_current_ || !it->second.size) {
        continue;
      }
      if (oldest == readback_staging_slots_.end() ||
          it->second.last_used_frame < oldest->second.last_used_frame) {
        oldest = it;
      }
    }
    if (oldest == readback_staging_slots_.end()) {
      // All in use this frame - go over budget rather than drop a live copy.
      return;
    }
    if (!eviction_prepared) {
      PrepareReadbackStagingEviction();
      eviction_prepared = true;
    }
    DestroyReadbackStagingSlot(oldest->second);
    readback_staging_slots_.erase(oldest);
  }
}

// The slot held for this key, or null if there is none.
ReadbackStagingSlot* FindReadbackStagingSlot(uint64_t key) {
  auto slot = readback_staging_slots_.find(key);
  return slot != readback_staging_slots_.end() ? &slot->second : nullptr;
}

// Copies length bytes out of a staging buffer into guest RAM at address. The
// GPU copy into the staging buffer must already have completed.
void ReadbackStagingToGuestRam(const ReadbackStagingBuffer& buffer,
                               uint32_t source_offset, uint32_t address,
                               uint32_t length) {
  if (buffer.mapped == nullptr || !length) {
    return;
  }
  uint8_t* dest = memory_->TranslatePhysical(address);
  uint8_t* source = static_cast<uint8_t*>(buffer.mapped) + source_offset;
  // vastcpy's non-temporal stores need both pointers and the length to be
  // cache-line aligned.
  uintptr_t unaligned = reinterpret_cast<uintptr_t>(dest) |
                        reinterpret_cast<uintptr_t>(source) | uintptr_t(length);
  if (unaligned & 63) {
    std::memcpy(dest, source, length);
  } else {
    memory::vastcpy(dest, source, length);
  }
}

// Trims staged export output a later resolve overwrote, keeping the rest.
void DropStagedMemexportOutput(uint32_t address, uint32_t length) {
  if (!length) {
    return;
  }
  uint64_t end = uint64_t(address) + length;
  for (size_t i = 0; i < memexport_staged_.size();) {
    MemexportStagedRange staged = memexport_staged_[i];
    uint64_t staged_end = uint64_t(staged.address) + staged.length;
    if (staged_end <= address || staged.address >= end) {
      ++i;
      continue;
    }
    // Split in place, copy out order decides overlapping ranges.
    bool head = staged.address < address;
    if (head) {
      memexport_staged_[i].length = address - staged.address;
      ++i;
    }
    if (staged_end > end) {
      MemexportStagedRange tail = staged;
      tail.offset += uint32_t(end - staged.address);
      tail.address = uint32_t(end);
      tail.length = uint32_t(staged_end - end);
      if (head) {
        memexport_staged_.insert(memexport_staged_.begin() + i, tail);
      } else {
        memexport_staged_[i] = tail;
      }
      ++i;
    } else if (!head) {
      memexport_staged_.erase(memexport_staged_.begin() + i);
    }
  }
}

// Releases staging buffers untouched for kReadbackBufferEvictionAgeFrames.
// Called once per frame.
void EvictOldReadbackStaging() {
  if (frame_current_ <= kReadbackBufferEvictionAgeFrames) {
    return;
  }
  uint64_t oldest_kept = frame_current_ - kReadbackBufferEvictionAgeFrames;
  bool eviction_prepared = false;
  for (auto it = readback_staging_slots_.begin();
       it != readback_staging_slots_.end();) {
    if (it->second.last_used_frame >= oldest_kept) {
      ++it;
      continue;
    }
    if (!eviction_prepared) {
      PrepareReadbackStagingEviction();
      eviction_prepared = true;
    }
    DestroyReadbackStagingSlot(it->second);
    it = readback_staging_slots_.erase(it);
  }
}

// Only safe where teardown is, the backend hook does not defer past shutdown.
void ClearReadbackStagingBuffers() {
  for (auto& slot : readback_staging_slots_) {
    DestroyReadbackStagingSlot(slot.second);
  }
  readback_staging_slots_.clear();
  readback_staging_bytes_ = 0;
}

void DestroyReadbackStagingSlot(ReadbackStagingSlot& slot) {
  if (slot.size) {
    DestroyReadbackStagingBuffer(slot.buffer);
    readback_staging_bytes_ -= slot.size;
    slot.size = 0;
  }
}

// One buffer per export range, keyed by MakeReadbackResolveKey.
std::unordered_map<uint64_t, ReadbackStagingSlot> readback_staging_slots_;
uint64_t readback_staging_bytes_ = 0;
