// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "common/settings.h"
#include "core/frontend/framebuffer_layout.h"

namespace Layout {
namespace {

class CardboardSettingsGuard {
public:
    CardboardSettingsGuard()
        : size{Settings::values.cardboard_screen_size.GetValue()},
          x_shift{Settings::values.cardboard_x_shift.GetValue()},
          y_shift{Settings::values.cardboard_y_shift.GetValue()} {}

    ~CardboardSettingsGuard() {
        Settings::values.cardboard_screen_size = size;
        Settings::values.cardboard_x_shift = x_shift;
        Settings::values.cardboard_y_shift = y_shift;
    }

private:
    u32 size;
    s32 x_shift;
    s32 y_shift;
};

} // namespace

TEST_CASE("Cardboard layout retains stacked screens in each eye", "[core][frontend][layout]") {
    CardboardSettingsGuard guard;
    Settings::values.cardboard_screen_size = 100;
    Settings::values.cardboard_x_shift = 0;
    Settings::values.cardboard_y_shift = 0;

    const FramebufferLayout stacked{
        1280, 720, true, true, {340, 0, 940, 360}, {400, 360, 880, 720},
    };
    const FramebufferLayout vr = GetCardboardSettings(stacked);

    CHECK((vr.top_screen == Common::Rectangle<u32>{170, 180, 470, 360}));
    CHECK((vr.bottom_screen == Common::Rectangle<u32>{200, 360, 440, 540}));
    CHECK(vr.top_screen.bottom <= vr.bottom_screen.top);
    CHECK(vr.cardboard.top_screen_right_eye == vr.top_screen.left);
    CHECK(vr.cardboard.bottom_screen_right_eye == vr.bottom_screen.left);
}

TEST_CASE("Cardboard horizontal alignment mirrors the eye viewports", "[core][frontend][layout]") {
    CardboardSettingsGuard guard;
    Settings::values.cardboard_screen_size = 100;
    Settings::values.cardboard_x_shift = 100;
    Settings::values.cardboard_y_shift = 0;

    const FramebufferLayout top_only{
        1280, 720, true, false, {340, 0, 940, 360}, {400, 360, 880, 720},
    };
    const FramebufferLayout vr = GetCardboardSettings(top_only);

    CHECK((vr.top_screen == Common::Rectangle<u32>{340, 270, 640, 450}));
    CHECK(vr.cardboard.top_screen_right_eye == 0);
    CHECK(vr.cardboard.user_x_shift == 170);
}

} // namespace Layout
