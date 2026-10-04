// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
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

class LayoutSettingsGuard {
public:
    LayoutSettingsGuard()
        : layout_option{Settings::values.layout_option.GetValue()},
          upright_layout_option{Settings::values.upright_layout_option.GetValue()},
          swap_screen{Settings::values.swap_screen.GetValue()},
          upright_screen{Settings::values.upright_screen.GetValue()},
          upright_screen_flipped{Settings::values.upright_screen_flipped.GetValue()},
          use_integer_scaling{Settings::values.use_integer_scaling.GetValue()},
          screen_gap{Settings::values.screen_gap.GetValue()} {
        Settings::values.layout_option = Settings::LayoutOption::Default;
        Settings::values.upright_layout_option = Settings::UprightLayoutOption::Original;
        Settings::values.swap_screen = false;
        Settings::values.upright_screen = false;
        Settings::values.upright_screen_flipped = false;
        Settings::values.use_integer_scaling = false;
        Settings::values.screen_gap = 0;
    }

    ~LayoutSettingsGuard() {
        Settings::values.layout_option = layout_option;
        Settings::values.upright_layout_option = upright_layout_option;
        Settings::values.swap_screen = swap_screen;
        Settings::values.upright_screen = upright_screen;
        Settings::values.upright_screen_flipped = upright_screen_flipped;
        Settings::values.use_integer_scaling = use_integer_scaling;
        Settings::values.screen_gap = screen_gap;
    }

private:
    Settings::LayoutOption layout_option;
    Settings::UprightLayoutOption upright_layout_option;
    bool swap_screen;
    bool upright_screen;
    bool upright_screen_flipped;
    bool use_integer_scaling;
    int screen_gap;
};

constexpr u32 kPanelWidth = 1280;
constexpr u32 kPanelHeight = 720;

// LargeFrameLayout truncates each rectangle edge to an integer separately, so with a screen gap
// the edges drift by a few pixels.
bool Near(u32 actual, u32 expected) {
    return (actual > expected ? actual - expected : expected - actual) <= 4;
}

float UprightAspect(const Common::Rectangle<u32>& rect) {
    return static_cast<float>(rect.GetWidth()) / static_cast<float>(rect.GetHeight());
}

void CheckSpansShortAxis(const Common::Rectangle<u32>& rect) {
    CHECK(Near(rect.top, 0));
    CHECK(Near(rect.bottom, kPanelHeight));
}

void CheckCentredOnLongAxis(const FramebufferLayout& layout) {
    const u32 left = std::min(layout.top_screen.left, layout.bottom_screen.left);
    const u32 right = std::max(layout.top_screen.right, layout.bottom_screen.right);
    CHECK(Near(left, kPanelWidth - right));
}

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

TEST_CASE("Equal-width stack fills the short axis", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;

    const FramebufferLayout layout = EqualWidthFrameLayout(kPanelWidth, kPanelHeight, false, true);

    CHECK(layout.width == kPanelWidth);
    CHECK(layout.height == kPanelHeight);
    CheckSpansShortAxis(layout.top_screen);
    CheckSpansShortAxis(layout.bottom_screen);
    CheckCentredOnLongAxis(layout);
    CHECK(Near(layout.top_screen.right, layout.bottom_screen.left));
    CHECK(std::abs(UprightAspect(layout.top_screen) - 240.0f / 400.0f) < 0.01f);
    CHECK(std::abs(UprightAspect(layout.bottom_screen) - 240.0f / 320.0f) < 0.01f);
}

TEST_CASE("Equal-width stack keeps both screens full width when swapped",
          "[core][frontend][layout]") {
    LayoutSettingsGuard guard;

    const FramebufferLayout layout = EqualWidthFrameLayout(kPanelWidth, kPanelHeight, true, true);

    CheckSpansShortAxis(layout.top_screen);
    CheckSpansShortAxis(layout.bottom_screen);
    CheckCentredOnLongAxis(layout);
    CHECK(Near(layout.bottom_screen.right, layout.top_screen.left));
    CHECK(std::abs(UprightAspect(layout.top_screen) - 240.0f / 400.0f) < 0.01f);
    CHECK(std::abs(UprightAspect(layout.bottom_screen) - 240.0f / 320.0f) < 0.01f);
}

TEST_CASE("Equal-width stack supports the flipped rotation", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;
    Settings::values.upright_screen_flipped = true;

    const FramebufferLayout layout = EqualWidthFrameLayout(kPanelWidth, kPanelHeight, false, true);

    CheckSpansShortAxis(layout.top_screen);
    CheckSpansShortAxis(layout.bottom_screen);
    CheckCentredOnLongAxis(layout);
    CHECK(Near(layout.bottom_screen.right, layout.top_screen.left));
}

TEST_CASE("Equal-width stack ignores integer scaling", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;
    const FramebufferLayout fractional =
        EqualWidthFrameLayout(kPanelWidth, kPanelHeight, false, true);

    Settings::values.use_integer_scaling = true;
    const FramebufferLayout integer = EqualWidthFrameLayout(kPanelWidth, kPanelHeight, false, true);

    CHECK(integer.top_screen == fractional.top_screen);
    CHECK(integer.bottom_screen == fractional.bottom_screen);
}

TEST_CASE("Original upright stack still uses integer scaling", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;
    Settings::values.use_integer_scaling = true;

    const FramebufferLayout layout = DefaultFrameLayout(kPanelWidth, kPanelHeight, false, true);

    CHECK(layout.top_screen.GetHeight() == 400);
    CHECK(layout.bottom_screen.GetHeight() == 320);
}

TEST_CASE("Equal-width stack keeps the screen gap", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;
    Settings::values.screen_gap = 24;

    const FramebufferLayout layout = EqualWidthFrameLayout(kPanelWidth, kPanelHeight, false, true);

    CheckSpansShortAxis(layout.top_screen);
    CheckSpansShortAxis(layout.bottom_screen);
    CheckCentredOnLongAxis(layout);
    CHECK(layout.bottom_screen.left - layout.top_screen.right >= 42);
}

TEST_CASE("Equal-width stack fits the long axis when the gap is too large",
          "[core][frontend][layout]") {
    LayoutSettingsGuard guard;
    Settings::values.screen_gap = 200;

    const FramebufferLayout layout = EqualWidthFrameLayout(kPanelWidth, kPanelHeight, false, true);

    CHECK(Near(layout.top_screen.left, 0));
    CHECK(Near(layout.bottom_screen.right, kPanelWidth));
    CHECK(layout.top_screen.GetHeight() < kPanelHeight);
    CHECK(Near(layout.top_screen.top, kPanelHeight - layout.top_screen.bottom));
}

TEST_CASE("Equal-width stack only applies to upright screens", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;

    CHECK_FALSE(UseEqualWidthStack(true));

    Settings::values.upright_layout_option = Settings::UprightLayoutOption::EqualWidth;
    CHECK(UseEqualWidthStack(true));
    CHECK_FALSE(UseEqualWidthStack(false));
}

TEST_CASE("Minimum size follows the equal-width stack", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;

    CHECK(GetMinimumSizeFromLayout(Settings::LayoutOption::Default, true) ==
          std::make_pair(480u, 400u));

    Settings::values.upright_layout_option = Settings::UprightLayoutOption::EqualWidth;
    CHECK(GetMinimumSizeFromLayout(Settings::LayoutOption::Default, true) ==
          std::make_pair(540u, 400u));
    CHECK(GetMinimumSizeFromLayout(Settings::LayoutOption::Default, false) ==
          std::make_pair(400u, 480u));

    Settings::values.swap_screen = true;
    CHECK(GetMinimumSizeFromLayout(Settings::LayoutOption::Default, true) ==
          std::make_pair(432u, 320u));
}

TEST_CASE("Resolution-scale layout follows the equal-width stack", "[core][frontend][layout]") {
    LayoutSettingsGuard guard;
    Settings::values.upright_screen = true;
    Settings::values.upright_layout_option = Settings::UprightLayoutOption::EqualWidth;

    const FramebufferLayout layout = FrameLayoutFromResolutionScale(1);
    CHECK(layout.width == 540);
    CHECK(layout.height == 400);
    CHECK((layout.top_screen == Common::Rectangle<u32>{0, 0, 240, 400}));
    CHECK((layout.bottom_screen == Common::Rectangle<u32>{240, 0, 540, 400}));

    Settings::values.swap_screen = true;
    const FramebufferLayout swapped = FrameLayoutFromResolutionScale(1);
    CHECK(swapped.width == 432);
    CHECK(swapped.height == 320);
    CHECK((swapped.bottom_screen == Common::Rectangle<u32>{0, 0, 240, 320}));
    CHECK((swapped.top_screen == Common::Rectangle<u32>{240, 0, 432, 320}));

    Settings::values.swap_screen = false;
    Settings::values.screen_gap = 24;
    const FramebufferLayout gapped = FrameLayoutFromResolutionScale(2);
    CHECK(gapped.width == 540 * 2 + 24 * 2);
    CHECK(gapped.height == 400 * 2);
}

} // namespace Layout
