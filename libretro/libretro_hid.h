/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro HID (Input Driver)
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * Implements xe::hid::InputDriver to feed libretro controller state
 * into Xenia's input system. The libretro frontend polls input in
 * retro_run(); we cache that state and serve it to Xenia when
 * GetState() is called from guest threads.
 */

#ifndef LIBRETRO_HID_H
#define LIBRETRO_HID_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "libretro.h"
#include "xenia/hid/input_driver.h"

namespace xe {
namespace hid {
namespace libretro_hid {

// Cached per-port state written by libretro frontend thread
// and read by Xenia guest threads.
struct LibretroControllerState {
  // Digital buttons (native libretro RETRO_DEVICE_JOYPAD bits)
  int16_t buttons[16] = {};
  // Analog sticks (RETRO_DEVICE_ANALOG, range -0x7FFF..+0x7FFF)
  int16_t left_stick_x  = 0;
  int16_t left_stick_y  = 0;
  int16_t right_stick_x = 0;
  int16_t right_stick_y = 0;
  // Analog triggers (RETRO_DEVICE_ANALOG INDEX_TRIGGER, 0..+0x7FFF)
  int16_t left_trigger   = 0;
  int16_t right_trigger  = 0;
  // Whether this port is connected
  bool connected = false;
  // Monotonically increasing packet counter
  uint32_t packet_number = 0;
};

// What GetKeystroke hands out for a port: the VK_PAD_* keys that went down,
// up or repeat since the title last asked, made from the polled state the
// way XInput makes them from the controller's
struct LibretroKeystrokeState {
  std::deque<X_INPUT_KEYSTROKE> queue;
  // The keys held at the last poll, and when each one was pressed or last
  // repeated
  std::vector<uint16_t> held;
  std::vector<std::chrono::steady_clock::time_point> next_repeat;
};

class LibretroInputDriver final : public InputDriver {
 public:
  static constexpr size_t kMaxPorts = 4;

  explicit LibretroInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~LibretroInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override;
  // One device per RetroPad port, port N bound to guest slot N. Always all
  // of them: a port the frontend unplugs reads as not connected in GetState,
  // so the binding never has to change
  std::vector<InputDeviceInfo> EnumerateDevices() override;

  // Called from libretro frontend thread (retro_run) to update
  // the cached controller state for all ports.
  void UpdateFromLibretro(retro_input_state_t input_state_cb);

  // Set the rumble callback obtained from the frontend.
  void SetRumbleCallback(retro_set_rumble_state_t cb);

  // Called by frontend retro_set_controller_port_device.
  void SetPortDevice(unsigned port, unsigned device);

 private:
  std::mutex state_mutex_;
  LibretroControllerState states_[kMaxPorts];
  LibretroKeystrokeState keystrokes_[kMaxPorts];
  void UpdateKeystrokes(size_t port);
  retro_set_rumble_state_t rumble_cb_ = nullptr;
  // Tracks which ports the frontend has assigned a device to.
  bool port_connected_[kMaxPorts] = {true, false, false, false};
};

// Factory function (matches xe::hid::nop::Create pattern).
std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order);

}  // namespace libretro_hid
}  // namespace hid
}  // namespace xe

#endif  // LIBRETRO_HID_H
