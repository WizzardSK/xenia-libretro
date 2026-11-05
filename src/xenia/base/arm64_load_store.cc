/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/arm64_load_store.h"

#if XE_ARCH_ARM64

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>

#include "xenia/base/exception_handler.h"
#include "xenia/base/host_thread_context.h"

#if XE_COMPILER_MSVC
#include <intrin.h>
#define xe_cpu_mrs(reg) _ReadStatusReg(reg)
#elif XE_COMPILER_CLANG || XE_COMPILER_GNUC
#include <arm_acle.h>
#define xe_cpu_mrs(reg) __arm_rsr64(#reg)
#else
#error "No MRS wrapper available for current compiler implemented."
#endif

namespace xe {

namespace {

#if XE_COMPILER_MSVC
constexpr uint32_t DCZID_EL0 = ARM64_SYSREG(0b11, 0b011, 0b0000, 0b0000, 0b111);
#endif

// The host pieces one access goes through, at most one per byte of an LDP of
// two Q registers.
struct MappedAccess {
  uint8_t* host[32];
  size_t length[32];
  uint32_t count = 0;
  // Whether a native read of any of the pieces succeeds.
  bool natively_readable = false;
};

bool MapAccess(Arm64TranslateFunction translate, void* translate_context,
               uint64_t address, uint32_t size, bool is_write,
               MappedAccess* mapped, uint64_t* fault_address) {
  uint32_t done = 0;
  while (done < size) {
    size_t span = 0;
    bool natively_readable = false;
    uint8_t* host = translate(translate_context, address + done, is_write,
                              &span, &natively_readable);
    if (!host || !span || mapped->count == std::size(mapped->host)) {
      *fault_address = address + done;
      return false;
    }
    const size_t length = std::min<size_t>(span, size - done);
    mapped->host[mapped->count] = host;
    mapped->length[mapped->count] = length;
    mapped->natively_readable |= natively_readable;
    ++mapped->count;
    done += uint32_t(length);
  }
  return true;
}

// A naturally aligned access in one piece is single-copy atomic, as the
// instruction would be.
template <typename T>
bool CopySized(uint8_t* host, uint8_t* data, bool is_write) {
  if (reinterpret_cast<uintptr_t>(host) % sizeof(T)) {
    return false;
  }
  std::atomic_ref<T> pointer(*reinterpret_cast<T*>(host));
  if (is_write) {
    T value;
    std::memcpy(&value, data, sizeof(T));
    pointer.store(value, std::memory_order_relaxed);
  } else {
    const T value = pointer.load(std::memory_order_relaxed);
    std::memcpy(data, &value, sizeof(T));
  }
  return true;
}

void CopyAccess(const MappedAccess& mapped, uint8_t* data, bool is_write) {
  if (mapped.count == 1) {
    uint8_t* host = mapped.host[0];
    switch (mapped.length[0]) {
      case 1:
        if (CopySized<uint8_t>(host, data, is_write)) {
          return;
        }
        break;
      case 2:
        if (CopySized<uint16_t>(host, data, is_write)) {
          return;
        }
        break;
      case 4:
        if (CopySized<uint32_t>(host, data, is_write)) {
          return;
        }
        break;
      case 8:
        if (CopySized<uint64_t>(host, data, is_write)) {
          return;
        }
        break;
      case 16:
        // Two halves, single-copy atomic as ARMv8.0 makes them.
        if (CopySized<uint64_t>(host, data, is_write)) {
          CopySized<uint64_t>(host + 8, data + 8, is_write);
          return;
        }
        break;
    }
  }
  for (uint32_t i = 0; i < mapped.count; ++i) {
    if (is_write) {
      std::memcpy(mapped.host[i], data, mapped.length[i]);
    } else {
      std::memcpy(data, mapped.host[i], mapped.length[i]);
    }
    data += mapped.length[i];
  }
}

// Whether an atomic can run on the access, which hardware requires to be
// naturally aligned.
bool IsSingleAligned(const MappedAccess& mapped, uint32_t size) {
  return mapped.count == 1 &&
         !(reinterpret_cast<uintptr_t>(mapped.host[0]) % size);
}

template <typename T>
uint64_t CompareExchangeSized(uint8_t* host, uint64_t expected,
                              uint64_t desired) {
  T value = T(expected);
  std::atomic_ref<T>(*reinterpret_cast<T*>(host))
      .compare_exchange_strong(value, T(desired), std::memory_order_seq_cst);
  return value;
}

// The old value, after storing |desired| if it was |expected|.
uint64_t CompareExchange(uint8_t* host, uint32_t size, uint64_t expected,
                         uint64_t desired) {
  switch (size) {
    case 1:
      return CompareExchangeSized<uint8_t>(host, expected, desired);
    case 2:
      return CompareExchangeSized<uint16_t>(host, expected, desired);
    case 4:
      return CompareExchangeSized<uint32_t>(host, expected, desired);
    default:
      return CompareExchangeSized<uint64_t>(host, expected, desired);
  }
}

uint64_t SizeMask(uint32_t size) {
  return size >= 8 ? ~uint64_t(0) : (uint64_t(1) << (size * 8)) - 1;
}

// The exclusive monitor an emulated LDXR sets and an emulated STXR checks. A
// store succeeds only if memory still holds the value the load saw.
struct ExclusiveMonitor {
  uint64_t address;
  uint64_t value;
  uint32_t size;
  bool valid;
};
thread_local ExclusiveMonitor exclusive_monitor;

class Emulation {
 public:
  Emulation(Exception* ex, Arm64TranslateFunction translate,
            void* translate_context)
      : ex_(ex),
        context_(ex->thread_context()),
        translate_(translate),
        translate_context_(translate_context) {}

  Arm64EmulateResult Run(uint32_t instruction);

  uint64_t fault_address() const { return fault_address_; }
  bool fault_is_write() const { return fault_is_write_; }

 private:
  // Xn with 31 as the zero register.
  uint64_t ReadX(uint32_t n) const { return n == 31 ? 0 : context_->x[n]; }
  void WriteX(uint32_t n, uint64_t value) {
    if (n != 31) {
      ex_->ModifyXRegister(n) = value;
    }
  }
  // Xn with 31 as the stack pointer.
  uint64_t ReadBase(uint32_t n) const {
    return n == 31 ? context_->sp : context_->x[n];
  }
  void WriteBase(uint32_t n, uint64_t value) {
    if (n == 31) {
      context_->sp = value;
    } else {
      ex_->ModifyXRegister(n) = value;
    }
  }
  void WriteV(uint32_t n, const uint8_t* data, uint32_t size) {
    vec128_t value;
    std::memset(&value, 0, sizeof(value));
    std::memcpy(&value, data, size);
    ex_->ModifyVRegister(n) = value;
  }

  bool Map(uint64_t address, uint32_t size, bool is_write,
           MappedAccess* mapped) {
    if (MapAccess(translate_, translate_context_, address, size, is_write,
                  mapped, &fault_address_)) {
      return true;
    }
    fault_is_write_ = is_write;
    return false;
  }

  Arm64EmulateResult Fault() { return Arm64EmulateResult::kFault; }
  Arm64EmulateResult Done() {
    ex_->set_resume_pc(context_->pc + 4);
    return Arm64EmulateResult::kDone;
  }

  Arm64EmulateResult Single(uint32_t instruction, uint64_t address,
                            bool writeback, uint64_t writeback_value);
  Arm64EmulateResult Pair(uint32_t instruction);
  Arm64EmulateResult Exclusive(uint32_t instruction);
  Arm64EmulateResult Atomic(uint32_t instruction);
  Arm64EmulateResult ZeroBlock(uint32_t instruction);
  Arm64EmulateResult CacheMaintenance(uint32_t instruction);

  Exception* ex_;
  HostThreadContext* context_;
  Arm64TranslateFunction translate_;
  void* translate_context_;
  uint64_t fault_address_ = 0;
  bool fault_is_write_ = false;
};

uint64_t ExtendRegister(uint64_t value, uint32_t option) {
  switch (option) {
    case 0b010:  // UXTW
      return uint32_t(value);
    case 0b110:  // SXTW
      return uint64_t(int64_t(int32_t(value)));
    default:  // LSL / UXTX / SXTX
      return value;
  }
}

int64_t SignExtend(uint64_t value, uint32_t bits) {
  const uint64_t sign = uint64_t(1) << (bits - 1);
  return int64_t((value ^ sign) - sign);
}

Arm64EmulateResult Emulation::Run(uint32_t instruction) {
  const uint32_t rn = (instruction >> 5) & 31;
  if ((instruction & 0xFFFFF0E0) == 0xD50B7020) {
    // SYS #3, C7, Cm, #1, the data and instruction cache operations by VA.
    switch ((instruction >> 8) & 0xF) {
      case 4:  // DC ZVA
        return ZeroBlock(instruction);
      case 5:   // IC IVAU
      case 10:  // DC CVAC
      case 11:  // DC CVAU
      case 12:  // DC CVAP
      case 13:  // DC CVADP
      case 14:  // DC CIVAC
        return CacheMaintenance(instruction);
      default:
        return Arm64EmulateResult::kUnsupported;
    }
  }
  if ((instruction & 0x3F000000) == 0x08000000) {
    return Exclusive(instruction);
  }
  if ((instruction & 0x3A000000) == 0x28000000) {
    return Pair(instruction);
  }
  if ((instruction & 0x3B000000) == 0x39000000) {
    // Unsigned immediate, scaled by the access size.
    const uint32_t size = instruction >> 30;
    const bool vector = (instruction >> 26) & 1;
    const uint32_t shift = vector && ((instruction >> 23) & 1) ? 4 : size;
    const uint64_t offset = uint64_t((instruction >> 10) & 0xFFF) << shift;
    return Single(instruction, ReadBase(rn) + offset, false, 0);
  }
  if ((instruction & 0x3B000000) != 0x38000000) {
    return Arm64EmulateResult::kUnsupported;
  }
  if ((instruction >> 21) & 1) {
    if (((instruction >> 10) & 3) == 0b00) {
      return Atomic(instruction);
    }
    if (((instruction >> 10) & 3) != 0b10) {
      // Pointer authentication.
      return Arm64EmulateResult::kUnsupported;
    }
    const uint32_t option = (instruction >> 13) & 7;
    if (!(option & 0b010)) {
      return Arm64EmulateResult::kUnsupported;
    }
    const uint32_t size = instruction >> 30;
    const bool vector = (instruction >> 26) & 1;
    const uint32_t shift =
        (instruction >> 12) & 1
            ? (vector && ((instruction >> 23) & 1) ? 4 : size)
            : 0;
    const uint64_t index =
        ExtendRegister(ReadX((instruction >> 16) & 31), option) << shift;
    return Single(instruction, ReadBase(rn) + index, false, 0);
  }
  const uint64_t base = ReadBase(rn);
  const uint64_t offset = uint64_t(SignExtend((instruction >> 12) & 0x1FF, 9));
  switch ((instruction >> 10) & 3) {
    case 0b01:  // Post-index.
      return Single(instruction, base, true, base + offset);
    case 0b11:  // Pre-index.
      return Single(instruction, base + offset, true, base + offset);
    default:  // Unscaled, and unprivileged, which is the same in user mode.
      return Single(instruction, base + offset, false, 0);
  }
}

Arm64EmulateResult Emulation::Single(uint32_t instruction, uint64_t address,
                                     bool writeback, uint64_t writeback_value) {
  const uint32_t size = instruction >> 30;
  const bool vector = (instruction >> 26) & 1;
  const uint32_t opc = (instruction >> 22) & 3;
  const uint32_t rt = instruction & 31;
  uint32_t bytes = 1u << size;
  bool is_load = opc != 0;
  // Bits a load sign-extends from, and to 32 or 64 bits.
  uint32_t sign_bits = 0;
  bool sign_to_32 = false;
  if (vector) {
    if (opc & 2) {
      if (size) {
        return Arm64EmulateResult::kUnsupported;
      }
      bytes = 16;
    }
    is_load = opc & 1;
  } else if (opc == 0b10) {
    if (size == 3) {
      // A prefetch never faults.
      return Done();
    }
    sign_bits = bytes * 8;
  } else if (opc == 0b11) {
    if (size >= 2) {
      return Arm64EmulateResult::kUnsupported;
    }
    sign_bits = bytes * 8;
    sign_to_32 = true;
  }

  MappedAccess mapped;
  if (!Map(address, bytes, !is_load, &mapped)) {
    return Fault();
  }
  uint8_t data[16] = {};
  if (is_load) {
    CopyAccess(mapped, data, false);
    if (vector) {
      WriteV(rt, data, bytes);
    } else {
      uint64_t value = 0;
      std::memcpy(&value, data, bytes);
      if (sign_bits) {
        value = uint64_t(SignExtend(value, sign_bits));
        if (sign_to_32) {
          value = uint32_t(value);
        }
      }
      WriteX(rt, value);
    }
  } else {
    if (vector) {
      std::memcpy(data, &context_->v[rt], bytes);
    } else {
      const uint64_t value = ReadX(rt);
      std::memcpy(data, &value, bytes);
    }
    CopyAccess(mapped, data, true);
  }
  if (writeback) {
    WriteBase((instruction >> 5) & 31, writeback_value);
  }
  return Done();
}

Arm64EmulateResult Emulation::Pair(uint32_t instruction) {
  const uint32_t opc = instruction >> 30;
  const bool vector = (instruction >> 26) & 1;
  const bool is_load = (instruction >> 22) & 1;
  const uint32_t rt = instruction & 31;
  const uint32_t rn = (instruction >> 5) & 31;
  const uint32_t rt2 = (instruction >> 10) & 31;
  uint32_t bytes;
  bool sign_extend = false;
  if (vector) {
    if (opc == 3) {
      return Arm64EmulateResult::kUnsupported;
    }
    bytes = 4u << opc;
  } else if (opc == 0) {
    bytes = 4;
  } else if (opc == 2) {
    bytes = 8;
  } else if (opc == 1 && is_load) {
    // LDPSW.
    bytes = 4;
    sign_extend = true;
  } else {
    return Arm64EmulateResult::kUnsupported;
  }
  const uint64_t offset =
      uint64_t(SignExtend((instruction >> 15) & 0x7F, 7)) * bytes;
  const uint64_t base = ReadBase(rn);
  const uint32_t mode = (instruction >> 23) & 3;
  const uint64_t address = mode == 0b01 ? base : base + offset;

  MappedAccess mapped;
  if (!Map(address, bytes * 2, !is_load, &mapped)) {
    return Fault();
  }
  uint8_t data[32] = {};
  if (is_load) {
    CopyAccess(mapped, data, false);
    if (vector) {
      WriteV(rt, data, bytes);
      WriteV(rt2, data + bytes, bytes);
    } else {
      for (uint32_t i = 0; i < 2; ++i) {
        uint64_t value = 0;
        std::memcpy(&value, data + i * bytes, bytes);
        if (sign_extend) {
          value = uint64_t(SignExtend(value, 32));
        }
        WriteX(i ? rt2 : rt, value);
      }
    }
  } else {
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t r = i ? rt2 : rt;
      if (vector) {
        std::memcpy(data + i * bytes, &context_->v[r], bytes);
      } else {
        const uint64_t value = ReadX(r);
        std::memcpy(data + i * bytes, &value, bytes);
      }
    }
    CopyAccess(mapped, data, true);
  }
  if (mode == 0b01 || mode == 0b11) {
    WriteBase(rn, base + offset);
  }
  return Done();
}

Arm64EmulateResult Emulation::Exclusive(uint32_t instruction) {
  const uint32_t bytes = 1u << (instruction >> 30);
  const bool o2 = (instruction >> 23) & 1;
  const bool is_load = (instruction >> 22) & 1;
  const bool o1 = (instruction >> 21) & 1;
  const uint32_t rs = (instruction >> 16) & 31;
  const uint32_t rt = instruction & 31;
  const uint64_t address = ReadBase((instruction >> 5) & 31);
  const uint64_t mask = SizeMask(bytes);
  if (o1 && !o2) {
    // LDXP, STXP and CASP.
    return Arm64EmulateResult::kUnsupported;
  }
  MappedAccess mapped;
  if (!o2 && !is_load) {
    // STXR, which only stores if the monitor is still set for this address.
    const ExclusiveMonitor monitor = exclusive_monitor;
    exclusive_monitor.valid = false;
    uint32_t status = 1;
    if (monitor.valid && monitor.address == address && monitor.size == bytes) {
      if (!Map(address, bytes, true, &mapped)) {
        return Fault();
      }
      if (!IsSingleAligned(mapped, bytes)) {
        return Arm64EmulateResult::kUnsupported;
      }
      // Where a native read succeeds, the LDXR may have run natively and left
      // this monitor stale, so the store fails.
      if (!mapped.natively_readable &&
          CompareExchange(mapped.host[0], bytes, monitor.value,
                          ReadX(rt) & mask) == monitor.value) {
        status = 0;
      }
    }
    WriteX(rs, status);
    return Done();
  }
  if (o1) {
    // CAS. Rs holds the expected value and gets the old one.
    if (!Map(address, bytes, true, &mapped)) {
      return Fault();
    }
    if (!IsSingleAligned(mapped, bytes)) {
      return Arm64EmulateResult::kUnsupported;
    }
    const uint64_t old = CompareExchange(mapped.host[0], bytes,
                                         ReadX(rs) & mask, ReadX(rt) & mask);
    WriteX(rs, old);
    return Done();
  }
  // LDXR and LDAR, or STLR.
  if (!Map(address, bytes, !is_load, &mapped)) {
    return Fault();
  }
  if (!o2 && !IsSingleAligned(mapped, bytes)) {
    // An exclusive load the matching store can't complete.
    return Arm64EmulateResult::kUnsupported;
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint8_t data[8] = {};
  if (is_load) {
    CopyAccess(mapped, data, false);
    uint64_t value = 0;
    std::memcpy(&value, data, bytes);
    WriteX(rt, value);
    if (!o2) {
      exclusive_monitor = {address, value, bytes, true};
    }
  } else {
    const uint64_t value = ReadX(rt);
    std::memcpy(data, &value, bytes);
    CopyAccess(mapped, data, true);
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
  return Done();
}

// DC ZVA, which zeroes the aligned block DCZID_EL0 sizes around Xt.
Arm64EmulateResult Emulation::ZeroBlock(uint32_t instruction) {
  const uint64_t dczid = xe_cpu_mrs(DCZID_EL0);
  if (dczid & 0x10) {
    // Prohibited, so the instruction would have been undefined.
    return Arm64EmulateResult::kUnsupported;
  }
  const uint32_t bytes = 4u << (dczid & 0xF);
  const uint64_t address = ReadX(instruction & 31) & ~uint64_t(bytes - 1);
  MappedAccess mapped;
  if (!Map(address, bytes, true, &mapped)) {
    return Fault();
  }
  for (uint32_t i = 0; i < mapped.count; ++i) {
    std::memset(mapped.host[i], 0, mapped.length[i]);
  }
  return Done();
}

// DC CVAC, CVAU, CVAP, CVADP, CIVAC and IC IVAU. They fault only on a line
// that isn't readable, and aren't repeated on the alias.
Arm64EmulateResult Emulation::CacheMaintenance(uint32_t instruction) {
  MappedAccess mapped;
  if (!Map(ReadX(instruction & 31), 1, false, &mapped)) {
    return Fault();
  }
  return Done();
}

// LD<op>, ST<op>, SWP and LDAPR, as a compare and swap loop.
Arm64EmulateResult Emulation::Atomic(uint32_t instruction) {
  if ((instruction >> 26) & 1) {
    return Arm64EmulateResult::kUnsupported;
  }
  const uint32_t bytes = 1u << (instruction >> 30);
  const uint32_t op = (instruction >> 12) & 0xF;
  const uint32_t rs = (instruction >> 16) & 31;
  const uint32_t rt = instruction & 31;
  const uint64_t address = ReadBase((instruction >> 5) & 31);
  const uint64_t mask = SizeMask(bytes);
  MappedAccess mapped;
  if (op == 0b1100) {
    // LDAPR.
    if (!Map(address, bytes, false, &mapped)) {
      return Fault();
    }
    uint8_t data[8] = {};
    CopyAccess(mapped, data, false);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint64_t value = 0;
    std::memcpy(&value, data, bytes);
    WriteX(rt, value);
    return Done();
  }
  if ((op & 0b1000) && op != 0b1000) {
    return Arm64EmulateResult::kUnsupported;
  }
  if (!Map(address, bytes, true, &mapped)) {
    return Fault();
  }
  if (!IsSingleAligned(mapped, bytes)) {
    return Arm64EmulateResult::kUnsupported;
  }
  const uint64_t operand = ReadX(rs) & mask;
  const uint32_t bits = bytes * 8;
  // A compare against 0 reads the current value, and stores only what was
  // already there.
  uint64_t old = CompareExchange(mapped.host[0], bytes, 0, 0);
  while (true) {
    uint64_t result;
    switch (op) {
      case 0b0000:
        result = old + operand;
        break;
      case 0b0001:
        result = old & ~operand;
        break;
      case 0b0010:
        result = old ^ operand;
        break;
      case 0b0011:
        result = old | operand;
        break;
      case 0b0100:
        result =
            SignExtend(old, bits) >= SignExtend(operand, bits) ? old : operand;
        break;
      case 0b0101:
        result =
            SignExtend(old, bits) <= SignExtend(operand, bits) ? old : operand;
        break;
      case 0b0110:
        result = std::max(old, operand);
        break;
      case 0b0111:
        result = std::min(old, operand);
        break;
      default:  // SWP.
        result = operand;
        break;
    }
    const uint64_t seen =
        CompareExchange(mapped.host[0], bytes, old, result & mask);
    if (seen == old) {
      break;
    }
    old = seen;
  }
  WriteX(rt, old);
  return Done();
}

}  // namespace

Arm64EmulateResult EmulateArm64LoadStore(Exception* ex,
                                         Arm64TranslateFunction translate,
                                         void* translate_context,
                                         uint64_t* fault_address,
                                         bool* fault_is_write) {
  if (ex->pc() == ex->fault_address()) {
    // The fetch faulted, so there is no instruction to read.
    return Arm64EmulateResult::kUnsupported;
  }
  Emulation emulation(ex, translate, translate_context);
  const uint32_t instruction = *reinterpret_cast<const uint32_t*>(ex->pc());
  const Arm64EmulateResult result = emulation.Run(instruction);
  if (result == Arm64EmulateResult::kFault) {
    *fault_address = emulation.fault_address();
    *fault_is_write = emulation.fault_is_write();
  }
  return result;
}

}  // namespace xe

#endif  // XE_ARCH_ARM64
