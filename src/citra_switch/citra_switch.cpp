// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <switch.h>

#include "citra_switch/applets/swkbd.h"
#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/menu.h"
#include "citra_switch/overlay_menu.h"
#include "common/horizon_thread.h"

extern "C" {
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

namespace {

constexpr std::array<std::pair<u64, SwitchFrontend::InputButton>, 16> button_map{{
    {HidNpadButton_A, SwitchFrontend::InputButton::A},
    {HidNpadButton_B, SwitchFrontend::InputButton::B},
    {HidNpadButton_X, SwitchFrontend::InputButton::X},
    {HidNpadButton_Y, SwitchFrontend::InputButton::Y},
    {HidNpadButton_Up, SwitchFrontend::InputButton::Up},
    {HidNpadButton_Down, SwitchFrontend::InputButton::Down},
    {HidNpadButton_Left, SwitchFrontend::InputButton::Left},
    {HidNpadButton_Right, SwitchFrontend::InputButton::Right},
    {HidNpadButton_L, SwitchFrontend::InputButton::L},
    {HidNpadButton_R, SwitchFrontend::InputButton::R},
    {HidNpadButton_Plus, SwitchFrontend::InputButton::Start},
    {HidNpadButton_Minus, SwitchFrontend::InputButton::Select},
    {HidNpadButton_ZL, SwitchFrontend::InputButton::ZL},
    {HidNpadButton_ZR, SwitchFrontend::InputButton::ZR},
    {HidNpadButton_StickL, SwitchFrontend::InputButton::L3},
    {HidNpadButton_StickR, SwitchFrontend::InputButton::R3},
}};

// Each controller style reports its six-axis sensor through a handle of its own.
// Sideways Joy-Con is disabled because it's more work and I don't see a big need for it.
std::array<HidSixAxisSensorHandle, 4> six_axis_handles{};

void StartSixAxis() {
    hidGetSixAxisSensorHandles(&six_axis_handles[0], 1, HidNpadIdType_Handheld,
                               HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&six_axis_handles[1], 1, HidNpadIdType_No1,
                               HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&six_axis_handles[2], 2, HidNpadIdType_No1,
                               HidNpadStyleTag_NpadJoyDual);
    for (const HidSixAxisSensorHandle& handle : six_axis_handles) {
        hidStartSixAxisSensor(handle);
    }
}

void StopSixAxis() {
    for (const HidSixAxisSensorHandle& handle : six_axis_handles) {
        hidStopSixAxisSensor(handle);
    }
}

SwitchFrontend::MotionState PollMotion(PadState& pad) {
    const u64 style_set = padGetStyleSet(&pad);
    HidSixAxisSensorState sensor{};
    bool read = false;

    if ((style_set & HidNpadStyleTag_NpadHandheld) != 0) {
        read = hidGetSixAxisSensorStates(six_axis_handles[0], &sensor, 1) > 0;
    } else if ((style_set & HidNpadStyleTag_NpadFullKey) != 0) {
        read = hidGetSixAxisSensorStates(six_axis_handles[1], &sensor, 1) > 0;
    } else if ((style_set & HidNpadStyleTag_NpadJoyDual) != 0) {
        // A dual pair reports through whichever Joy-Con is actually attached.
        const u64 attributes = padGetAttributes(&pad);
        if ((attributes & HidNpadAttribute_IsLeftConnected) != 0) {
            read = hidGetSixAxisSensorStates(six_axis_handles[2], &sensor, 1) > 0;
        } else if ((attributes & HidNpadAttribute_IsRightConnected) != 0) {
            read = hidGetSixAxisSensorStates(six_axis_handles[3], &sensor, 1) > 0;
        }
    }

    if (!read) {
        return {};
    }
    return {
        .active = true,
        .accel_x = sensor.acceleration.x,
        .accel_y = sensor.acceleration.y,
        .accel_z = sensor.acceleration.z,
        .gyro_x = sensor.angular_velocity.x,
        .gyro_y = sensor.angular_velocity.y,
        .gyro_z = sensor.angular_velocity.z,
    };
}

// Reads the pad into `state` and returns the raw buttons held mask.
// Forwarding the state to the guest is left to the caller so it can withhold input while the quick menu is up.
u64 PollInput(PadState& pad, SwitchFrontend::InputState& state) {
    padUpdate(&pad);
    const u64 held = padGetButtons(&pad);
    const HidAnalogStickState left = padGetStickPos(&pad, 0);
    const HidAnalogStickState right = padGetStickPos(&pad, 1);

    state = SwitchFrontend::InputState{
        .left_x = left.x,
        .left_y = left.y,
        .right_x = right.x,
        .right_y = right.y,
        .motion = PollMotion(pad),
    };
    for (const auto& [source, target] : button_map) {
        if ((held & source) != 0) {
            state.buttons |= SwitchFrontend::ButtonMask(target);
        }
    }

    HidTouchScreenState touch{};
    if (hidGetTouchScreenStates(&touch, 1) != 0 && touch.count > 0) {
        state.touch_pressed = true;
        state.touch_x = touch.touches[0].x;
        state.touch_y = touch.touches[0].y;
    }
    return held;
}

void RunGame(PadState& pad, const std::string& rom) {
    if (!SwitchFrontend::CreateWindow(nwindowGetDefault())) {
        std::printf("EmuWindow no worky.\n");
        SwitchFrontend::SetMenuNotice("Couldn't create the render window");
        return;
    }

    // Each game always starts with the touch pointer off and the quick menu closed.
    SwitchFrontend::ResetPointer();

    if (SwitchFrontend::BootRom(rom)) {
        u64 prev_held = 0;
        u64 prev_input = 0;
        while (appletMainLoop()) {
            // Blocks while the system keyboard is up. The emulation thread is waiting on it, so
            // nothing is being drawn meanwhile.
            SwitchFrontend::PumpKeyboard();

            SwitchFrontend::InputState state;
            const u64 held = PollInput(pad, state);
            const u64 pressed = held & ~prev_held;
            // Emulator actions are edge-detected in the remappable InputButton space.
            const u64 input_pressed = state.buttons & ~prev_input;

            // The +/- chord toggles the in-game quick menu.
            constexpr u64 chord = HidNpadButton_Plus | HidNpadButton_Minus;
            const bool chord_edge = (held & chord) == chord && (prev_held & chord) != chord;
            if (chord_edge) {
                SwitchFrontend::ToggleQuickMenu();
            }

            const bool menu_open = SwitchFrontend::IsQuickMenuOpen();

            // While the menu is up the guest sees neutral input.
            SwitchFrontend::UpdateInput(menu_open || chord_edge ? SwitchFrontend::InputState{}
                                                                : state);

            if (menu_open && !chord_edge) {
                const SwitchFrontend::QuickMenuNav nav{
                    .up = (pressed & (HidNpadButton_Up | HidNpadButton_StickLUp)) != 0,
                    .down = (pressed & (HidNpadButton_Down | HidNpadButton_StickLDown)) != 0,
                    .left = (pressed & (HidNpadButton_Left | HidNpadButton_StickLLeft)) != 0,
                    .right = (pressed & (HidNpadButton_Right | HidNpadButton_StickLRight)) != 0,
                    .confirm = (pressed & HidNpadButton_A) != 0,
                    .cancel = (pressed & HidNpadButton_B) != 0,
                    .alt = (pressed & HidNpadButton_X) != 0,
                    .alt2 = (pressed & HidNpadButton_Y) != 0,
                    .tab_prev = (pressed & HidNpadButton_L) != 0,
                    .tab_next = (pressed & HidNpadButton_R) != 0,
                    .page_prev = (pressed & HidNpadButton_ZL) != 0,
                    .page_next = (pressed & HidNpadButton_ZR) != 0,
                };
                if (SwitchFrontend::UpdateQuickMenu(nav) ==
                    SwitchFrontend::QuickMenuAction::ExitGame) {
                    break;
                }
            } else if (!menu_open) {
                using SwitchFrontend::ButtonMask;
                using SwitchFrontend::GetMapping;
                using SwitchFrontend::MappableControl;
                // The button bound to Cycle Screen Layout (R3 by default) steps the layout.
                if ((input_pressed & ButtonMask(GetMapping(MappableControl::CycleLayout))) != 0) {
                    SwitchFrontend::CycleScreenLayout();
                }
                // The button bound to Toggle Touch Pointer (L3 by default) toggles pointer mode.
                if ((input_pressed & ButtonMask(GetMapping(MappableControl::TogglePointer))) != 0) {
                    SwitchFrontend::TogglePointerMode();
                }
                // The button bound to Swap Screens (unbound by default) flips the two screens.
                if ((input_pressed & ButtonMask(GetMapping(MappableControl::SwapScreens))) != 0) {
                    SwitchFrontend::ToggleSwapScreens();
                }
            }

            prev_held = held;
            prev_input = state.buttons;
            if (!SwitchFrontend::IsRunning()) {
                break;
            }
            // The async GPU thread shares core 0 with this loop.
            svcSleepThread(4'000'000);
        }
        SwitchFrontend::StopRom();
        if (SwitchFrontend::ArticDisconnected()) {
            SwitchFrontend::SetMenuNotice(
                "Artic connection failed. Please check the address and tool/server version");
        } else if (SwitchFrontend::LoadFailed()) {
            SwitchFrontend::SetMenuNotice("Couldn't launch. Please check keys/ROM");
        } else if (rom.starts_with("articinio://") || rom.starts_with("articinin://")) {
            const auto installed = SwitchFrontend::GetSystemFileSetupState();
            const bool complete =
                rom.starts_with("articinio://") ? installed.old3ds : installed.new3ds;
            SwitchFrontend::SetMenuNotice(
                complete ? "System-file setup completed"
                         : "System-file setup stopped before all files were installed",
                !complete);
        }
    } else {
        SwitchFrontend::SetMenuNotice("Couldn't launch. ROM not loadable");
    }

    // Make sure a lingering overlay never survives into the next game or the library menu.
    SwitchFrontend::CloseQuickMenu();
    SwitchFrontend::DestroyWindow();
}

} // namespace

int main(int argc, char* argv[]) {
    const bool have_socket = R_SUCCEEDED(socketInitializeDefault());
    if (have_socket) {
        nxlinkStdio();
    }
    // Mount the embedded romfs for deko3D shaders
    const bool have_romfs = R_SUCCEEDED(romfsInit());
    if (!have_romfs) {
        std::printf("Warning: romfsInit() failed.\n");
    }
    if (!Common::Horizon::PinCurrentThread(0)) {
        std::printf("Warning: failed to pin frontend thread to core 0.\n");
    }

    // Resolve SD-card dirs and create folders/files if not present
    const int launch_count = SwitchFrontend::Bootstrap();
    std::printf("FS & logging up (launch #%d). Logs are located at sdmc:/switch/dekopon/log/\n",
                launch_count);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    StartSixAxis();

    SwitchFrontend::InitializeInput();

    std::string pending_rom = (argc > 1 && argv[1] != nullptr) ? argv[1] : std::string{};

    while (appletMainLoop()) {
        std::string rom;
        if (!pending_rom.empty()) {
            rom = std::move(pending_rom);
            pending_rom.clear();
        } else {
            const SwitchFrontend::MenuResult choice = SwitchFrontend::RunMenu(pad);
            if (choice.action == SwitchFrontend::MenuAction::Exit) {
                break;
            }
            rom = choice.path;
        }

        if (!rom.empty()) {
            RunGame(pad, rom);
        }
    }

    SwitchFrontend::ShutdownMenu();
    SwitchFrontend::ShutdownInput();
    StopSixAxis();
    SwitchFrontend::Shutdown();
    if (have_romfs) {
        romfsExit();
    }
    if (have_socket) {
        socketExit();
    }
    return 0;
}
