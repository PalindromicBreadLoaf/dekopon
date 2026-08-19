// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>

#include "citra_switch/emu_window.h"
#include "citra_switch/input.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"

// Button mappings and inputs for the Switch
namespace SwitchFrontend {
namespace {

constexpr float kStickRange = 32767.0f;
constexpr float kStickDeadzone = 0.15f;

// libnx reports angular velocity in rotations/sec vs the 3DS gyroscope in deg/sec.
constexpr float kRotationsToDegrees = 360.0f;

// Pointer travel at full stick deflection, in bottom-screens per second.
constexpr float kStickPointerSpeed = 1.5f;

// Pointer travel per full console rotation, in bottom-screens, at 100% sensitivity. The
// per-axis sensitivity percentages scale this base speed.
constexpr float kGyroPointerSpeed = 6.0f;

// Bounds for the user-tunable gyro pointer sensitivity, as a percentage of kGyroPointerSpeed.
constexpr int kGyroSensitivityMin = 10;
constexpr int kGyroSensitivityMax = 500;

// Sign of the gyro->pointer mapping.
constexpr float kGyroSignX = -1.0f;
constexpr float kGyroSignY = -1.0f;

// Ignore stalls so a resumed frame can't fling the pointer (Although it was funny to watch).
constexpr float kMaxPointerDelta = 0.1f;

// What the 3DS accelerometer reads while lying face-up, which is the orientation a Switch held upright maps onto.
constexpr Common::Vec3<float> kRestAccel{0.0f, -1.0f, 0.0f};

std::atomic<std::uint64_t> s_buttons{};
std::array<std::atomic<float>, Settings::NativeAnalog::NumAnalogs> s_stick_x{};
std::array<std::atomic<float>, Settings::NativeAnalog::NumAnalogs> s_stick_y{};
std::array<std::atomic<float>, 3> s_accel{};
std::array<std::atomic<float>, 3> s_gyro{};
bool s_touch_active{};

// The position is stored as a fraction of the bottom screen so it stays valid across layout
// changes and can never leave the screen (it is clamped to [0, 1]).
std::atomic<PointerSource> s_pointer_source{PointerSource::LeftStick};
std::atomic<GyroSource> s_gyro_source{GyroSource::Automatic};
std::atomic<int> s_gyro_sensitivity_x{100};
std::atomic<int> s_gyro_sensitivity_y{100};
std::atomic<GyroOrientation> s_gyro_orientation{GyroOrientation::Horizontal};
std::atomic<bool> s_pointer_mode{false};
std::atomic<float> s_pointer_fx{0.5f};
std::atomic<float> s_pointer_fy{0.5f};

// The physical Switch button bound to each control. Written from the menu thread, read on the
// input/emulation threads, hence the atomics.
std::array<std::atomic<InputButton>, NumMappableControls> s_mapping{};

// The 3DS button each of the first fourteen controls drives. The controls line up with these in order.
constexpr std::array<Settings::NativeButton::Values, 14> kControlToNative{{
    Settings::NativeButton::A,      Settings::NativeButton::B,
    Settings::NativeButton::X,      Settings::NativeButton::Y,
    Settings::NativeButton::Up,     Settings::NativeButton::Down,
    Settings::NativeButton::Left,   Settings::NativeButton::Right,
    Settings::NativeButton::L,      Settings::NativeButton::R,
    Settings::NativeButton::Start,  Settings::NativeButton::Select,
    Settings::NativeButton::ZL,     Settings::NativeButton::ZR,
}};

// The default (recommended) control layout.
constexpr std::array<InputButton, NumMappableControls> kDefaultMapping{{
    InputButton::A,     InputButton::B,      InputButton::X,     InputButton::Y,
    InputButton::Up,    InputButton::Down,   InputButton::Left,  InputButton::Right,
    InputButton::L,     InputButton::R,      InputButton::Start, InputButton::Select,
    InputButton::ZL,    InputButton::ZR,
    InputButton::L3, // TogglePointer
    InputButton::R3, // CycleLayout
    InputButton::ZR, // TouchTap
    // The player can set this one since I don't know what to map it to.
    InputButton::None, // SwapScreens
}};

// Seeds the mappings with their defaults at static-init time, before the config is loaded.
[[maybe_unused]] const bool s_mapping_seeded = [] {
    for (int i = 0; i < NumMappableControls; ++i) {
        s_mapping[i].store(kDefaultMapping[i], std::memory_order_relaxed);
    }
    return true;
}();

// Elapsed time since the previous UpdateInput.
float PointerDeltaSeconds() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    return std::clamp(dt, 0.0f, kMaxPointerDelta);
}

class SwitchButton final : public Input::ButtonDevice {
public:
    explicit SwitchButton(InputButton button_) : button{button_} {}

    bool GetStatus() const override {
        return (s_buttons.load(std::memory_order_relaxed) & ButtonMask(button)) != 0;
    }

private:
    InputButton button;
};

class SwitchAnalog final : public Input::AnalogDevice {
public:
    explicit SwitchAnalog(std::size_t analog_) : analog{analog_} {}

    std::tuple<float, float> GetStatus() const override {
        return {s_stick_x[analog].load(std::memory_order_relaxed),
                s_stick_y[analog].load(std::memory_order_relaxed)};
    }

private:
    std::size_t analog;
};

class SwitchMotion final : public Input::MotionDevice {
public:
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        return {Load(s_accel), Load(s_gyro)};
    }

private:
    static Common::Vec3<float> Load(const std::array<std::atomic<float>, 3>& source) {
        return {source[0].load(std::memory_order_relaxed),
                source[1].load(std::memory_order_relaxed),
                source[2].load(std::memory_order_relaxed)};
    }
};

class SwitchButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        const int button =
            std::clamp(params.Get("button", 0), 0, static_cast<int>(InputButton::None));
        return std::make_unique<SwitchButton>(static_cast<InputButton>(button));
    }
};

class SwitchAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const int analog =
            std::clamp(params.Get("analog", 0), 0, Settings::NativeAnalog::NumAnalogs - 1);
        return std::make_unique<SwitchAnalog>(static_cast<std::size_t>(analog));
    }
};

class SwitchMotionFactory final : public Input::Factory<Input::MotionDevice> {
public:
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<SwitchMotion>();
    }
};

std::string ButtonParam(InputButton button) {
    return Common::ParamPackage{
        {"engine", "switch"},
        {"button", std::to_string(static_cast<int>(button))},
    }
        .Serialize();
}

std::string AnalogParam(Settings::NativeAnalog::Values analog) {
    return Common::ParamPackage{
        {"engine", "switch"},
        {"analog", std::to_string(static_cast<int>(analog))},
    }
        .Serialize();
}

std::string MotionParam() {
    return Common::ParamPackage{{"engine", "switch"}}.Serialize();
}

// The Switch and 3DS sensor frames are one rotation apart: the 3DS's x+ (left), y+ (out of the
// touch screen) and z+ (up) read off the Switch's -x, z and y respectively.
Common::Vec3<float> ToConsoleFrame(float x, float y, float z) {
    return {-x, z, y};
}

// Expresses a console-frame vector in the same orientation in which the renderer displays the 3DS.
Common::Vec3<float> OrientToLayout(const Common::Vec3<float>& vector,
                                   Layout::DisplayOrientation orientation) {
    switch (orientation) {
    case Layout::DisplayOrientation::Portrait:
        return {-vector.z, vector.y, vector.x};
    case Layout::DisplayOrientation::LandscapeFlipped:
        return {-vector.x, vector.y, -vector.z};
    case Layout::DisplayOrientation::PortraitFlipped:
        return {vector.z, vector.y, -vector.x};
    case Layout::DisplayOrientation::Landscape:
    default:
        return vector;
    }
}

void StoreMotion(const Common::Vec3<float>& accel, const Common::Vec3<float>& gyro) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        s_accel[axis].store(accel[axis], std::memory_order_relaxed);
        s_gyro[axis].store(gyro[axis], std::memory_order_relaxed);
    }
}

std::tuple<float, float> NormalizeStick(std::int32_t raw_x, std::int32_t raw_y) {
    const float x = std::clamp(static_cast<float>(raw_x) / kStickRange, -1.0f, 1.0f);
    const float y = std::clamp(static_cast<float>(raw_y) / kStickRange, -1.0f, 1.0f);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= kStickDeadzone) {
        return {0.0f, 0.0f};
    }

    const float scaled_magnitude =
        (std::min(magnitude, 1.0f) - kStickDeadzone) / (1.0f - kStickDeadzone);
    return {x / magnitude * scaled_magnitude, y / magnitude * scaled_magnitude};
}

// Moves the pointer this frame from either the (already-normalized) driving stick or the gyro,
// then clamps it to the bottom screen.
void AdvancePointer(const InputState& state, float dt, float stick_x, float stick_y) {
    float dfx = 0.0f;
    float dfy = 0.0f;
    if (s_pointer_source.load(std::memory_order_relaxed) != PointerSource::Gyro) {
        dfx = stick_x * kStickPointerSpeed * dt;
        dfy = -stick_y * kStickPointerSpeed * dt; // stick up is +y, but the screen's top is frac 0.
    } else if (state.motion.active) {
        const float yaw = state.motion.gyro_y;   // vertical rotation
        const float pitch = state.motion.gyro_x; // horizontal rotation
        const float speed_x =
            kGyroPointerSpeed * s_gyro_sensitivity_x.load(std::memory_order_relaxed) / 100.0f;
        const float speed_y =
            kGyroPointerSpeed * s_gyro_sensitivity_y.load(std::memory_order_relaxed) / 100.0f;
        dfx = kGyroSignX * yaw * speed_x * dt;
        dfy = kGyroSignY * pitch * speed_y * dt;
    }
    s_pointer_fx.store(std::clamp(s_pointer_fx.load(std::memory_order_relaxed) + dfx, 0.0f, 1.0f),
                       std::memory_order_relaxed);
    s_pointer_fy.store(std::clamp(s_pointer_fy.load(std::memory_order_relaxed) + dfy, 0.0f, 1.0f),
                       std::memory_order_relaxed);
}

// Sticks, motion, and touch never change.
void SetProfileDefaults() {
    auto& profile = Settings::values.current_input_profile;
    profile.name = "Nintendo Switch";
    profile.buttons.fill("engine:null");
    profile.analogs[Settings::NativeAnalog::CirclePad] =
        AnalogParam(Settings::NativeAnalog::CirclePad);
    profile.analogs[Settings::NativeAnalog::CStick] = AnalogParam(Settings::NativeAnalog::CStick);
    profile.motion_device = MotionParam();
    profile.touch_device = "engine:emu_window";
    profile.controller_touch_device.clear();
    profile.use_touchpad = false;
    profile.use_touch_from_button = false;
}

} // namespace

InputButton GetMapping(MappableControl control) {
    return s_mapping[static_cast<int>(control)].load(std::memory_order_relaxed);
}

void SetMapping(MappableControl control, InputButton button) {
    s_mapping[static_cast<int>(control)].store(button, std::memory_order_relaxed);
}

InputButton DefaultMapping(MappableControl control) {
    return kDefaultMapping[static_cast<int>(control)];
}

void ApplyButtonMappings() {
    auto& profile = Settings::values.current_input_profile;
    for (int i = 0; i < static_cast<int>(kControlToNative.size()); ++i) {
        const InputButton button = GetMapping(static_cast<MappableControl>(i));
        profile.buttons[kControlToNative[i]] =
            button == InputButton::None ? "engine:null" : ButtonParam(button);
    }
}

const char* ControlName(MappableControl control) {
    switch (control) {
    case MappableControl::A:
        return "A";
    case MappableControl::B:
        return "B";
    case MappableControl::X:
        return "X";
    case MappableControl::Y:
        return "Y";
    case MappableControl::Up:
        return "D-Pad Up";
    case MappableControl::Down:
        return "D-Pad Down";
    case MappableControl::Left:
        return "D-Pad Left";
    case MappableControl::Right:
        return "D-Pad Right";
    case MappableControl::L:
        return "L";
    case MappableControl::R:
        return "R";
    case MappableControl::Start:
        return "Start";
    case MappableControl::Select:
        return "Select";
    case MappableControl::ZL:
        return "ZL";
    case MappableControl::ZR:
        return "ZR";
    case MappableControl::TogglePointer:
        return "Toggle Touch Pointer";
    case MappableControl::CycleLayout:
        return "Cycle Screen Layout";
    case MappableControl::TouchTap:
        return "Touch Tap (pointer mode)";
    case MappableControl::SwapScreens:
        return "Swap Screens";
    case MappableControl::Count:
        break;
    }
    return "";
}

const char* PhysicalButtonName(InputButton button) {
    switch (button) {
    case InputButton::A:
        return "A";
    case InputButton::B:
        return "B";
    case InputButton::X:
        return "X";
    case InputButton::Y:
        return "Y";
    case InputButton::Up:
        return "D-Pad Up";
    case InputButton::Down:
        return "D-Pad Down";
    case InputButton::Left:
        return "D-Pad Left";
    case InputButton::Right:
        return "D-Pad Right";
    case InputButton::L:
        return "L";
    case InputButton::R:
        return "R";
    case InputButton::Start:
        return "+ (Plus)";
    case InputButton::Select:
        return "- (Minus)";
    case InputButton::ZL:
        return "ZL";
    case InputButton::ZR:
        return "ZR";
    case InputButton::L3:
        return "L3 (Left Stick)";
    case InputButton::R3:
        return "R3 (Right Stick)";
    case InputButton::None:
        return "Unbound";
    }
    return "";
}

const char* ControlConfigKey(MappableControl control) {
    switch (control) {
    case MappableControl::A:
        return "map_a";
    case MappableControl::B:
        return "map_b";
    case MappableControl::X:
        return "map_x";
    case MappableControl::Y:
        return "map_y";
    case MappableControl::Up:
        return "map_up";
    case MappableControl::Down:
        return "map_down";
    case MappableControl::Left:
        return "map_left";
    case MappableControl::Right:
        return "map_right";
    case MappableControl::L:
        return "map_l";
    case MappableControl::R:
        return "map_r";
    case MappableControl::Start:
        return "map_start";
    case MappableControl::Select:
        return "map_select";
    case MappableControl::ZL:
        return "map_zl";
    case MappableControl::ZR:
        return "map_zr";
    case MappableControl::TogglePointer:
        return "map_toggle_pointer";
    case MappableControl::CycleLayout:
        return "map_cycle_layout";
    case MappableControl::TouchTap:
        return "map_touch_tap";
    case MappableControl::SwapScreens:
        return "map_swap_screens";
    case MappableControl::Count:
        break;
    }
    return "";
}

void InitializeInput() {
    Input::RegisterFactory<Input::ButtonDevice>("switch", std::make_shared<SwitchButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>("switch", std::make_shared<SwitchAnalogFactory>());
    Input::RegisterFactory<Input::MotionDevice>("switch", std::make_shared<SwitchMotionFactory>());
    StoreMotion(kRestAccel, {});
    SetProfileDefaults();
    ApplyButtonMappings();
}

void UpdateInput(const InputState& state) {
    const auto [left_x, left_y] = NormalizeStick(state.left_x, state.left_y);
    const auto [right_x, right_y] = NormalizeStick(state.right_x, state.right_y);
    const bool pointer_mode = s_pointer_mode.load(std::memory_order_relaxed);
    const PointerSource pointer_source = s_pointer_source.load(std::memory_order_relaxed);
    const bool left_pointer = pointer_mode && pointer_source == PointerSource::LeftStick;
    const bool right_pointer = pointer_mode && pointer_source == PointerSource::RightStick;

    // In pointer mode the tap button taps the touchscreen instead of reaching the guest.
    const std::uint64_t tap_mask =
        ButtonMask(s_mapping[static_cast<int>(MappableControl::TouchTap)].load(
            std::memory_order_relaxed));
    const bool tap = pointer_mode && (state.buttons & tap_mask) != 0;
    const std::uint64_t buttons = pointer_mode ? state.buttons & ~tap_mask : state.buttons;
    s_buttons.store(buttons, std::memory_order_relaxed);

    // Whichever stick drives the pointer is freed from the pad while it does so.
    s_stick_x[Settings::NativeAnalog::CirclePad].store(left_pointer ? 0.0f : left_x,
                                                       std::memory_order_relaxed);
    s_stick_y[Settings::NativeAnalog::CirclePad].store(left_pointer ? 0.0f : left_y,
                                                       std::memory_order_relaxed);
    s_stick_x[Settings::NativeAnalog::CStick].store(right_pointer ? 0.0f : right_x,
                                                    std::memory_order_relaxed);
    s_stick_y[Settings::NativeAnalog::CStick].store(right_pointer ? 0.0f : right_y,
                                                    std::memory_order_relaxed);

    EmuWindow_Switch* window = GetEmuWindow();
    Layout::DisplayOrientation orientation = Layout::DisplayOrientation::Landscape;
    if (GetGyroOrientation() == GyroOrientation::Vertical) {
        orientation = Layout::DisplayOrientation::Portrait;
        if (window) {
            const Layout::DisplayOrientation layout_orientation =
                window->GetFramebufferLayout().GetOrientation();
            if (layout_orientation != Layout::DisplayOrientation::Landscape) {
                orientation = layout_orientation;
            }
        }
    }
    if (state.motion.active) {
        StoreMotion(OrientToLayout(ToConsoleFrame(state.motion.accel_x, state.motion.accel_y,
                                                  state.motion.accel_z),
                                   orientation),
                    OrientToLayout(ToConsoleFrame(state.motion.gyro_x, state.motion.gyro_y,
                                                  state.motion.gyro_z),
                                   orientation) *
                        kRotationsToDegrees);
    } else {
        StoreMotion(kRestAccel, {});
    }

    const float dt = PointerDeltaSeconds();
    if (pointer_mode) {
        AdvancePointer(state, dt, right_pointer ? right_x : left_x,
                       right_pointer ? right_y : left_y);
    }

    if (!window) {
        s_touch_active = false;
        return;
    }

    // A physical touch always wins in a conflict.
    bool touch_pressed = state.touch_pressed;
    unsigned touch_x = state.touch_x;
    unsigned touch_y = state.touch_y;
    if (!touch_pressed && tap) {
        const auto& bottom = window->GetFramebufferLayout().bottom_screen;
        touch_pressed = true;
        touch_x = std::min(static_cast<unsigned>(bottom.left +
                                                 s_pointer_fx.load(std::memory_order_relaxed) *
                                                     bottom.GetWidth()),
                           bottom.right - 1);
        touch_y = std::min(static_cast<unsigned>(bottom.top +
                                                 s_pointer_fy.load(std::memory_order_relaxed) *
                                                     bottom.GetHeight()),
                           bottom.bottom - 1);
    }

    if (touch_pressed) {
        if (s_touch_active) {
            window->TouchMoved(touch_x, touch_y);
        } else {
            s_touch_active = window->TouchPressed(touch_x, touch_y);
        }
    } else if (s_touch_active) {
        window->TouchReleased();
        s_touch_active = false;
    }
}

PointerSource GetPointerSource() {
    return s_pointer_source.load(std::memory_order_relaxed);
}

void SetPointerSource(PointerSource source) {
    if (source >= PointerSource::Count) {
        source = PointerSource::LeftStick;
    }
    s_pointer_source.store(source, std::memory_order_relaxed);
}

const char* PointerSourceName(PointerSource source) {
    switch (source) {
    case PointerSource::LeftStick:
        return "Left Stick";
    case PointerSource::Gyro:
        return "Gyro";
    case PointerSource::RightStick:
        return "Right Stick";
    case PointerSource::Count:
        break;
    }
    return "";
}

GyroSource GetGyroSource() {
    return s_gyro_source.load(std::memory_order_relaxed);
}

void SetGyroSource(GyroSource source) {
    if (source >= GyroSource::Count) {
        source = GyroSource::Automatic;
    }
    s_gyro_source.store(source, std::memory_order_relaxed);
}

const char* GyroSourceName(GyroSource source) {
    switch (source) {
    case GyroSource::Automatic:
        return "Automatic (Default)";
    case GyroSource::LeftController:
        return "Left Controller";
    case GyroSource::RightController:
        return "Right Controller";
    case GyroSource::Console:
        return "Console / Handheld";
    case GyroSource::Count:
        break;
    }
    return "";
}

int GetGyroSensitivityX() {
    return s_gyro_sensitivity_x.load(std::memory_order_relaxed);
}

int GetGyroSensitivityY() {
    return s_gyro_sensitivity_y.load(std::memory_order_relaxed);
}

void SetGyroSensitivity(int x_percent, int y_percent) {
    s_gyro_sensitivity_x.store(std::clamp(x_percent, kGyroSensitivityMin, kGyroSensitivityMax),
                               std::memory_order_relaxed);
    s_gyro_sensitivity_y.store(std::clamp(y_percent, kGyroSensitivityMin, kGyroSensitivityMax),
                               std::memory_order_relaxed);
}

GyroOrientation GetGyroOrientation() {
    return s_gyro_orientation.load(std::memory_order_relaxed);
}

void SetGyroOrientation(GyroOrientation orientation) {
    s_gyro_orientation.store(orientation, std::memory_order_relaxed);
}

const char* GyroOrientationName(GyroOrientation orientation) {
    return orientation == GyroOrientation::Vertical ? "Vertical" : "Horizontal (Default)";
}

bool IsPointerModeActive() {
    return s_pointer_mode.load(std::memory_order_relaxed);
}

void TogglePointerMode() {
    s_pointer_mode.store(!s_pointer_mode.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void SetPointerMode(bool enabled) {
    s_pointer_mode.store(enabled, std::memory_order_relaxed);
}

void ResetPointer() {
    s_pointer_mode.store(false, std::memory_order_relaxed);
    s_pointer_fx.store(0.5f, std::memory_order_relaxed);
    s_pointer_fy.store(0.5f, std::memory_order_relaxed);
}

PointerCursor GetPointerCursor() {
    return {s_pointer_mode.load(std::memory_order_relaxed),
            s_pointer_fx.load(std::memory_order_relaxed),
            s_pointer_fy.load(std::memory_order_relaxed)};
}

void ShutdownInput() {
    UpdateInput({});
    Input::UnregisterFactory<Input::ButtonDevice>("switch");
    Input::UnregisterFactory<Input::AnalogDevice>("switch");
    Input::UnregisterFactory<Input::MotionDevice>("switch");
    Input::UnregisterFactory<Input::TouchDevice>("emu_window");
}

} // namespace SwitchFrontend
