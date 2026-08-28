// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace SwitchFrontend {

// A physical button on the Switch pad.
enum class InputButton : std::uint8_t {
    A,
    B,
    X,
    Y,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    Start,
    Select,
    ZL,
    ZR,
    L3,
    R3,
    None,
};

// The number of physical buttons the user can bind a control to.
inline constexpr int NumPhysicalButtons = static_cast<int>(InputButton::R3) + 1;

inline constexpr int NumBindingChoices = NumPhysicalButtons + 1;

// A control the player can remap.
enum class MappableControl : std::uint8_t {
    A,
    B,
    X,
    Y,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    Start,
    Select,
    ZL,
    ZR,
    TogglePointer,
    CycleLayout,
    TouchTap,
    SwapScreens,
    Count,
};

inline constexpr int NumMappableControls = static_cast<int>(MappableControl::Count);

// An unbound control gets an empty mask.
constexpr std::uint64_t ButtonMask(InputButton button) {
    return button == InputButton::None ? 0
                                       : std::uint64_t{1} << static_cast<std::uint8_t>(button);
}

// A six-axis sample nicely borrowed (read stolen) from libnx.
struct MotionState {
    bool active{}; /// False when the current controller has no readable sensor
    float accel_x{};
    float accel_y{};
    float accel_z{};
    float gyro_x{};
    float gyro_y{};
    float gyro_z{};
};

struct InputState {
    std::uint64_t buttons{};
    std::int32_t left_x{};
    std::int32_t left_y{};
    std::int32_t right_x{};
    std::int32_t right_y{};
    bool touch_pressed{};
    std::uint32_t touch_x{};
    std::uint32_t touch_y{};
    MotionState motion{};
};

// What drives the on-screen touch pointer while pointer mode is on. The values are written to
// config.ini, so only append to this list.
enum class PointerSource : std::uint8_t {
    LeftStick,
    Gyro,
    RightStick,
    Count,
};

inline constexpr int NumPointerSources = static_cast<int>(PointerSource::Count);

// Which physical motion sensor drives the emulated 3DS sensors.
enum class GyroSource : std::uint8_t {
    Automatic,
    LeftController,
    RightController,
    Console,
    Count,
};

inline constexpr int NumGyroSources = static_cast<int>(GyroSource::Count);

enum class GyroOrientation : std::uint8_t {
    Horizontal,
    Vertical,
    Count,
};

inline constexpr int NumGyroOrientations = static_cast<int>(GyroOrientation::Count);

// The bottom-screen pointer position as a fraction of the bottom screen, for the crosshair.
struct PointerCursor {
    bool active{};
    float frac_x{}; // 0 at the left edge, 1 at the right edge.
    float frac_y{}; // 0 at the top edge, 1 at the bottom edge.
};

// Registers the Switch controller and sets up the default 3DS bindings.
void InitializeInput();

// The physical Switch button currently bound to `control`.
InputButton GetMapping(MappableControl control);

// Binds `control` to a physical Switch button. Guest-button changes take effect on the next
// ApplyButtonMappings(). Emulator-action bindings are read live.
void SetMapping(MappableControl control, InputButton button);

// The default physical button for `control`.
InputButton DefaultMapping(MappableControl control);

// Rebuilds the guest input profile from the current guest-button mappings.
void ApplyButtonMappings();

// Display name of a control.
const char* ControlName(MappableControl control);

// Display name of a physical Switch button.
const char* PhysicalButtonName(InputButton button);

// Stable config.ini key for a control's binding.
const char* ControlConfigKey(MappableControl control);

// Pushes the latest libnx input to the emulation thread.
void UpdateInput(const InputState& state);

// Releases all input and unregisters the Switch controller.
void ShutdownInput();

// The configured pointer driver.
PointerSource GetPointerSource();
void SetPointerSource(PointerSource source);

// Display name of a pointer driver.
const char* PointerSourceName(PointerSource source);

GyroSource GetGyroSource();
void SetGyroSource(GyroSource source);
const char* GyroSourceName(GyroSource source);

// Per-axis gyro pointer sensitivity as a percentage of the default speed (100 = default).
int GetGyroSensitivityX();
int GetGyroSensitivityY();
void SetGyroSensitivity(int x_percent, int y_percent);

GyroOrientation GetGyroOrientation();
void SetGyroOrientation(GyroOrientation orientation);
const char* GyroOrientationName(GyroOrientation orientation);

// Pointer mode lets the stick/gyro drive the 3DS touchscreen with ZL/ZR as the tap.
bool IsPointerModeActive();
void TogglePointerMode();
void SetPointerMode(bool enabled);

// Disables pointer mode and recenters the pointer.
void ResetPointer();

// The current crosshair position, queried by the emu window for the renderer.
PointerCursor GetPointerCursor();

} // namespace SwitchFrontend
