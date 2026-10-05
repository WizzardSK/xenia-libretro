/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_LZX_H_
#define XENIA_CPU_LZX_H_

#include <string>
#include <vector>

#include "xenia/cpu/module.h"

namespace xe {
struct xex2_delta_patch;
}  // namespace xe

struct lzxd_stream;
struct mspack_memory_file_t;
struct mspack_system;

int lzx_decompress(const void* lzx_data, size_t lzx_len, void* dest,
                   size_t dest_len, uint32_t window_size, void* window_data,
                   size_t window_data_len);

// Decodes an LZX stream one frame per call, the way the kernel's LDI exports
// take it. The compressed bits of each frame start on their own.
class LzxFrameDecoder {
 public:
  explicit LzxFrameDecoder(uint32_t window_size);
  ~LzxFrameDecoder();
  LzxFrameDecoder(const LzxFrameDecoder&) = delete;
  LzxFrameDecoder& operator=(const LzxFrameDecoder&) = delete;

  // Starts a new stream. False if the window can't be allocated.
  bool Reset();
  // Decodes the next frame, which inflates to dest_size bytes, at most 32 KB.
  bool Decompress(const void* source, size_t source_size, void* dest,
                  size_t dest_size);

 private:
  uint32_t window_bits_ = 0;
  mspack_system* system_;
  mspack_memory_file_t* input_;
  mspack_memory_file_t* output_;
  lzxd_stream* stream_ = nullptr;
};

int lzxdelta_apply_patch(xe::xex2_delta_patch* patch, size_t patch_len,
                         uint32_t window_size, void* dest);

#endif  // XENIA_CPU_LZX_H_
