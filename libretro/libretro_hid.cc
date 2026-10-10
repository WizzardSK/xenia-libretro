/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro HID (Input Driver) Implementation
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#include "libretro_hid.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace hid {
namespace libretro_hid {

// Libretro RetroPad uses SNES positional layout:
//   B=south  A=east  Y=west  X=north
// Xbox 360:
//   A=south  B=east  X=west  Y=north
static uint16_t XboxButtons(const LibretroControllerState& s) {
  uint16_t btns = 0;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_UP])    btns |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_DOWN])  btns |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_LEFT])  btns |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_RIGHT]) btns |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_START]) btns |= X_INPUT_GAMEPAD_START;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_SELECT])btns |= X_INPUT_GAMEPAD_BACK;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_L3])    btns |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_R3])    btns |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_L])     btns |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_R])     btns |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_B])     btns |= X_INPUT_GAMEPAD_A;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_A])     btns |= X_INPUT_GAMEPAD_B;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_Y])     btns |= X_INPUT_GAMEPAD_X;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_X])     btns |= X_INPUT_GAMEPAD_Y;
  return btns;
}

// The direction key for a stick, as XInput reports it: one of eight, past
// the stick's dead zone, or none (0). x right and y up positive.
static uint16_t StickKey(int x, int y, int deadzone, uint16_t up) {
  const bool u = y > deadzone, d = y < -deadzone;
  const bool r = x > deadzone, l = x < -deadzone;
  // Up, Down, Right, Left, UpLeft, UpRight, DownRight, DownLeft follow
  // each other from the Up key
  if (u && l) return up + 4;
  if (u && r) return up + 5;
  if (d && r) return up + 6;
  if (d && l) return up + 7;
  if (u) return up;
  if (d) return up + 1;
  if (r) return up + 2;
  if (l) return up + 3;
  return 0;
}

// ---- LibretroInputDriver --------------------------------------------------

LibretroInputDriver::LibretroInputDriver(xe::ui::Window* window,
                                         size_t window_z_order)
    : InputDriver(window, window_z_order) {
  // Mark all ports connected (RetroArch always has port 0).
  for (size_t i = 0; i < kMaxPorts; ++i) {
    states_[i].connected = (i == 0);
  }
}

LibretroInputDriver::~LibretroInputDriver() = default;

X_STATUS LibretroInputDriver::Setup() { return X_STATUS_SUCCESS; }

InputType LibretroInputDriver::GetInputType() const {
  return InputType::Controller;
}

std::vector<InputDeviceInfo> LibretroInputDriver::EnumerateDevices() {
  std::vector<InputDeviceInfo> devices;
  for (size_t i = 0; i < kMaxPorts; ++i) {
    InputDeviceInfo info;
    info.driver_slot = static_cast<uint8_t>(i);
    info.stable_id = "libretro:" + std::to_string(i);
    info.display_name = "RetroPad " + std::to_string(i + 1);
    info.preferred_slot = static_cast<int8_t>(i);
    devices.push_back(std::move(info));
  }
  return devices;
}

X_RESULT LibretroInputDriver::GetCapabilities(uint32_t user_index,
                                               uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!states_[user_index].connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  // be<T> auto byte-swaps - assign native values
  out_caps->flags = X_INPUT_CAPS_FFB_SUPPORTED;

  // Report full gamepad capability
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = 32767;
  out_caps->gamepad.thumb_ly = 32767;
  out_caps->gamepad.thumb_rx = 32767;
  out_caps->gamepad.thumb_ry = 32767;
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;

  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::GetState(uint32_t user_index,
                                        X_INPUT_STATE* out_state) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto& s = states_[user_index];
  if (!s.connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::memset(out_state, 0, sizeof(*out_state));
  // be<T> auto byte-swaps - assign native values
  out_state->packet_number = s.packet_number;

  // Map libretro joypad buttons -> Xbox 360 button bitmask
  out_state->gamepad.buttons = XboxButtons(s);

  // Triggers: libretro 0..+0x7FFF ??? Xbox 0..255
  out_state->gamepad.left_trigger =
      static_cast<uint8_t>((s.left_trigger * 255) / 0x7FFF);
  out_state->gamepad.right_trigger =
      static_cast<uint8_t>((s.right_trigger * 255) / 0x7FFF);

  // Sticks: libretro -0x7FFF..+0x7FFF ??? Xbox -32768..+32767 (Y axis inverted).
  int16_t lx = s.left_stick_x;
  int16_t ly = static_cast<int16_t>(-s.left_stick_y);
  int16_t rx = s.right_stick_x;
  int16_t ry = static_cast<int16_t>(-s.right_stick_y);

  if (lx > -X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
      lx <  X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE) lx = 0;
  if (ly > -X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
      ly <  X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ly = 0;
  if (rx > -X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE &&
      rx <  X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) rx = 0;
  if (ry > -X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE &&
      ry <  X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) ry = 0;

  out_state->gamepad.thumb_lx = lx;
  out_state->gamepad.thumb_ly = ly;
  out_state->gamepad.thumb_rx = rx;
  out_state->gamepad.thumb_ry = ry;

  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::SetState(uint32_t user_index,
                                        X_INPUT_VIBRATION* vibration) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!states_[user_index].connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  if (rumble_cb_ && vibration) {
    // be<T> auto-converts to native on read
    uint16_t strong = vibration->left_motor_speed;
    uint16_t weak   = vibration->right_motor_speed;
    rumble_cb_(static_cast<unsigned>(user_index),
               RETRO_RUMBLE_STRONG, strong);
    rumble_cb_(static_cast<unsigned>(user_index),
               RETRO_RUMBLE_WEAK, weak);
  }
  return X_ERROR_SUCCESS;
}

// XInputGetKeystroke for the RetroPad. Many titles drive their menus with it
// rather than with the state (Minecraft's main menu took no input at all while
// this said EMPTY), so the button, trigger and stick changes UpdateFromLibretro
// saw are handed out one at a time, oldest first.
X_RESULT LibretroInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!states_[user_index].connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  auto& queue = keystrokes_[user_index].queue;
  if (queue.empty()) return X_ERROR_EMPTY;
  *out_keystroke = queue.front();
  queue.pop_front();
  return X_ERROR_SUCCESS;
}

// Turns the state just polled into keystrokes: a key down for each key that
// is now held and was not, a key up for each that was and is not, and, as
// XInput does, repeats for a held key - first after 400 ms, then every 100.
void LibretroInputDriver::UpdateKeystrokes(size_t port) {
  using namespace std::chrono;
  using xe::ui::VirtualKey;
  auto& s = states_[port];
  auto& k = keystrokes_[port];
  if (!s.connected) {
    k.queue.clear();
    k.held.clear();
    k.next_repeat.clear();
    return;
  }

  static const struct { uint16_t mask; VirtualKey key; } kButtons[] = {
      {X_INPUT_GAMEPAD_A, VirtualKey::kXInputPadA},
      {X_INPUT_GAMEPAD_B, VirtualKey::kXInputPadB},
      {X_INPUT_GAMEPAD_X, VirtualKey::kXInputPadX},
      {X_INPUT_GAMEPAD_Y, VirtualKey::kXInputPadY},
      {X_INPUT_GAMEPAD_RIGHT_SHOULDER, VirtualKey::kXInputPadRShoulder},
      {X_INPUT_GAMEPAD_LEFT_SHOULDER, VirtualKey::kXInputPadLShoulder},
      {X_INPUT_GAMEPAD_DPAD_UP, VirtualKey::kXInputPadDpadUp},
      {X_INPUT_GAMEPAD_DPAD_DOWN, VirtualKey::kXInputPadDpadDown},
      {X_INPUT_GAMEPAD_DPAD_LEFT, VirtualKey::kXInputPadDpadLeft},
      {X_INPUT_GAMEPAD_DPAD_RIGHT, VirtualKey::kXInputPadDpadRight},
      {X_INPUT_GAMEPAD_START, VirtualKey::kXInputPadStart},
      {X_INPUT_GAMEPAD_BACK, VirtualKey::kXInputPadBack},
      {X_INPUT_GAMEPAD_LEFT_THUMB, VirtualKey::kXInputPadLThumbPress},
      {X_INPUT_GAMEPAD_RIGHT_THUMB, VirtualKey::kXInputPadRThumbPress},
  };
  std::vector<uint16_t> now;
  const uint16_t btns = XboxButtons(s);
  for (const auto& b : kButtons)
    if (btns & b.mask) now.push_back(static_cast<uint16_t>(b.key));
  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD is 30 of 255
  if (s.left_trigger > 30 * 0x7FFF / 255)
    now.push_back(static_cast<uint16_t>(VirtualKey::kXInputPadLTrigger));
  if (s.right_trigger > 30 * 0x7FFF / 255)
    now.push_back(static_cast<uint16_t>(VirtualKey::kXInputPadRTrigger));
  // RetroArch's y is down positive, XInput's up positive
  if (uint16_t key = StickKey(s.left_stick_x, -s.left_stick_y,
                              X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
                              static_cast<uint16_t>(VirtualKey::kXInputPadLThumbUp)))
    now.push_back(key);
  if (uint16_t key = StickKey(s.right_stick_x, -s.right_stick_y,
                              X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE,
                              static_cast<uint16_t>(VirtualKey::kXInputPadRThumbUp)))
    now.push_back(key);

  const auto t = steady_clock::now();
  auto push = [&](uint16_t key, uint16_t flags) {
    // A title that never reads keystrokes must not grow this without end
    if (k.queue.size() >= 64) k.queue.pop_front();
    X_INPUT_KEYSTROKE ks;
    std::memset(&ks, 0, sizeof(ks));
    ks.virtual_key = key;
    ks.flags = flags;
    ks.user_index = static_cast<uint8_t>(port);
    k.queue.push_back(ks);
  };
  for (size_t i = 0; i < k.held.size(); ++i)
    if (std::find(now.begin(), now.end(), k.held[i]) == now.end())
      push(k.held[i], X_INPUT_KEYSTROKE_KEYUP);
  std::vector<steady_clock::time_point> next(now.size());
  for (size_t i = 0; i < now.size(); ++i) {
    auto it = std::find(k.held.begin(), k.held.end(), now[i]);
    if (it == k.held.end()) {
      push(now[i], X_INPUT_KEYSTROKE_KEYDOWN);
      next[i] = t + milliseconds(400);
    } else {
      next[i] = k.next_repeat[it - k.held.begin()];
      if (t >= next[i]) {
        push(now[i], X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT);
        next[i] = t + milliseconds(100);
      }
    }
  }
  k.held = std::move(now);
  k.next_repeat = std::move(next);
}

void LibretroInputDriver::UpdateFromLibretro(
    retro_input_state_t input_state_cb) {
  if (!input_state_cb) return;

  std::lock_guard<std::mutex> lock(state_mutex_);

  for (size_t port = 0; port < kMaxPorts; ++port) {
    auto& s = states_[port];

    // Read digital buttons
    bool any_input = false;
    for (int id = 0; id < 16; ++id) {
      s.buttons[id] = input_state_cb(
          static_cast<unsigned>(port), RETRO_DEVICE_JOYPAD, 0, id);
      if (s.buttons[id]) any_input = true;
    }

    // Read analog sticks
    s.left_stick_x = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    s.left_stick_y = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    s.right_stick_x = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    s.right_stick_y = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

    if (s.left_stick_x || s.left_stick_y ||
        s.right_stick_x || s.right_stick_y)
      any_input = true;

    // Read analog triggers (L2/R2): try analog first, fallback to digital.
    int16_t lt = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
    int16_t rt = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2);

    if (lt == 0 && s.buttons[RETRO_DEVICE_ID_JOYPAD_L2])
      lt = 0x7FFF;  // digital fallback
    if (rt == 0 && s.buttons[RETRO_DEVICE_ID_JOYPAD_R2])
      rt = 0x7FFF;

    s.left_trigger = lt;
    s.right_trigger = rt;
    if (lt || rt) any_input = true;

    // A port is connected if the frontend assigned a device to it.
    s.connected = port_connected_[port];

    if (any_input && s.connected) s.packet_number++;
    UpdateKeystrokes(port);
  }
}

void LibretroInputDriver::SetRumbleCallback(retro_set_rumble_state_t cb) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  rumble_cb_ = cb;
}

void LibretroInputDriver::SetPortDevice(unsigned port, unsigned device) {
  if (port >= kMaxPorts) return;
  std::lock_guard<std::mutex> lock(state_mutex_);
  // RETRO_DEVICE_NONE (0) = disconnected, other = connected.
  port_connected_[port] = (device != RETRO_DEVICE_NONE);
  states_[port].connected = port_connected_[port];
  if (!port_connected_[port]) {
    // Clear state for disconnected port
    std::memset(&states_[port], 0, sizeof(states_[port]));
  }
}

// ---- Factory --------------------------------------------------------------

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order) {
  return std::make_unique<LibretroInputDriver>(window, window_z_order);
}

}  // namespace libretro_hid
}  // namespace hid
}  // namespace xe
