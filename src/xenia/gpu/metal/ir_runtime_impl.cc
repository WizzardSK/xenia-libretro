/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The Metal Shader Converter runtime header only defines its bind point
// constants, descriptor table encoders and draw helpers under
// IR_PRIVATE_IMPLEMENTATION. This is the one translation unit that does so.

#include "third_party/metal-cpp/Metal/Metal.hpp"

#define IR_PRIVATE_IMPLEMENTATION
#include "metal_irconverter_runtime.h"
