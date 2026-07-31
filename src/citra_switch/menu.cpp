// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/menu.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/rail_icons.h"
#include "citra_switch/settings_menu.h"
#include "common/horizon_boost.h"

namespace SwitchFrontend {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The panel.
constexpr int kPanelW = 1280;
constexpr int kPanelH = 720;

// The canvas the menu lays itself out on.
int g_screen_w = kPanelW;
int g_screen_h = kPanelH;
int g_rotation = 0; // Degrees clockwise.

bool RotatedUpright() {
    return g_rotation == 90 || g_rotation == 270;
}

constexpr u32 MakeColor(u8 r, u8 g, u8 b, u8 a = 0xFF) {
    return (u32{a} << 24) | (u32{b} << 16) | (u32{g} << 8) | u32{r};
}

constexpr u32 kColBg = MakeColor(0x17, 0x18, 0x1B);
constexpr u32 kColRail = MakeColor(0x1E, 0x20, 0x24);
constexpr u32 kColSurface = MakeColor(0x24, 0x26, 0x2B);
constexpr u32 kColSurfaceHi = MakeColor(0x30, 0x33, 0x39);
constexpr u32 kColBadge = MakeColor(0x3A, 0x3C, 0x42);
constexpr u32 kColAccent = MakeColor(0xFA, 0xAA, 0x49);
constexpr u32 kColAccentDim = MakeColor(0x8C, 0x5F, 0x29);
constexpr u32 kColText = MakeColor(0xF1, 0xF2, 0xF4);
constexpr u32 kColTextDim = MakeColor(0x9B, 0xA0, 0xA6);
constexpr u32 kColOnAccent = MakeColor(0x17, 0x18, 0x1B);
constexpr u32 kColError = MakeColor(0xE0, 0x5A, 0x4A);
constexpr u32 kColHintBar = MakeColor(0x1B, 0x1C, 0x20);

class Canvas {
public:
    Canvas() : pixels(static_cast<std::size_t>(kPanelW) * kPanelH) {}

    u32* Data() {
        return pixels.data();
    }

    int Width() const {
        return width;
    }

    int Height() const {
        return height;
    }

    void Resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<std::size_t>(w) * h, 0);
    }

    void Clear(u32 color) {
        std::fill(pixels.begin(), pixels.end(), color);
    }

    void Blend(int x, int y, u32 color, u8 coverage) {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) {
            return;
        }
        const u32 a = ((color >> 24) & 0xFF) * coverage / 255;
        u32& dst = pixels[static_cast<std::size_t>(y) * width + x];
        if (a == 0) {
            return;
        }
        if (a >= 0xFF) {
            dst = (dst & 0xFF000000u) | (color & 0x00FFFFFFu);
            return;
        }
        const u32 inv = 255 - a;
        const u32 sr = color & 0xFF, sg = (color >> 8) & 0xFF, sb = (color >> 16) & 0xFF;
        const u32 dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF;
        const u32 rr = (sr * a + dr * inv) / 255;
        const u32 rg = (sg * a + dg * inv) / 255;
        const u32 rb = (sb * a + db * inv) / 255;
        dst = MakeColor(static_cast<u8>(rr), static_cast<u8>(rg), static_cast<u8>(rb));
    }

    void FillRect(int x, int y, int w, int h, u32 color) {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(width, x + w), y1 = std::min(height, y + h);
        const u8 alpha = (color >> 24) & 0xFF;
        for (int yy = y0; yy < y1; ++yy) {
            if (alpha >= 0xFF) {
                std::fill_n(pixels.data() + static_cast<std::size_t>(yy) * width + x0, x1 - x0,
                            color);
            } else {
                for (int xx = x0; xx < x1; ++xx) {
                    Blend(xx, yy, color, alpha);
                }
            }
        }
    }

    void FillRoundRect(int x, int y, int w, int h, int r, u32 color) {
        r = std::clamp(r, 0, std::min(w, h) / 2);
        for (int row = 0; row < h; ++row) {
            int cut = 0;
            if (row < r) {
                const int t = r - 1 - row;
                cut = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - t * t)));
            } else if (row >= h - r) {
                const int t = row - (h - r);
                cut = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - t * t)));
            }
            FillRect(x + cut, y + row, w - 2 * cut, 1, color);
        }
    }

    void RoundBorder(int x, int y, int w, int h, int r, int thickness, u32 border, u32 inner) {
        FillRoundRect(x, y, w, h, r, border);
        FillRoundRect(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                      std::max(0, r - thickness), inner);
    }

    void BlitIcon(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size) {
        if (icon.empty() || src_size <= 0) {
            return;
        }
        for (int oy = 0; oy < dst_size; ++oy) {
            const float sy = (oy + 0.5f) * src_size / dst_size - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src_size - 1);
            const int y1 = std::min(y0 + 1, src_size - 1);
            const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
            for (int ox = 0; ox < dst_size; ++ox) {
                const float sx = (ox + 0.5f) * src_size / dst_size - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src_size - 1);
                const int x1 = std::min(x0 + 1, src_size - 1);
                const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
                const u32 c00 = icon[y0 * src_size + x0], c10 = icon[y0 * src_size + x1];
                const u32 c01 = icon[y1 * src_size + x0], c11 = icon[y1 * src_size + x1];
                float ch[3];
                for (int i = 0; i < 3; ++i) {
                    const int s = i * 8;
                    const float top = ((c00 >> s) & 0xFF) * (1 - fx) + ((c10 >> s) & 0xFF) * fx;
                    const float bot = ((c01 >> s) & 0xFF) * (1 - fx) + ((c11 >> s) & 0xFF) * fx;
                    ch[i] = top * (1 - fy) + bot * fy;
                }
                const int px = dx + ox, py = dy + oy;
                if (px >= 0 && py >= 0 && px < width && py < height) {
                    pixels[static_cast<std::size_t>(py) * width + px] = MakeColor(
                        static_cast<u8>(ch[0]), static_cast<u8>(ch[1]), static_cast<u8>(ch[2]));
                }
            }
        }
    }

private:
    std::vector<u32> pixels;
    int width = kPanelW;
    int height = kPanelH;
};

class Font {
public:
    bool Init() {
        if (initialised) {
            return valid;
        }
        initialised = true;
        if (R_FAILED(plInitialize(PlServiceType_User))) {
            return false;
        }
        if (FT_Init_FreeType(&library) != 0) {
            return false;
        }
        AddSharedFace(PlSharedFontType_Standard);
        AddSharedFace(PlSharedFontType_ChineseSimplified);
        AddSharedFace(PlSharedFontType_ExtChineseSimplified);
        AddSharedFace(PlSharedFontType_KO);
        valid = !faces.empty();
        return valid;
    }

    void Shutdown() {
        if (!initialised) {
            return;
        }
        cache.clear();
        for (FT_Face face : faces) {
            FT_Done_Face(face);
        }
        faces.clear();
        if (library) {
            FT_Done_FreeType(library);
            library = nullptr;
        }
        plExit();
        initialised = false;
        valid = false;
    }

    int Draw(Canvas& canvas, int x, int baseline, std::string_view text, int size, u32 color) {
        int pen = x;
        std::size_t i = 0;
        while (i < text.size()) {
            const u32 cp = DecodeUtf8(text, i);
            const Glyph* g = GetGlyph(cp, size);
            if (!g) {
                continue;
            }
            const int gx = pen + g->left;
            const int gy = baseline - g->top;
            for (int row = 0; row < g->h; ++row) {
                for (int col = 0; col < g->w; ++col) {
                    canvas.Blend(gx + col, gy + row, color, g->coverage[row * g->w + col]);
                }
            }
            pen += g->advance;
        }
        return pen - x;
    }

    int Measure(std::string_view text, int size) {
        int w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            const u32 cp = DecodeUtf8(text, i);
            if (const Glyph* g = GetGlyph(cp, size)) {
                w += g->advance;
            }
        }
        return w;
    }

    std::string Truncate(std::string_view text, int size, int maxw) {
        if (Measure(text, size) <= maxw) {
            return std::string{text};
        }
        const int ell = Measure("…", size);
        std::string out;
        int w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t start = i;
            const u32 cp = DecodeUtf8(text, i);
            const Glyph* g = GetGlyph(cp, size);
            const int adv = g ? g->advance : 0;
            if (w + adv + ell > maxw) {
                break;
            }
            out.append(text.substr(start, i - start));
            w += adv;
        }
        out.append("…");
        return out;
    }

    std::string TruncateFront(std::string_view text, int size, int maxw) {
        if (Measure(text, size) <= maxw) {
            return std::string{text};
        }
        const int ell = Measure("…", size);
        std::size_t i = 0;
        while (i < text.size()) {
            DecodeUtf8(text, i);
            const std::string_view tail = text.substr(i);
            if (ell + Measure(tail, size) <= maxw) {
                return "…" + std::string{tail};
            }
        }
        return "…";
    }

private:
    struct Glyph {
        int w{}, h{}, left{}, top{}, advance{};
        std::vector<u8> coverage;
    };

    void AddSharedFace(PlSharedFontType type) {
        PlFontData data{};
        if (R_FAILED(plGetSharedFontByType(&data, type))) {
            return;
        }
        FT_Face face{};
        if (FT_New_Memory_Face(library, static_cast<const FT_Byte*>(data.address),
                               static_cast<FT_Long>(data.size), 0, &face) == 0) {
            faces.push_back(face);
        }
    }

    const Glyph* GetGlyph(u32 cp, int size) {
        const u64 key = (static_cast<u64>(size) << 32) | cp;
        if (auto it = cache.find(key); it != cache.end()) {
            return &it->second;
        }
        FT_Face face = faces.empty() ? nullptr : faces.front();
        for (FT_Face candidate : faces) {
            if (FT_Get_Char_Index(candidate, cp) != 0) {
                face = candidate;
                break;
            }
        }
        if (!face) {
            return nullptr;
        }
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
        // NO_AUTOHINT keeps rendering on the font's native TrueType hinter.
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_AUTOHINT) != 0) {
            return nullptr;
        }
        const FT_GlyphSlot slot = face->glyph;
        Glyph g;
        g.w = static_cast<int>(slot->bitmap.width);
        g.h = static_cast<int>(slot->bitmap.rows);
        g.left = slot->bitmap_left;
        g.top = slot->bitmap_top;
        g.advance = static_cast<int>(slot->advance.x >> 6);
        g.coverage.resize(static_cast<std::size_t>(g.w) * g.h);
        for (int row = 0; row < g.h; ++row) {
            std::memcpy(g.coverage.data() + row * g.w,
                        slot->bitmap.buffer + row * slot->bitmap.pitch, g.w);
        }
        return &cache.emplace(key, std::move(g)).first->second;
    }

    static u32 DecodeUtf8(std::string_view s, std::size_t& i) {
        const u8 c = static_cast<u8>(s[i++]);
        if (c < 0x80) {
            return c;
        }
        int extra = 0;
        u32 cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return '?';
        }
        for (int k = 0; k < extra && i < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<u8>(s[i++]) & 0x3F);
        }
        return cp;
    }

    bool initialised{};
    bool valid{};
    FT_Library library{};
    std::vector<FT_Face> faces;
    std::unordered_map<u64, Glyph> cache;
};

Font g_font;

int CenterBaseline(int y, int h, int size) {
    return y + (h + static_cast<int>(size * 0.7f)) / 2;
}

struct Repeater {
    int held_frames[4]{};

    // Returns a bitmask of directions that should act this frame.
    u32 Step(bool up, bool down, bool left, bool right) {
        const bool active[4] = {up, down, left, right};
        u32 fired = 0;
        for (int d = 0; d < 4; ++d) {
            if (!active[d]) {
                held_frames[d] = 0;
                continue;
            }
            const int f = held_frames[d]++;
            if (f == 0 || (f >= 24 && (f - 24) % 5 == 0)) {
                fired |= 1u << d;
            }
        }
        return fired;
    }
};
enum { DirUp = 1, DirDown = 2, DirLeft = 4, DirRight = 8 };

enum class Tab { Library, Install, Settings, Paths, Artic };

// Which pane the cursor lives in.
enum class Focus { Rail, Content };

// Indexed by Tab, so the order has to match the enum.
constexpr std::array<std::pair<Tab, const char*>, 5> kRailItems{{{Tab::Library, "Library"},
                                                                 {Tab::Install, "Install"},
                                                                 {Tab::Settings, "Settings"},
                                                                 {Tab::Paths, "Paths"},
                                                                 {Tab::Artic, "Artic"}}};

constexpr bool RailItemsMatchTabs() {
    for (int i = 0; i < static_cast<int>(kRailItems.size()); ++i) {
        if (static_cast<int>(kRailItems[i].first) != i) {
            return false;
        }
    }
    return true;
}
static_assert(RailItemsMatchTabs(), "kRailItems must be indexable by Tab");

constexpr int kRailFirstY = 96;
constexpr int kRailItemH = 84;
constexpr int kRailItemStep = 108;

int RailItemTop(int index) {
    return kRailFirstY + index * kRailItemStep;
}

std::optional<Tab> RailHitTest(int y) {
    for (int i = 0; i < static_cast<int>(kRailItems.size()); ++i) {
        const int top = RailItemTop(i);
        if (y >= top && y < top + kRailItemH) {
            return kRailItems[i].first;
        }
    }
    return std::nullopt;
}

constexpr int kRailW = 148;
constexpr int kHeaderH = 64;
constexpr int kHintH = 44;
constexpr int kContentX = kRailW;
constexpr int kContentTop = kHeaderH;
int ContentW() {
    return g_screen_w - kRailW;
}

int ContentBottom() {
    return g_screen_h - kHintH;
}

constexpr int kTileW = 196;
constexpr int kTileH = 170;
constexpr int kTileGap = 16;
constexpr int kIconSize = 96;

std::string g_notice;
int g_notice_frames = 0;
bool g_notice_is_error = true;

// ~4 seconds at 60fps.
constexpr int kNoticeFrames = 240;

void ShowNotice(const std::string& text, bool error) {
    g_notice = text;
    g_notice_frames = kNoticeFrames;
    g_notice_is_error = error;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out{s};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// The Settings page strip, which sits between the header and the rows.
constexpr int kTabStripTop = kContentTop + 10;
constexpr int kTabStripH = 34;
constexpr int kTabPadX = 14;
constexpr int kTabPadMin = 4;
constexpr int kTabGap = 6;

struct TabRect {
    int x{};
    int w{};
};

// Lays the page chips out as one centred row, tightening the padding rather than overflowing the
// content area if the shared font measures wider than the nominal padding allows for.
std::array<TabRect, NumSettingsPages> SettingsTabRects() {
    std::array<TabRect, NumSettingsPages> rects{};
    int text = 0;
    for (int i = 0; i < NumSettingsPages; ++i) {
        rects[i].w = g_font.Measure(SettingsPageName(static_cast<SettingsPage>(i)), 18);
        text += rects[i].w;
    }
    const int available = ContentW() - 48 - (NumSettingsPages - 1) * kTabGap;
    const int pad = std::clamp((available - text) / (2 * NumSettingsPages), kTabPadMin, kTabPadX);

    int total = 0;
    for (int i = 0; i < NumSettingsPages; ++i) {
        rects[i].w += pad * 2;
        total += rects[i].w + (i > 0 ? kTabGap : 0);
    }
    int x = kContentX + std::max(24, (ContentW() - total) / 2);
    for (int i = 0; i < NumSettingsPages; ++i) {
        rects[i].x = x;
        x += rects[i].w + kTabGap;
    }
    return rects;
}

// Where the label sits inside its chip, which follows the same tightening.
int SettingsTabTextInset(const std::array<TabRect, NumSettingsPages>& rects, int index) {
    return (rects[index].w -
            g_font.Measure(SettingsPageName(static_cast<SettingsPage>(index)), 18)) /
           2;
}

bool CompactTabStrip() {
    int total = (NumSettingsPages - 1) * kTabGap + NumSettingsPages * 2 * kTabPadMin;
    for (int i = 0; i < NumSettingsPages; ++i) {
        total += g_font.Measure(SettingsPageName(static_cast<SettingsPage>(i)), 18);
    }
    return total > ContentW() - 48;
}

std::optional<int> SettingsTabHitTest(int x, int y, int active) {
    if (y < kTabStripTop || y >= kTabStripTop + kTabStripH) {
        return std::nullopt;
    }
    if (CompactTabStrip()) {
        const int step = x < kContentX + ContentW() / 2 ? -1 : 1;
        return (active + step + NumSettingsPages) % NumSettingsPages;
    }
    const auto rects = SettingsTabRects();
    for (int i = 0; i < NumSettingsPages; ++i) {
        if (x >= rects[i].x && x < rects[i].x + rects[i].w) {
            return i;
        }
    }
    return std::nullopt;
}

// swkbd prompt for the one text setting.
std::string PromptLogFilter(const std::string& initial) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return initial;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Log filter");
    swkbdConfigSetGuideText(&kbd, "e.g. *:Info Render:Debug");
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 255);
    char out[512] = {};
    const Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? std::string{out} : initial;
}

// Rows on the Paths page.
enum PathRow { PathRowUserDir, PathRowRomsDir, PathRowRecursive, PathRowCount };

constexpr int kPathRowH = 76;
constexpr int kPathToggleH = 52;
constexpr int kPathRowGap = 8;

int PathRowHeight(int row) {
    return row == PathRowRecursive ? kPathToggleH : kPathRowH;
}

int PathRowTop(int row) {
    int y = kContentTop + 16;
    for (int i = 0; i < row; ++i) {
        y += PathRowHeight(i) + kPathRowGap;
    }
    return y;
}

const char* PathRowLabel(int row) {
    switch (row) {
    case PathRowUserDir:
        return "Dekopon Folder";
    case PathRowRomsDir:
        return "ROM Folder";
    default:
        return "Scan Subfolders";
    }
}

enum ArticRow {
    ArticRowAddress,
    ArticRowConnect,
    ArticRowOld3ds,
    ArticRowNew3ds,
    ArticRowController,
    ArticRowCount,
};

// The folder browser covers the whole screen, rail included.
constexpr int kBrowseTop = 108;
constexpr int kBrowseRowH = 44;
int BrowseRows() {
    return std::max(1, (ContentBottom() - kBrowseTop) / kBrowseRowH);
}

void DrawListScrollbar(Canvas& c, int track_x, int top, int visible_rows, int row_h, int count,
                       int scroll) {
    if (count <= visible_rows) {
        return;
    }
    const int track_h = visible_rows * row_h;
    c.FillRoundRect(track_x, top, 4, track_h, 2, kColRail);
    const int thumb_h = std::max(24, track_h * visible_rows / count);
    const int max_scroll = count - visible_rows;
    const int thumb_y = top + (track_h - thumb_h) * scroll / std::max(1, max_scroll);
    c.FillRoundRect(track_x, thumb_y, 4, thumb_h, 2, kColAccent);
}

// Where a hint row starts.
int HintX() {
    return g_screen_w >= kPanelW ? kContentX + 24 : 24;
}

// Draws a small button chip
int DrawHint(Canvas& canvas, int x, int y, const char* button, const char* label) {
    constexpr int chip_h = 26;
    const int letter_w = g_font.Measure(button, 18);
    const int chip_w = std::max(chip_h, letter_w + 16);
    canvas.FillRoundRect(x, y, chip_w, chip_h, chip_h / 2, kColBadge);
    g_font.Draw(canvas, x + (chip_w - letter_w) / 2, CenterBaseline(y, chip_h, 18), button, 18,
                kColText);
    const int label_x = x + chip_w + 8;
    const int label_w = g_font.Draw(canvas, label_x, CenterBaseline(y, chip_h, 18), label, 18,
                                    kColTextDim);
    return chip_w + 8 + label_w;
}

// Indexed by Tab, so the order has to match the enum.
constexpr std::array<const std::uint8_t*, 5> kRailIconMasks{
    RailIcons::kLibrary, RailIcons::kInstall, RailIcons::kSettings, RailIcons::kPaths, nullptr};

// Draws a nav-rail icon, tinted like text: the mask supplies coverage only.
void DrawRailIcon(Canvas& canvas, Tab tab, int cx, int cy, u32 color) {
    const std::uint8_t* mask = kRailIconMasks[static_cast<std::size_t>(tab)];
    if (mask == nullptr) {
        constexpr const char* label = "AB";
        g_font.Draw(canvas, cx - g_font.Measure(label, 18) / 2, cy + 6, label, 18, color);
        return;
    }
    const int x0 = cx - RailIcons::kSize / 2;
    const int y0 = cy - RailIcons::kSize / 2;
    for (int row = 0; row < RailIcons::kSize; ++row) {
        for (int col = 0; col < RailIcons::kSize; ++col) {
            canvas.Blend(x0 + col, y0 + row, color, mask[row * RailIcons::kSize + col]);
        }
    }
}

void DrawRail(Canvas& canvas, Tab active, Tab cursor, bool rail_focused) {
    canvas.FillRect(0, 0, kRailW, g_screen_h, kColRail);
    // The solid accent pill follows the cursor while the rail is focused.
    const Tab pill = rail_focused ? cursor : active;
    for (int i = 0; i < static_cast<int>(kRailItems.size()); ++i) {
        const auto& [tab, label] = kRailItems[i];
        const int y = RailItemTop(i);
        const bool on = tab == pill;
        const bool ghost = rail_focused && tab == active && tab != cursor;
        if (on) {
            canvas.FillRoundRect(12, y, kRailW - 24, kRailItemH, 16, kColAccent);
        } else if (ghost) {
            // Keep a faint marker on the section you came from.
            canvas.FillRoundRect(12, y, kRailW - 24, kRailItemH, 16, kColSurface);
        }
        const u32 fg = on ? kColOnAccent : kColTextDim;
        DrawRailIcon(canvas, tab, kRailW / 2, y + 32, fg);
        const int tw = g_font.Measure(label, 18);
        g_font.Draw(canvas, (kRailW - tw) / 2, y + 68, label, 18, fg);
    }
}

void DrawHeader(Canvas& canvas, std::string_view subtitle) {
    canvas.FillRect(kContentX, 0, ContentW(), kHeaderH, kColBg);
    g_font.Draw(canvas, kContentX + 24, CenterBaseline(0, kHeaderH, 28), "Dekopon", 28, kColText);
    if (!subtitle.empty()) {
        const int sw = g_font.Measure(subtitle, 20);
        g_font.Draw(canvas, g_screen_w - 24 - sw, CenterBaseline(0, kHeaderH, 20), subtitle, 20,
                    kColTextDim);
    }
    canvas.FillRect(kContentX, kHeaderH - 1, ContentW(), 1, kColRail);
}

void DrawNotice(Canvas& canvas) {
    if (g_notice_frames <= 0 || g_notice.empty()) {
        return;
    }
    const int pad = 20;
    const int tw = g_font.Measure(g_notice, 18);
    const int w = tw + pad * 2;
    const int x = kContentX + (ContentW() - w) / 2;
    const int y = ContentBottom() - 52;
    canvas.FillRoundRect(x, y, w, 36, 10, g_notice_is_error ? kColError : kColAccentDim);
    g_font.Draw(canvas, x + pad, CenterBaseline(y, 36, 18), g_notice, 18, kColText);
}

void DrawTile(Canvas& canvas, const GameEntry& game, int x, int y, bool selected,
              bool content_focused) {
    if (selected && content_focused) {
        canvas.RoundBorder(x, y, kTileW, kTileH, 14, 3, kColAccent, kColSurfaceHi);
    } else if (selected) {
        // Selected but the cursor is on the rail
        canvas.RoundBorder(x, y, kTileW, kTileH, 14, 3, kColBadge, kColSurfaceHi);
    } else {
        canvas.FillRoundRect(x, y, kTileW, kTileH, 14, kColSurface);
    }

    const int icon_x = x + (kTileW - kIconSize) / 2;
    const int icon_y = y + 14;
    if (!game.icon.empty()) {
        canvas.BlitIcon(game.icon, game.icon_size, icon_x, icon_y, kIconSize);
    } else {
        // Placeholder plate with the file type initial.
        canvas.FillRoundRect(icon_x, icon_y, kIconSize, kIconSize, 10, kColBadge);
        const std::string letter = game.file_type.empty() ? "?" : game.file_type.substr(0, 1);
        const int lw = g_font.Measure(letter, 40);
        g_font.Draw(canvas, icon_x + (kIconSize - lw) / 2, CenterBaseline(icon_y, kIconSize, 40),
                    letter, 40, kColTextDim);
    }

    const int text_w = kTileW - 24;
    const std::string title = g_font.Truncate(game.title, 18, text_w);
    const int title_w = g_font.Measure(title, 18);
    g_font.Draw(canvas, x + (kTileW - title_w) / 2, icon_y + kIconSize + 24, title, 18, kColText);

    // File type, then a LOCKED marker for encrypted dumps and an SD marker for installed titles.
    const int badge_y = y + kTileH - 30;
    int badge_x = x + 12;
    if (!game.file_type.empty()) {
        const int bw = g_font.Measure(game.file_type, 14) + 14;
        canvas.FillRoundRect(badge_x, badge_y, bw, 20, 7, kColBadge);
        g_font.Draw(canvas, badge_x + 7, CenterBaseline(badge_y, 20, 14), game.file_type, 14,
                    kColTextDim);
        badge_x += bw + 6;
    }
    if (game.encrypted) {
        const int lw = g_font.Measure("LOCKED", 14) + 14;
        canvas.FillRoundRect(badge_x, badge_y, lw, 20, 7, kColAccentDim);
        g_font.Draw(canvas, badge_x + 7, CenterBaseline(badge_y, 20, 14), "LOCKED", 14, kColText);
        badge_x += lw + 6;
    }
    if (game.insertable && GetInsertedCartridge() == game.path) {
        const int lw = g_font.Measure("CART", 14) + 14;
        canvas.FillRoundRect(badge_x, badge_y, lw, 20, 7, kColAccentDim);
        g_font.Draw(canvas, badge_x + 7, CenterBaseline(badge_y, 20, 14), "CART", 14, kColText);
        badge_x += lw + 6;
    }
    if (game.installed) {
        const int lw = g_font.Measure("SD", 14) + 14;
        canvas.FillRoundRect(badge_x, badge_y, lw, 20, 7, kColAccentDim);
        g_font.Draw(canvas, badge_x + 7, CenterBaseline(badge_y, 20, 14), "SD", 14, kColText);
    }
}

void DrawEmptyLibrary(Canvas& canvas, const std::string& roms_dir) {
    const char* line1 = "No games found";
    const char* line2 = "Copy .3ds / .cci / .cxi / .3dsx files to";
    const std::string line3 = g_font.TruncateFront(roms_dir, 18, ContentW() - 48);
    const char* line4 = "or install a .cia from the Install tab";
    const int cx = kContentX + ContentW() / 2;
    const int top = (kContentTop + ContentBottom()) / 2 - 70;
    const int w1 = g_font.Measure(line1, 26);
    g_font.Draw(canvas, cx - w1 / 2, top, line1, 26, kColText);
    const int w2 = g_font.Measure(line2, 18);
    g_font.Draw(canvas, cx - w2 / 2, top + 36, line2, 18, kColTextDim);
    const int w3 = g_font.Measure(line3, 18);
    g_font.Draw(canvas, cx - w3 / 2, top + 60, line3, 18, kColAccent);
    const int w4 = g_font.Measure(line4, 18);
    g_font.Draw(canvas, cx - w4 / 2, top + 90, line4, 18, kColTextDim);
}

// Layout of the library grid
struct Grid {
    int cols{};
    int start_x{};
    int top{};
    int visible_rows{};
};

Grid ComputeGrid() {
    Grid g;
    const int avail = ContentW() - 48;
    g.cols = std::max(1, (avail + kTileGap) / (kTileW + kTileGap));
    const int used = g.cols * kTileW + (g.cols - 1) * kTileGap;
    g.start_x = kContentX + 24 + (avail - used) / 2;
    g.top = kContentTop + 20;
    g.visible_rows = std::max(1, (ContentBottom() - g.top) / (kTileH + kTileGap));
    return g;
}

// swkbd search prompt
std::string PromptSearch(const std::string& initial) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return initial;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Search library");
    swkbdConfigSetGuideText(&kbd, "Game title");
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 128);
    char out[256] = {};
    const Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? std::string{out} : initial;
}

std::optional<std::string> PromptArticAddress(const std::string& initial) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return std::nullopt;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Artic server address");
    swkbdConfigSetGuideText(&kbd, "3DS IP address :port");
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 255);
    char out[512] = {};
    const Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) {
        return std::nullopt;
    }
    std::string address{out};
    const auto first = std::find_if_not(address.begin(), address.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(address.rbegin(), address.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    return first < last ? std::string(first, last) : std::string{};
}

bool IsValidArticAddress(const std::string& address) {
    if (address.empty() || address.find('/') != std::string::npos ||
        std::any_of(address.begin(), address.end(),
                    [](unsigned char c) { return std::isspace(c) != 0; })) {
        return false;
    }
    const std::size_t colon = address.find(':');
    const std::string host = address.substr(0, colon);
    if (host.empty() ||
        !std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '.' || c == '-' || c == '_';
        })) {
        return false;
    }
    if (colon == std::string::npos) {
        return true;
    }
    if (colon != address.rfind(':') || colon + 1 == address.size()) {
        return false;
    }
    const std::string port = address.substr(colon + 1);
    if (!std::all_of(port.begin(), port.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return false;
    }
    const unsigned long value = std::strtoul(port.c_str(), nullptr, 10);
    return value > 0 && value <= 0xFFFF;
}

std::string FormatTitleId(u64 program_id) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(program_id));
    return buf;
}

// Updates and DLC ride on a base title rather than standing alone, so they get the accent.
u32 KindBadgeColor(TitleKind kind) {
    switch (kind) {
    case TitleKind::Update:
    case TitleKind::AddOnContent:
        return kColAccentDim;
    default:
        return kColBadge;
    }
}

// The Install page lists the CIAs in one folder.
constexpr int kInstallHeaderH = 40;
constexpr int kInstallTop = kContentTop + kInstallHeaderH + 8;
constexpr int kInstallRowH = 46;
int InstallRows() {
    return std::max(1, (ContentBottom() - kInstallTop) / kInstallRowH);
}

// Modal panel listing what is installed alongside one library entry.
void DrawTitleDetails(Canvas& c, const GameEntry& game, const TitleDetails& details) {
    const int w = std::min(660, ContentW() - 48);
    constexpr int h = 390;
    const int x = kContentX + (ContentW() - w) / 2;
    const int y = kContentTop + (ContentBottom() - kContentTop - h) / 2;
    c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
    c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

    int ty = y + 22;
    g_font.Draw(c, x + 24, ty + 20, g_font.Truncate(game.title, 24, w - 48), 24, kColText);
    ty += 32;
    if (!game.publisher.empty()) {
        g_font.Draw(c, x + 24, ty + 18, g_font.Truncate(game.publisher, 18, w - 48), 18,
                    kColTextDim);
    }
    ty += 30;
    c.FillRect(x + 24, ty, w - 48, 1, kColRail);
    ty += 12;

    const auto row = [&](const char* label, const std::string& value, u32 color) {
        g_font.Draw(c, x + 24, ty + 18, label, 18, kColTextDim);
        g_font.Draw(c, x + 190, ty + 18, g_font.Truncate(value, 18, w - 214), 18, color);
        ty += 30;
    };

    row("Title ID",
        details.program_id == 0 ? std::string{"Unknown"} : FormatTitleId(details.program_id),
        kColText);
    row("Type", TitleKindName(details.kind), kColText);
    row("Source",
        game.installed ? "Installed on SD (" + game.file_type + ")"
                       : "ROM file (" + game.file_type + ")",
        kColText);
    row("Version",
        details.has_base_version ? FormatTitleVersion(details.base_version)
                                 : std::string{"Unknown (no TMD)"},
        details.has_base_version ? kColAccent : kColTextDim);
    row("Update",
        details.has_update ? FormatTitleVersion(details.update_version)
                           : std::string{"Not installed"},
        details.has_update ? kColAccent : kColTextDim);
    row("DLC",
        details.has_dlc ? std::to_string(details.dlc_contents) +
                              (details.dlc_contents == 1 ? " content" : " contents")
                        : std::string{"Not installed"},
        details.has_dlc ? kColAccent : kColTextDim);
    const bool inserted = game.insertable && GetInsertedCartridge() == game.path;
    if (game.insertable) {
        row("Cartridge", inserted ? "Inserted" : "Not inserted",
            inserted ? kColAccent : kColTextDim);
    }

    ty += 4;
    g_font.Draw(c, x + 24, ty + 16, g_font.TruncateFront(game.path, 16, w - 48), 16, kColTextDim);

    int hx = x + 24;
    const int hy = y + h - 38;
    if (game.insertable) {
        hx += DrawHint(c, hx, hy, "X", inserted ? "Eject Cartridge" : "Insert Cartridge") + 22;
    }
    DrawHint(c, hx, hy, "B", "Close");
}

class Menu {
public:
    MenuResult Run(PadState& pad) {
        pad_state = &pad;
        // Put a frame up before the ROM scan
        EnsureFramebuffer();
        ApplyRotation();
        DrawLoading();
        Present();
        Rescan();
        while (appletMainLoop()) {
            padUpdate(&pad);
            ApplyRotation();
            const u64 down = padGetButtonsDown(&pad);
            held = padGetButtons(&pad);

            // +/- together exits the app, but will not allow during an install.
            if (!install_active && (held & (HidNpadButton_Plus | HidNpadButton_Minus)) ==
                                       (HidNpadButton_Plus | HidNpadButton_Minus)) {
                if (remap_open) {
                    CloseRemap();
                }
                Flush();
                return {MenuAction::Exit, {}};
            }

            const HidAnalogStickState ls = padGetStickPos(&pad, 0);
            constexpr int dz = 12000;
            const MenuDirections dirs = RotateMenuDirections({
                .up = (down & HidNpadButton_Up) != 0 || ls.y > dz,
                .down = (down & HidNpadButton_Down) != 0 || ls.y < -dz,
                .left = (down & HidNpadButton_Left) != 0 || ls.x < -dz,
                .right = (down & HidNpadButton_Right) != 0 || ls.x > dz,
            });
            const u32 nav = repeater.Step(dirs.up, dirs.down, dirs.left, dirs.right);

            MenuResult result;
            bool done = false;
            if (install_active) {
                PumpInstall();
            } else if (confirm) {
                HandleConfirm(down);
            } else if (preset_picker_open) {
                HandlePresetPicker(down, nav);
            } else if (layout_picker_open) {
                HandleLayoutPicker(down, nav);
            } else if (remap_open) {
                HandleRemap(down, nav);
            } else if (details_open) {
                const GameEntry& game = games[filtered[selected]];
                if ((down & HidNpadButton_X) && game.insertable) {
                    SetInsertedCartridge(GetInsertedCartridge() == game.path ? "" : game.path);
                }
                if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) {
                    details_open = false;
                }
            } else if (focus == Focus::Rail) {
                HandleRail(down, nav);
            } else if (tab == Tab::Library) {
                done = HandleLibrary(down, nav, result);
            } else if (tab == Tab::Install) {
                HandleInstall(down, nav);
            } else if (tab == Tab::Settings) {
                done = HandleSettings(down, nav);
            } else if (tab == Tab::Paths) {
                done = HandlePaths(down, nav);
            } else {
                done = HandleArtic(down, nav, result);
            }
            if (done) {
                return result;
            }

            if (!install_active && !details_open && !layout_picker_open && !remap_open &&
                !preset_picker_open && !confirm) {
                HandleTouch();
            }
            if (pending_launch) {
                MenuResult launch{MenuAction::Launch, *pending_launch};
                pending_launch.reset();
                return launch;
            }

            Draw();
            Present();
            if (g_notice_frames > 0) {
                --g_notice_frames;
            }
        }
        if (remap_open) {
            CloseRemap();
        }
        Flush();
        return {MenuAction::Exit, {}};
    }

    // Releasing the framebuffer hands the nwindow back so the emulator's renderer can claim it for the launched game.
    ~Menu() {
        // Only reachable with a worker still running if appletMainLoop() bowed out mid-install.
        if (install_thread.joinable()) {
            install_thread.join();
        }
        if (fb_ready) {
            framebufferClose(&fb);
        }
    }

private:
    Tab tab{Tab::Library};
    Focus focus{Focus::Content};
    Tab rail_sel{Tab::Library}; // Highlighted rail item while focused.
    std::vector<GameEntry> games;
    std::vector<int> filtered; // Indices into `games` after search filtering.
    int selected = 0;          // Index into `filtered`.
    int scroll_row = 0;
    int paths_sel = 0;
    int artic_sel = 0;
    std::string search;
    SwitchPaths paths{};
    SystemFileSetupState artic_install_state{};
    bool artic_state_loaded = false;

    // Settings tab.
    SettingsPage settings_page{SettingsPage::General};
    std::vector<SettingsRow> settings_rows;
    // Kept per page so switching back lands where the cursor was left.
    std::array<int, NumSettingsPages> settings_sel{};
    std::array<int, NumSettingsPages> settings_scroll{};

    Repeater repeater;
    Framebuffer fb{};
    PadState* pad_state = nullptr;
    u64 held = 0; // This frame's held buttons.
    bool fb_ready = false;
    bool settings_dirty = false; // Edited settings not yet written to config.ini.
    bool paths_dirty = false;

    // Library detail panel.
    bool details_open = false;
    TitleDetails details{};

    // R3 screen-layout picker.
    bool layout_picker_open = false;
    int layout_picker_sel = 0;

    // Preset picker the reset row opens.
    bool preset_picker_open = false;
    int preset_sel = 0;

    // Controller remapping page.
    bool remap_open = false;
    int remap_sel = 0;
    int remap_scroll = 0;

    // Confirmation for the settings rows that destroy something.
    struct ConfirmPrompt {
        std::string title;
        std::vector<std::string> lines;
        std::string note;
        const char* accept;
        std::function<void()> on_accept;
    };
    std::optional<ConfirmPrompt> confirm;

    // Install page.
    std::string install_dir;
    std::vector<DirEntry> install_dirs;
    std::vector<CiaEntry> install_cias;
    int install_sel = 0;
    int install_scroll = 0;
    bool install_listed = false; // Whether install_dir has been read at least once.

    // The installation worker.
    std::thread install_thread;
    std::atomic<bool> install_done{false};
    std::atomic<std::size_t> install_written{0};
    std::atomic<std::size_t> install_total{0};
    InstallResult install_result{};
    std::string install_name;
    bool install_active = false;

    void Rescan() {
        games = ScanGames();
        paths = GetPaths();
        if (install_dir.empty()) {
            install_dir = paths.roms_dir;
        }
        ApplyFilter();
    }

    // Switch tabs persisting any pending edits when leaving an editing page so a disk write
    // happens once per editing session rather than once per adjustment.
    void SetTab(Tab next) {
        if (tab == Tab::Settings && next != Tab::Settings) {
            FlushSettings();
        }
        if (tab == Tab::Paths && next != Tab::Paths) {
            // The library on screen came from the old directory so it must be re-read.
            const bool stale = ScanInputsChanged();
            FlushPaths();
            if (stale) {
                ShowBusy("Refreshing library...");
                search.clear();
                Rescan();
            }
        }
        tab = next;
        if (tab == Tab::Install && !install_listed) {
            ShowBusy("Reading CIAs...");
            RefreshInstallList();
        }
        if (tab == Tab::Settings) {
            SetSettingsPage(settings_page);
        }
        if (tab == Tab::Artic && !artic_state_loaded) {
            ShowBusy("Checking system files...");
            artic_install_state = GetSystemFileSetupState();
            artic_state_loaded = true;
        }
    }

    void SetSettingsPage(SettingsPage page) {
        settings_page = page;
        settings_rows = BuildSettingsPage(page);
        const int last = std::max(0, static_cast<int>(settings_rows.size()) - 1);
        SettingsSel() = std::clamp(SettingsSel(), 0, last);
        ScrollSettingsIntoView();
    }

    int& SettingsSel() {
        return settings_sel[static_cast<std::size_t>(settings_page)];
    }

    int& SettingsScroll() {
        return settings_scroll[static_cast<std::size_t>(settings_page)];
    }

    // True while the edited scan inputs differ from what the last scan used.
    bool ScanInputsChanged() const {
        const SwitchPaths& live = GetPaths();
        return paths.roms_dir != live.roms_dir || paths.scan_recursive != live.scan_recursive;
    }

    // The dekopon directory only moves on the next launch.
    bool RestartPending() const {
        return paths.user_dir != GetActiveUserDir();
    }

    void Flush() {
        FlushSettings();
        FlushPaths();
    }

    void FlushSettings() {
        if (settings_dirty) {
            CommitSettings();
            settings_dirty = false;
        }
    }

    void FlushPaths() {
        if (paths_dirty) {
            SetPaths(paths);
            paths_dirty = false;
        }
    }

    void ApplyFilter() {
        filtered.clear();
        const std::string needle = ToLowerAscii(search);
        for (int i = 0; i < static_cast<int>(games.size()); ++i) {
            if (needle.empty() || ToLowerAscii(games[i].title).find(needle) != std::string::npos) {
                filtered.push_back(i);
            }
        }
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(filtered.size()) - 1));
        scroll_row = 0;
    }

    // Returns true if the menu should return `result` to the caller.
    bool HandleLibrary(u64 down, u32 nav, MenuResult& result) {
        const Grid grid = ComputeGrid();
        const int count = static_cast<int>(filtered.size());
        if (count > 0) {
            if (nav & DirLeft) {
                selected = std::max(0, selected - 1);
            }
            if (nav & DirRight) {
                selected = std::min(count - 1, selected + 1);
            }
            if (nav & DirUp) {
                selected = std::max(0, selected - grid.cols);
            }
            if (nav & DirDown) {
                selected = std::min(count - 1, selected + grid.cols);
            }
            if (down & HidNpadButton_A) {
                result = {MenuAction::Launch, games[filtered[selected]].path};
                return true;
            }
            // Guarded so that reaching for the +/- exit combo doesn't flash the panel open.
            if ((down & HidNpadButton_Plus) && !(held & HidNpadButton_Minus)) {
                details = GetTitleDetails(games[filtered[selected]]);
                details_open = true;
            }
        }
        if (down & HidNpadButton_X) {
            search = PromptSearch(search);
            ApplyFilter();
        }
        if (down & HidNpadButton_Y) {
            // Rescanning the SD card blocks
            ShowBusy("Refreshing library…");
            search.clear();
            Rescan();
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        EnsureVisible(grid);
        return false;
    }

    // Row model for the Install page: ".." (unless at a device root), then subfolders, then CIAs.
    int InstallParentRows() const {
        return ParentDirectory(install_dir).empty() ? 0 : 1;
    }

    int InstallRowCount() const {
        return InstallParentRows() + static_cast<int>(install_dirs.size()) +
               static_cast<int>(install_cias.size());
    }

    // The CIA under the cursor, or nothing when it sits on ".." or a folder.
    const CiaEntry* SelectedCia() const {
        const int i = install_sel - InstallParentRows() - static_cast<int>(install_dirs.size());
        if (i < 0 || i >= static_cast<int>(install_cias.size())) {
            return nullptr;
        }
        return &install_cias[i];
    }

    void RefreshInstallList() {
        install_dirs = ListSubdirectories(install_dir);
        install_cias = ListCiaFiles(install_dir);
        install_sel = std::clamp(install_sel, 0, std::max(0, InstallRowCount() - 1));
        install_listed = true;
    }

    void EnterInstallDir(const std::string& next) {
        install_dir = next;
        install_sel = 0;
        install_scroll = 0;
        RefreshInstallList();
    }

    void HandleInstall(u64 down, u32 nav) {
        const int count = InstallRowCount();
        install_sel = std::clamp(install_sel, 0, std::max(0, count - 1));
        if (nav & DirUp) {
            install_sel = std::max(0, install_sel - 1);
        }
        if (nav & DirDown) {
            install_sel = std::min(std::max(0, count - 1), install_sel + 1);
        }
        install_scroll = std::clamp(install_scroll, std::max(0, install_sel - InstallRows() + 1),
                                    std::max(0, std::min(install_sel, count - InstallRows())));
        if (down & HidNpadButton_A) {
            const int base = InstallParentRows();
            const int di = install_sel - base;
            if (base == 1 && install_sel == 0) {
                EnterInstallDir(ParentDirectory(install_dir));
            } else if (di < static_cast<int>(install_dirs.size())) {
                EnterInstallDir(install_dirs[di].path);
            } else if (const CiaEntry* cia = SelectedCia()) {
                TryStartInstall(*cia);
            }
        }
        if (down & HidNpadButton_Y) {
            ShowBusy("Reading CIAs...");
            RefreshInstallList();
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
    }

    void TryStartInstall(const CiaEntry& cia) {
        if (!cia.readable) {
            ShowNotice(cia.name + ": not a valid CIA", true);
            return;
        }
        if (ConfirmInstall(cia)) {
            StartInstall(cia);
        }
    }

    void StartInstall(const CiaEntry& cia) {
        install_name = cia.name;
        install_written = 0;
        install_total = cia.size;
        install_done = false;
        install_active = true;
        install_thread = std::thread([this, path = cia.path] {
            const Common::Horizon::CpuBoostScope boost;
            install_result = InstallCia(path, [this](std::size_t written, std::size_t total) {
                install_written = written;
                install_total = total;
            });
            install_done = true;
        });
    }

    void PumpInstall() {
        if (!install_done) {
            return;
        }
        install_thread.join();
        install_active = false;
        const bool ok = install_result == InstallResult::Success;
        ShowNotice(install_name + ": " + InstallResultText(install_result), !ok);
        RefreshInstallList();
        if (ok) {
            // A successful install may have put a new title in the library's reach.
            games = ScanGames();
            ApplyFilter();
        }
    }

    // Blocks on a yes/no prompt.
    bool ConfirmInstall(const CiaEntry& cia) {
        u16 installed_version = 0;
        const bool replacing = GetInstalledVersion(cia.program_id, installed_version);
        while (appletMainLoop()) {
            padUpdate(pad_state);
            const u64 down = padGetButtonsDown(pad_state);
            if (down & HidNpadButton_A) {
                return true;
            }
            if (down & HidNpadButton_B) {
                return false;
            }
            Draw();
            DrawConfirmInstall(canvas, cia, replacing, installed_version);
            Present();
        }
        return false;
    }

    // Move the cursor out to the Library/Settings rail
    void EnterRail() {
        focus = Focus::Rail;
        rail_sel = tab;
    }

    void HandleRail(u64 down, u32 nav) {
        int index = static_cast<int>(rail_sel);
        if (nav & DirUp) {
            index = std::max(0, index - 1);
        }
        if (nav & DirDown) {
            index = std::min(static_cast<int>(kRailItems.size()) - 1, index + 1);
        }
        rail_sel = kRailItems[index].first;
        if (down & HidNpadButton_A) {
            SetTab(rail_sel);
            focus = Focus::Content;
        }
        // B cancels back into the section left.
        if (down & HidNpadButton_B) {
            rail_sel = tab;
            focus = Focus::Content;
        }
    }

    // Keeps the selected settings row inside the visible window.
    void ScrollSettingsIntoView() {
        int& scroll = SettingsScroll();
        const int sel = SettingsSel();
        scroll = std::clamp(scroll, std::max(0, sel - SettingsVisibleRows() + 1), sel);
    }

    void OpenLayoutPicker() {
        layout_picker_sel = 0;
        layout_picker_open = true;
    }

    void StepSettingsPage(int dir) {
        SetSettingsPage(static_cast<SettingsPage>(
            (static_cast<int>(settings_page) + dir + NumSettingsPages) % NumSettingsPages));
    }

    void OpenSettingsModal(SettingsModal modal) {
        switch (modal) {
        case SettingsModal::LayoutCycle:
            OpenLayoutPicker();
            break;
        case SettingsModal::ControllerMap:
            OpenRemap();
            break;
        case SettingsModal::LogFilter:
            SetLogFilter(PromptLogFilter(GetLogFilter()));
            settings_dirty = true;
            break;
        case SettingsModal::ResetDefaults:
            OpenPresetPicker();
            break;
        case SettingsModal::ClearShaderCache:
            OpenShaderCacheConfirm();
            break;
        default:
            break;
        }
    }

    bool HandleSettings(u64 down, u32 nav) {
        if (down & HidNpadButton_L) {
            StepSettingsPage(-1);
        }
        if (down & HidNpadButton_R) {
            StepSettingsPage(+1);
        }
        if (settings_rows.empty()) {
            if (down & HidNpadButton_B) {
                EnterRail();
            }
            return false;
        }

        const int count = static_cast<int>(settings_rows.size());
        int& sel = SettingsSel();
        if (nav & DirUp) {
            sel = std::max(0, sel - 1);
        }
        if (nav & DirDown) {
            sel = std::min(count - 1, sel + 1);
        }
        ScrollSettingsIntoView();

        const SettingsRow& row = settings_rows[sel];
        if (row.modal != SettingsModal::None) {
            if ((down & HidNpadButton_A) || (nav & DirRight)) {
                OpenSettingsModal(row.modal);
            }
            if (down & HidNpadButton_B) {
                EnterRail();
            }
            return false;
        }

        // Settings::values is edited live. FlushSettings() only batches the config.ini write.
        if (nav & DirLeft) {
            row.step(-1);
            settings_dirty = true;
        }
        if ((nav & DirRight) || (down & HidNpadButton_A)) {
            row.step(+1);
            settings_dirty = true;
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        return false;
    }

    void HandleLayoutPicker(u64 down, u32 nav) {
        const int count = GetScreenLayoutCount();
        if (nav & DirUp) {
            layout_picker_sel = (layout_picker_sel - 1 + count) % count;
        }
        if (nav & DirDown) {
            layout_picker_sel = (layout_picker_sel + 1) % count;
        }
        if (down & HidNpadButton_A) {
            SetLayoutCycleMask(GetLayoutCycleMask() ^ (1u << layout_picker_sel));
            settings_dirty = true;
        }
        if (down & HidNpadButton_B) {
            layout_picker_open = false;
        }
    }

    void OpenPresetPicker() {
        preset_sel = 0;
        preset_picker_open = true;
    }

    void HandlePresetPicker(u64 down, u32 nav) {
        if (nav & DirUp) {
            preset_sel = (preset_sel - 1 + NumSettingsPresets) % NumSettingsPresets;
        }
        if (nav & DirDown) {
            preset_sel = (preset_sel + 1) % NumSettingsPresets;
        }
        if (down & HidNpadButton_A) {
            OpenResetConfirm(static_cast<SettingsPreset>(preset_sel));
        }
        if (down & HidNpadButton_B) {
            preset_picker_open = false;
        }
    }

    void OpenResetConfirm(SettingsPreset preset) {
        const std::string name = SettingsPresetName(preset);
        const bool risky = preset != SettingsPreset::Default;
        confirm = ConfirmPrompt{
            "Reset to " + name + " settings?",
            risky ? std::vector<std::string>{"Every setting is reset, then the " + name,
                                             "selected preset is applied. Mappings are reset too."}
                  : std::vector<std::string>{"Every setting returns to its default for this",
                                             "version, with controller mappings included."},
            risky ? "This preset can break some games."
                  : "Your folders, titles, and saves are untouched.",
            "Reset",
            [this, preset] {
                ResetSettings(preset);
                // ResetSettings() has already written the new values out.
                settings_dirty = false;
                preset_picker_open = false;
                ShowNotice("Settings reset to " + std::string{SettingsPresetName(preset)}, false);
            }};
    }

    void OpenShaderCacheConfirm() {
        confirm = ConfirmPrompt{
            "Clear the shader cache?",
            {"Every game's compiled shaders and pipelines will be deleted.",
             "Expect stutter on your next run of a game."},
            "Post-processing shaders are untouched.",
            "Clear",
            [this] {
                const u64 freed = ClearShaderCache();
                SetSettingsPage(settings_page);
                ShowNotice("Shader cache cleared, " + FormatSize(freed) + " freed", false);
            }};
    }

    void HandleConfirm(u64 down) {
        if (down & HidNpadButton_A) {
            // The action outlives the prompt it came from, so it is moved out before closing.
            const std::function<void()> action = std::move(confirm->on_accept);
            confirm.reset();
            action();
            return;
        }
        if (down & HidNpadButton_B) {
            confirm.reset();
        }
    }

    void OpenRemap() {
        remap_sel = 0;
        remap_scroll = 0;
        remap_open = true;
    }

    void CloseRemap() {
        remap_open = false;
        // Rebuild the guest input profile and persist.
        ApplyButtonMappings();
        SaveConfig();
    }

    void ScrollRemapIntoView() {
        remap_scroll =
            std::clamp(remap_scroll, std::max(0, remap_sel - RemapVisibleRows() + 1), remap_sel);
    }

    // Cycles the physical Switch button bound to `control` by `dir`.
    void StepRemapMapping(MappableControl control, int dir) {
        const int cur = static_cast<int>(GetMapping(control));
        const int next = (cur + dir + NumBindingChoices) % NumBindingChoices;
        SetMapping(control, static_cast<InputButton>(next));
    }

    void HandleRemap(u64 down, u32 nav) {
        if (nav & DirUp) {
            remap_sel = (remap_sel - 1 + NumMappableControls) % NumMappableControls;
        }
        if (nav & DirDown) {
            remap_sel = (remap_sel + 1) % NumMappableControls;
        }
        ScrollRemapIntoView();

        const auto control = static_cast<MappableControl>(remap_sel);
        if (nav & DirLeft) {
            StepRemapMapping(control, -1);
        }
        if ((nav & DirRight) || (down & HidNpadButton_A)) {
            StepRemapMapping(control, +1);
        }
        if (down & HidNpadButton_X) {
            SetMapping(control, InputButton::None);
        }
        if (down & HidNpadButton_Y) {
            SetMapping(control, DefaultMapping(control));
        }
        if (down & HidNpadButton_B) {
            CloseRemap();
        }
    }

    bool HandlePaths(u64 down, u32 nav) {
        if (nav & DirUp) {
            paths_sel = std::max(0, paths_sel - 1);
        }
        if (nav & DirDown) {
            paths_sel = std::min(PathRowCount - 1, paths_sel + 1);
        }
        if (paths_sel == PathRowRecursive) {
            if (nav & DirLeft) {
                paths.scan_recursive = false;
                paths_dirty = true;
            }
            if (nav & DirRight) {
                paths.scan_recursive = true;
                paths_dirty = true;
            }
            if (down & HidNpadButton_A) {
                paths.scan_recursive = !paths.scan_recursive;
                paths_dirty = true;
            }
        } else if (down & HidNpadButton_A) {
            PickFolder(paths_sel);
        }
        if (down & HidNpadButton_Y) {
            ResetToDefault(paths_sel);
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        return false;
    }

    bool EditArticAddress() {
        const std::optional<std::string> address = PromptArticAddress(GetArticBaseAddress());
        if (!address) {
            return false;
        }
        if (!IsValidArticAddress(*address)) {
            ShowNotice("Enter the 3DS' IP address followed by :port", true);
            return false;
        }
        SetArticBaseAddress(*address);
        return true;
    }

    bool EnsureArticAddress() {
        if (IsValidArticAddress(GetArticBaseAddress())) {
            return true;
        }
        return EditArticAddress();
    }

    bool ConfirmArticSetup(bool old3ds, bool replacing) {
        while (appletMainLoop()) {
            padUpdate(pad_state);
            const u64 down = padGetButtonsDown(pad_state);
            if (down & HidNpadButton_A) {
                return true;
            }
            if (down & HidNpadButton_B) {
                return false;
            }
            Draw();
            DrawArticSetupConfirm(canvas, old3ds, replacing);
            Present();
        }
        return false;
    }

    bool ActivateArtic(MenuResult& result) {
        if (artic_sel == ArticRowAddress) {
            EditArticAddress();
            return false;
        }
        if (artic_sel == ArticRowController) {
            SetUseArticBaseController(!GetUseArticBaseController());
            return false;
        }
        const bool old3ds = artic_sel == ArticRowOld3ds;
        if (artic_sel == ArticRowNew3ds && !artic_install_state.old3ds) {
            ShowNotice("Old 3DS system files must be set up first", true);
            return false;
        }
        if (!EnsureArticAddress()) {
            return false;
        }
        if (artic_sel == ArticRowConnect) {
            result = {MenuAction::Launch, "articbase://" + GetArticBaseAddress()};
            return true;
        }

        const bool replacing =
            old3ds ? artic_install_state.old3ds : artic_install_state.new3ds;
        if (!ConfirmArticSetup(old3ds, replacing)) {
            return false;
        }

        ShowBusy("Preparing system-file setup...");
        PrepareSystemFileSetup(old3ds ? SystemFileSetupMode::Old3ds
                                     : SystemFileSetupMode::New3ds);
        result = {MenuAction::Launch,
                  std::string{old3ds ? "articinio://" : "articinin://"} +
                      GetArticBaseAddress()};
        return true;
    }

    bool HandleArtic(u64 down, u32 nav, MenuResult& result) {
        if (nav & DirUp) {
            artic_sel = std::max(0, artic_sel - 1);
        }
        if (nav & DirDown) {
            artic_sel = std::min(ArticRowCount - 1, artic_sel + 1);
        }
        if (down & HidNpadButton_A) {
            return ActivateArtic(result);
        }
        if (down & HidNpadButton_B) {
            EnterRail();
        }
        return false;
    }

    void PickFolder(int row) {
        const std::string& current = row == PathRowUserDir ? paths.user_dir : paths.roms_dir;
        const std::optional<std::string> picked = BrowseForFolder(current);
        if (!picked) {
            return;
        }
        if (row == PathRowUserDir) {
            paths.user_dir = *picked;
        } else {
            paths.roms_dir = *picked;
        }
        paths_dirty = true;
    }

    void ResetToDefault(int row) {
        switch (row) {
        case PathRowUserDir:
            paths.user_dir = GetDefaultUserDir();
            break;
        case PathRowRomsDir:
            paths.roms_dir = GetDefaultRomsDir(paths.user_dir);
            break;
        default:
            paths.scan_recursive = true;
            break;
        }
        paths_dirty = true;
    }

    void EnsureVisible(const Grid& grid) {
        if (filtered.empty()) {
            return;
        }
        const int row = selected / grid.cols;
        if (row < scroll_row) {
            scroll_row = row;
        } else if (row >= scroll_row + grid.visible_rows) {
            scroll_row = row - grid.visible_rows + 1;
        }
    }

    void HandleTouch() {
        HidTouchScreenState ts{};
        if (hidGetTouchScreenStates(&ts, 1) == 0 || ts.count == 0) {
            touch_was_down = false;
            return;
        }
        // The panel is never rotated, so a contact has to be turned onto the canvas.
        const int px = static_cast<int>(ts.touches[0].x);
        const int py = static_cast<int>(ts.touches[0].y);
        int tx = px;
        int ty = py;
        switch (g_rotation) {
        case 90:
            tx = py;
            ty = g_screen_h - 1 - px;
            break;
        case 180:
            tx = g_screen_w - 1 - px;
            ty = g_screen_h - 1 - py;
            break;
        case 270:
            tx = g_screen_w - 1 - py;
            ty = px;
            break;
        default:
            break;
        }
        if (touch_was_down) {
            return; // Act on the initial contact only.
        }
        touch_was_down = true;

        if (tx < kRailW) {
            if (const std::optional<Tab> hit = RailHitTest(ty)) {
                SetTab(*hit);
                rail_sel = tab;
                focus = Focus::Content;
            }
            return;
        }
        if (tab == Tab::Library) {
            const Grid grid = ComputeGrid();
            for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
                int tile_x, tile_y;
                if (!TileRect(grid, i, tile_x, tile_y)) {
                    continue;
                }
                if (tx >= tile_x && tx < tile_x + kTileW && ty >= tile_y &&
                    ty < tile_y + kTileH) {
                    if (selected == i) {
                        pending_launch = games[filtered[i]].path; // Second tap launches.
                    }
                    selected = i;
                    EnsureVisible(grid);
                    break;
                }
            }
        } else if (tab == Tab::Install) {
            const int row = install_scroll + (ty - kInstallTop) / kInstallRowH;
            if (ty >= kInstallTop && ty < ContentBottom() && row < InstallRowCount()) {
                install_sel = row;
            }
        } else if (tab == Tab::Settings) {
            if (const std::optional<int> page =
                    SettingsTabHitTest(tx, ty, static_cast<int>(settings_page))) {
                SetSettingsPage(static_cast<SettingsPage>(*page));
                return;
            }
            const int visible = (ty - kSettingsTop) / kSettingsRowStride;
            const int row = SettingsScroll() + visible;
            if (ty >= kSettingsTop && visible < SettingsVisibleRows() && row >= 0 &&
                row < static_cast<int>(settings_rows.size())) {
                SettingsSel() = row;
                if (settings_rows[row].modal != SettingsModal::None) {
                    OpenSettingsModal(settings_rows[row].modal);
                } else {
                    settings_rows[row].step(tx > kContentX + ContentW() / 2 ? +1 : -1);
                    settings_dirty = true;
                }
            }
        } else if (tab == Tab::Paths) {
            for (int i = 0; i < PathRowCount; ++i) {
                const int y = PathRowTop(i);
                if (ty < y || ty >= y + PathRowHeight(i)) {
                    continue;
                }
                paths_sel = i;
                if (i == PathRowRecursive) {
                    paths.scan_recursive = !paths.scan_recursive;
                    paths_dirty = true;
                } else {
                    PickFolder(i);
                }
                break;
            }
        } else {
            constexpr int row_top = kContentTop + 112;
            constexpr int row_stride = 78;
            const int row = (ty - row_top) / row_stride;
            if (ty >= row_top && row >= 0 && row < ArticRowCount) {
                if (artic_sel == row) {
                    MenuResult result;
                    if (ActivateArtic(result)) {
                        pending_launch = std::move(result.path);
                    }
                } else {
                    artic_sel = row;
                }
            }
        }
    }

    // Screen rect of filtered tile `i` given the current scroll.
    bool TileRect(const Grid& grid, int i, int& out_x, int& out_y) {
        const int row = i / grid.cols;
        const int col = i % grid.cols;
        const int y = grid.top + (row - scroll_row) * (kTileH + kTileGap);
        if (row < scroll_row || row >= scroll_row + grid.visible_rows) {
            return false;
        }
        out_x = grid.start_x + col * (kTileW + kTileGap);
        out_y = y;
        return true;
    }

    // Modal folder picker. Blocks until a folder is chosen or the user backs out.
    std::optional<std::string> BrowseForFolder(const std::string& start) {
        std::string dir = EnsureDirectory(start) ? start : std::string{"sdmc:/"};
        std::vector<DirEntry> entries = ListSubdirectories(dir);
        int sel = 0;
        int scroll = 0;
        Repeater rep;

        auto Enter = [&](const std::string& next, const std::string& highlight) {
            dir = next;
            entries = ListSubdirectories(dir);
            const int base = ParentDirectory(dir).empty() ? 0 : 1;
            sel = 0;
            scroll = 0;
            for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                if (entries[i].path == highlight) {
                    sel = i + base;
                    break;
                }
            }
        };

        while (appletMainLoop()) {
            padUpdate(pad_state);
            const u64 down = padGetButtonsDown(pad_state);
            const HidAnalogStickState ls = padGetStickPos(pad_state, 0);
            constexpr int dz = 12000;
            const u32 nav = rep.Step((down & HidNpadButton_Up) || ls.y > dz,
                                     (down & HidNpadButton_Down) || ls.y < -dz, false, false);

            const std::string parent = ParentDirectory(dir);
            // Row 0 is ".." everywhere but device root.
            const int base = parent.empty() ? 0 : 1;
            const int count = static_cast<int>(entries.size()) + base;
            sel = std::clamp(sel, 0, std::max(0, count - 1));

            if (nav & DirUp) {
                sel = std::max(0, sel - 1);
            }
            if (nav & DirDown) {
                sel = std::min(std::max(0, count - 1), sel + 1);
            }
            scroll = std::clamp(scroll, std::max(0, sel - BrowseRows() + 1),
                                std::max(0, std::min(sel, count - BrowseRows())));
            if (down & HidNpadButton_A) {
                if (base == 1 && sel == 0) {
                    Enter(parent, dir);
                } else if (sel >= base && sel - base < static_cast<int>(entries.size())) {
                    Enter(entries[sel - base].path, "");
                }
            }
            if (down & HidNpadButton_B) {
                if (parent.empty()) {
                    return std::nullopt;
                }
                Enter(parent, dir);
            }
            if (down & HidNpadButton_Y) {
                return std::nullopt;
            }
            if (down & HidNpadButton_Plus) {
                return dir;
            }

            DrawBrowser(dir, entries, sel, scroll);
            Present();
        }
        return std::nullopt;
    }

    void DrawBrowser(const std::string& dir, const std::vector<DirEntry>& entries, int sel,
                     int scroll) {
        Canvas& c = canvas;
        c.Clear(kColBg);

        g_font.Draw(c, 40, 44, "Select folder", 28, kColText);
        g_font.Draw(c, 40, 76, g_font.TruncateFront(dir, 20, g_screen_w - 80), 20, kColAccent);
        c.FillRect(40, 96, g_screen_w - 80, 1, kColRail);

        const bool has_parent = !ParentDirectory(dir).empty();
        const int base = has_parent ? 1 : 0;
        const int count = static_cast<int>(entries.size()) + base;

        if (count == 0) {
            g_font.Draw(c, 52, kBrowseTop + 30, "No subfolders here", 20, kColTextDim);
        }
        for (int i = scroll; i < std::min(count, scroll + BrowseRows()); ++i) {
            const int y = kBrowseTop + (i - scroll) * kBrowseRowH;
            if (i == sel) {
                c.FillRoundRect(32, y, g_screen_w - 64, kBrowseRowH - 4, 8, kColSurfaceHi);
                c.FillRoundRect(32, y + 8, 4, kBrowseRowH - 20, 2, kColAccent);
            }
            const bool up = has_parent && i == 0;
            const std::string name = up ? ".." : entries[i - base].name + "/";
            g_font.Draw(c, 52, CenterBaseline(y, kBrowseRowH - 4, 20),
                        g_font.Truncate(name, 20, g_screen_w - 128), 20,
                        up ? kColTextDim : kColText);
        }
        DrawListScrollbar(c, g_screen_w - 20, kBrowseTop, BrowseRows(), kBrowseRowH, count, scroll);

        c.FillRect(0, ContentBottom(), g_screen_w, kHintH, kColHintBar);
        c.FillRect(0, ContentBottom(), g_screen_w, 1, kColRail);
        int hx = 40;
        const int hy = ContentBottom() + (kHintH - 26) / 2;
        hx += DrawHint(c, hx, hy, "A", "Open") + 22;
        hx += DrawHint(c, hx, hy, "B", has_parent ? "Up" : "Cancel") + 22;
        hx += DrawHint(c, hx, hy, "+", "Select this folder") + 22;
        DrawHint(c, hx, hy, "Y", "Cancel");
    }

    void DrawPathsPage(Canvas& c) {
        DrawHeader(c, "");
        const bool content_focus = focus == Focus::Content;
        const int x = kContentX + 24;
        const int w = ContentW() - 48;
        for (int i = 0; i < PathRowCount; ++i) {
            const int y = PathRowTop(i);
            const int h = PathRowHeight(i);
            const bool on = i == paths_sel;
            if (on) {
                c.FillRoundRect(x, y, w, h, 10, content_focus ? kColSurfaceHi : kColSurface);
                c.FillRoundRect(x, y + 8, 4, h - 16, 2, content_focus ? kColAccent : kColBadge);
            }
            if (i == PathRowRecursive) {
                g_font.Draw(c, x + 20, CenterBaseline(y, h, 22), PathRowLabel(i), 22, kColText);
                const char* value = paths.scan_recursive ? "On" : "Off";
                const int vw = g_font.Measure(value, 22);
                g_font.Draw(c, x + w - 24 - vw, CenterBaseline(y, h, 22), value, 22,
                            on && content_focus ? kColAccent : kColTextDim);
                continue;
            }
            g_font.Draw(c, x + 20, y + 30, PathRowLabel(i), 22, kColText);
            const std::string& dir = i == PathRowUserDir ? paths.user_dir : paths.roms_dir;
            g_font.Draw(c, x + 20, y + 58, g_font.TruncateFront(dir, 18, w - 44), 18,
                        on && content_focus ? kColAccent : kColTextDim);
        }

        int y = PathRowTop(PathRowCount - 1) + PathRowHeight(PathRowCount - 1) + 30;
        if (RestartPending()) {
            g_font.Draw(c, x + 20, y, "Restart Dekopon to move to the new folder.", 18, kColAccent);
            y += 26;
        }

        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            int hx = HintX();
            const int hy = ContentBottom() + (kHintH - 26) / 2;
            hx +=
                DrawHint(c, hx, hy, "A", paths_sel == PathRowRecursive ? "Toggle" : "Browse") + 22;
            hx += DrawHint(c, hx, hy, "Y", "Default") + 22;
            hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
    }

    void DrawArticPage(Canvas& c) {
        DrawHeader(c, "3DS connectivity");
        const bool content_focus = focus == Focus::Content;
        const int x = kContentX + 24;
        const int w = ContentW() - 48;

        g_font.Draw(c, x + 12, kContentTop + 30,
                    "Run Artic Base or Azahar Artic Setup Tool on a 3DS on this network.", 18,
                    kColTextDim);

        constexpr int row_top = kContentTop + 112;
        constexpr int row_h = 68;
        constexpr int row_stride = 78;
        for (int i = 0; i < ArticRowCount; ++i) {
            const int y = row_top + i * row_stride;
            const bool on = i == artic_sel;
            if (on) {
                c.FillRoundRect(x, y, w, row_h, 10,
                                content_focus ? kColSurfaceHi : kColSurface);
                c.FillRoundRect(x, y + 8, 4, row_h - 16, 2,
                                content_focus ? kColAccent : kColBadge);
            }

            const char* label = "";
            std::string value;
            u32 value_color = on && content_focus ? kColAccent : kColTextDim;
            switch (i) {
            case ArticRowAddress:
                label = "Server Address";
                value = GetArticBaseAddress().empty() ? "Not set" : GetArticBaseAddress();
                break;
            case ArticRowConnect:
                label = "Connect to Artic Base";
                value = "Play a game off a real 3DS";
                break;
            case ArticRowOld3ds:
                label = "Set Up Old 3DS System Files";
                value = artic_install_state.old3ds ? "Installed! Select to reinstall" : "Ready";
                break;
            case ArticRowNew3ds:
                label = "Set Up New 3DS System Files";
                if (!artic_install_state.old3ds) {
                    value = "Old 3DS setup required first";
                    value_color = kColError;
                } else {
                    value = artic_install_state.new3ds ? "Installed! Select to reinstall"
                                                      : "Ready";
                }
                break;
            default:
                label = "Use Real 3DS as a Controller";
                value = GetUseArticBaseController() ? "On" : "Off";
                break;
            }
            g_font.Draw(c, x + 20, y + 27, label, 21, kColText);
            g_font.Draw(c, x + 20, y + 52, g_font.Truncate(value, 17, w - 44), 17, value_color);
        }

        g_font.Draw(c, x + 12, ContentBottom() - 30,
                    "System setup installs unique console data. Keep your Dekopon folder private.",
                    16, kColTextDim);

        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            const char* action = "Select";
            if (artic_sel == ArticRowAddress) {
                action = "Edit";
            } else if (artic_sel == ArticRowConnect) {
                action = "Connect";
            } else if (artic_sel == ArticRowOld3ds || artic_sel == ArticRowNew3ds) {
                action = "Set Up";
            } else if (artic_sel == ArticRowController) {
                action = "Toggle";
            }
            int hx = HintX();
            const int hy = ContentBottom() + (kHintH - 26) / 2;
            hx += DrawHint(c, hx, hy, "A", action) + 22;
            hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
    }

    void DrawArticSetupConfirm(Canvas& c, bool old3ds, bool replacing) {
        const int w = std::min(760, g_screen_w - 48);
        constexpr int h = 326;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

        const std::string title =
            std::string{replacing ? "Reinstall " : "Set up "} +
            (old3ds ? "Old 3DS system files?" : "New 3DS system files?");
        g_font.Draw(c, x + 28, y + 44, title, 24, kColText);
        g_font.Draw(c, x + 28, y + 82,
                    "This connects to Azahar Artic Setup Tool and installs system titles", 18,
                    kColTextDim);
        g_font.Draw(c, x + 28, y + 108,
                    "and console specific data from the real 3DS into this Dekopon folder.", 18,
                    kColTextDim);
        g_font.Draw(c, x + 28, y + 148,
                    "Do not share the folder after setup. Do not take both systems online", 18,
                    kColError);
        g_font.Draw(c, x + 28, y + 174,
                    "at the same time. The selected set of system titles will be replaced.", 18,
                    kColError);
        g_font.Draw(c, x + 28, y + 214,
                    "Both setup modes work from either Old or New 3DS hardware.", 18, kColTextDim);

        int hx = x + 28;
        const int hy = y + h - 42;
        hx += DrawHint(c, hx, hy, "A", replacing ? "Reinstall" : "Continue") + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    void Draw() {
        Canvas& c = canvas;
        c.Clear(kColBg);
        DrawRail(c, tab, rail_sel, focus == Focus::Rail);
        if (tab == Tab::Library) {
            DrawLibrary(c);
        } else if (tab == Tab::Install) {
            DrawInstallPage(c);
        } else if (tab == Tab::Settings) {
            DrawSettingsPage(c);
        } else if (tab == Tab::Paths) {
            DrawPathsPage(c);
        } else {
            DrawArticPage(c);
        }
        DrawNotice(c);
        DrawHintBar(c);
        if (details_open && !filtered.empty()) {
            DrawTitleDetails(c, games[filtered[selected]], details);
        }
        if (layout_picker_open) {
            DrawLayoutPicker(c);
        }
        if (preset_picker_open) {
            DrawPresetPicker(c);
        }
        if (remap_open) {
            DrawRemapPage(c);
        }
        if (confirm) {
            DrawConfirm(c);
        }
        if (install_active) {
            DrawInstallProgress(c);
        }
    }

    void DrawInstallPage(Canvas& c) {
        const std::size_t count_cias = install_cias.size();
        DrawHeader(c, std::to_string(count_cias) + (count_cias == 1 ? " CIA" : " CIAs"));
        const bool content_focus = focus == Focus::Content;
        const int x = kContentX + 24;
        const int w = ContentW() - 48;
        g_font.Draw(c, x, kContentTop + 26, g_font.TruncateFront(install_dir, 18, w), 18,
                    kColAccent);
        c.FillRect(x, kContentTop + kInstallHeaderH, w, 1, kColRail);

        const int count = InstallRowCount();
        if (count == 0) {
            g_font.Draw(c, x + 20, kInstallTop + 30, "No CIAs or subfolders here", 20, kColTextDim);
        }
        const int base = InstallParentRows();
        for (int i = install_scroll; i < std::min(count, install_scroll + InstallRows()); ++i) {
            const int y = kInstallTop + (i - install_scroll) * kInstallRowH;
            if (i == install_sel) {
                c.FillRoundRect(x, y, w, kInstallRowH - 4, 8,
                                content_focus ? kColSurfaceHi : kColSurface);
                c.FillRoundRect(x, y + 8, 4, kInstallRowH - 20, 2,
                                content_focus ? kColAccent : kColBadge);
            }
            const int text_y = CenterBaseline(y, kInstallRowH - 4, 18);
            if (base == 1 && i == 0) {
                g_font.Draw(c, x + 20, text_y, "..", 18, kColTextDim);
                continue;
            }
            const int di = i - base;
            if (di < static_cast<int>(install_dirs.size())) {
                g_font.Draw(c, x + 20, text_y,
                            g_font.Truncate(install_dirs[di].name + "/", 18, w - 44), 18, kColText);
                continue;
            }
            DrawCiaRow(c, install_cias[di - static_cast<int>(install_dirs.size())], x, y, w);
        }
        DrawListScrollbar(c, g_screen_w - 10, kInstallTop, InstallRows(), kInstallRowH, count,
                          install_scroll);

        if (focus == Focus::Rail) {
            DrawRailHints(c);
            return;
        }
        int hx = HintX();
        const int hy = ContentBottom() + (kHintH - 26) / 2;
        hx += DrawHint(c, hx, hy, "A", SelectedCia() ? "Install" : "Open") + 22;
        hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
        hx += DrawHint(c, hx, hy, "Y", "Refresh") + 22;
        DrawHint(c, hx, hy, "+ -", "Exit");
    }

    // Name on the left, then size, version and a kind badge packed to the right.
    void DrawCiaRow(Canvas& c, const CiaEntry& cia, int x, int y, int w) {
        const int text_y = CenterBaseline(y, kInstallRowH - 4, 18);
        const int badge_y = y + (kInstallRowH - 4 - 20) / 2;
        const int badge_text_y = CenterBaseline(badge_y, 20, 14);
        int right = x + w - 20;

        const char* badge = cia.readable ? TitleKindName(cia.kind) : "UNREADABLE";
        const u32 badge_col = cia.readable ? KindBadgeColor(cia.kind) : kColError;
        const int bw = g_font.Measure(badge, 14) + 14;
        right -= bw;
        c.FillRoundRect(right, badge_y, bw, 20, 7, badge_col);
        g_font.Draw(c, right + 7, badge_text_y, badge, 14, kColText);
        right -= 12;

        if (cia.readable) {
            const std::string version = FormatTitleVersion(cia.version);
            const int vw = g_font.Measure(version, 16);
            right -= vw;
            g_font.Draw(c, right, text_y, version, 16, kColTextDim);
            right -= 12;
        }

        const std::string size = FormatSize(cia.size);
        const int sw = g_font.Measure(size, 16);
        right -= sw;
        g_font.Draw(c, right, text_y, size, 16, kColTextDim);

        g_font.Draw(c, x + 20, text_y, g_font.Truncate(cia.name, 18, std::max(60, right - x - 32)),
                    18, kColText);
    }

    void DrawConfirmInstall(Canvas& c, const CiaEntry& cia, bool replacing, u16 installed_version) {
        const int w = std::min(620, g_screen_w - 48);
        constexpr int h = 268;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

        int ty = y + 20;
        g_font.Draw(c, x + 24, ty + 22, "Install this title?", 24, kColText);
        ty += 36;
        g_font.Draw(c, x + 24, ty + 18, g_font.Truncate(cia.name, 18, w - 48), 18, kColTextDim);
        ty += 32;
        c.FillRect(x + 24, ty, w - 48, 1, kColRail);
        ty += 10;

        const auto row = [&](const char* label, const std::string& value, u32 color) {
            g_font.Draw(c, x + 24, ty + 18, label, 18, kColTextDim);
            g_font.Draw(c, x + 190, ty + 18, g_font.Truncate(value, 18, w - 214), 18, color);
            ty += 28;
        };
        row("Type", TitleKindName(cia.kind), kColText);
        row("Title ID", FormatTitleId(cia.program_id), kColText);
        row("Version", FormatTitleVersion(cia.version), kColText);
        if (replacing) {
            row("Replaces", FormatTitleVersion(installed_version), kColError);
        }

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "A", "Install") + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    void DrawInstallProgress(Canvas& c) {
        const std::size_t written = install_written.load();
        const std::size_t total = install_total.load();
        const int w = std::min(560, g_screen_w - 48);
        constexpr int h = 136;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);
        g_font.Draw(c, x + 24, y + 42, g_font.Truncate("Installing " + install_name, 20, w - 48),
                    20, kColText);

        const int bar_x = x + 24;
        const int bar_y = y + 64;
        const int bar_w = w - 48;
        c.FillRoundRect(bar_x, bar_y, bar_w, 10, 5, kColRail);
        const int fill =
            total == 0 ? 0 : static_cast<int>(static_cast<u64>(bar_w) * written / total);
        c.FillRoundRect(bar_x, bar_y, std::clamp(fill, 0, bar_w), 10, 5, kColAccent);

        g_font.Draw(c, bar_x, bar_y + 36, FormatSize(written) + " / " + FormatSize(total), 18,
                    kColTextDim);
        const char* warn = "Don't close Dekopon or turn off the console";
        g_font.Draw(c, x + w - 24 - g_font.Measure(warn, 18), bar_y + 36, warn, 18, kColTextDim);
    }

    void DrawLibrary(Canvas& c) {
        std::string sub;
        if (!search.empty()) {
            sub = "Search: " + search + "  (" + std::to_string(filtered.size()) + ")";
        } else {
            sub = std::to_string(games.size()) + (games.size() == 1 ? " game" : " games");
        }
        DrawHeader(c, sub);

        const bool content_focus = focus == Focus::Content;
        if (filtered.empty()) {
            DrawEmptyLibrary(c, paths.roms_dir);
        } else {
            const Grid grid = ComputeGrid();
            for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
                int x, y;
                if (TileRect(grid, i, x, y)) {
                    DrawTile(c, games[filtered[i]], x, y, i == selected, content_focus);
                }
            }
            DrawScrollbar(c, grid);
        }
        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            int hx = HintX();
            const int hy = ContentBottom() + (kHintH - 26) / 2;
            hx += DrawHint(c, hx, hy, "A", "Launch") + 22;
            hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
            hx += DrawHint(c, hx, hy, "X", "Search") + 22;
            hx += DrawHint(c, hx, hy, "Y", "Refresh") + 22;
            hx += DrawHint(c, hx, hy, "+", "Details") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
    }

    void DrawScrollbar(Canvas& c, const Grid& grid) {
        const int total_rows = (static_cast<int>(filtered.size()) + grid.cols - 1) / grid.cols;
        if (total_rows <= grid.visible_rows) {
            return;
        }
        const int track_h = grid.visible_rows * (kTileH + kTileGap) - kTileGap;
        const int track_x = g_screen_w - 10;
        c.FillRoundRect(track_x, grid.top, 4, track_h, 2, kColRail);
        const int thumb_h = std::max(24, track_h * grid.visible_rows / total_rows);
        const int max_scroll = total_rows - grid.visible_rows;
        const int thumb_y = grid.top + (track_h - thumb_h) * scroll_row / std::max(1, max_scroll);
        c.FillRoundRect(track_x, thumb_y, 4, thumb_h, 2, kColAccent);
    }

    static constexpr int kRowH = 41;

    // Scroll the settings window now that it has overflowed the size of the screen.
    static constexpr int kSettingsTop = kTabStripTop + kTabStripH + 12;
    static constexpr int kSettingsRowStride = kRowH + 8;
    static constexpr int kSettingsFooterH = 52;

    static int SettingsVisibleRows() {
        return std::max(1,
                        (ContentBottom() - kSettingsTop - kSettingsFooterH) / kSettingsRowStride);
    }

    // The controller-mapping modal covers most of the screen and scrolls its own list.
    static constexpr int kRemapRowH = 42;
    static constexpr int kRemapTopPad = 92;    // Room for the title.
    static constexpr int kRemapBottomPad = 56; // Room for the button hints.
    static constexpr int kRemapPanelY = 44;

    static int RemapW() {
        return std::min(860, g_screen_w - 48);
    }

    static int RemapPanelH() {
        return g_screen_h - 2 * kRemapPanelY;
    }

    static int RemapVisibleRows() {
        return std::max(1, (RemapPanelH() - kRemapTopPad - kRemapBottomPad) / kRemapRowH);
    }

    void DrawSettingsTabs(Canvas& c) {
        if (CompactTabStrip()) {
            const char* name = SettingsPageName(settings_page);
            const int baseline = CenterBaseline(kTabStripTop, kTabStripH, 18);
            const int w = g_font.Measure(name, 18) + kTabPadX * 2;
            const int x = kContentX + (ContentW() - w) / 2;
            c.FillRoundRect(x, kTabStripTop, w, kTabStripH, kTabStripH / 2, kColAccent);
            g_font.Draw(c, x + kTabPadX, baseline, name, 18, kColOnAccent);
            g_font.Draw(c, x - 24, baseline, "<", 18, kColTextDim);
            g_font.Draw(c, x + w + 14, baseline, ">", 18, kColTextDim);
            return;
        }
        const auto rects = SettingsTabRects();
        for (int i = 0; i < NumSettingsPages; ++i) {
            const bool on = i == static_cast<int>(settings_page);
            if (on) {
                c.FillRoundRect(rects[i].x, kTabStripTop, rects[i].w, kTabStripH, kTabStripH / 2,
                                kColAccent);
            }
            const char* name = SettingsPageName(static_cast<SettingsPage>(i));
            g_font.Draw(c, rects[i].x + SettingsTabTextInset(rects, i),
                        CenterBaseline(kTabStripTop, kTabStripH, 18), name, 18,
                        on ? kColOnAccent : kColTextDim);
        }
    }

    void DrawSettingsPage(Canvas& c) {
        DrawHeader(c, "");
        DrawSettingsTabs(c);

        const bool content_focus = focus == Focus::Content;
        const int count = static_cast<int>(settings_rows.size());
        const int sel = SettingsSel();
        const int scroll = SettingsScroll();
        const int x = kContentX + 24;
        const int w = ContentW() - 48;
        const int last = std::min(count, scroll + SettingsVisibleRows());
        for (int i = scroll; i < last; ++i) {
            const int y = kSettingsTop + (i - scroll) * kSettingsRowStride;
            const bool on = i == sel;
            if (on) {
                c.FillRoundRect(x, y, w, kRowH, 10, content_focus ? kColSurfaceHi : kColSurface);
                c.FillRoundRect(x, y + 8, 4, kRowH - 16, 2,
                                content_focus ? kColAccent : kColBadge);
            }
            g_font.Draw(c, x + 20, CenterBaseline(y, kRowH, 22), settings_rows[i].label, 22,
                        kColText);
            const std::string value = settings_rows[i].value();
            const int max_value_w = w - 44 - g_font.Measure(settings_rows[i].label, 22);
            const std::string shown = g_font.Truncate(value, 22, std::max(60, max_value_w));
            const int vw = g_font.Measure(shown, 22);
            g_font.Draw(c, x + w - 24 - vw, CenterBaseline(y, kRowH, 22), shown, 22,
                        on && content_focus ? kColAccent : kColTextDim);
        }
        DrawListScrollbar(c, g_screen_w - 20, kSettingsTop, SettingsVisibleRows(),
                          kSettingsRowStride, count, scroll);

        const int footer_y = ContentBottom() - kSettingsFooterH;
        const std::string backend =
            std::string{"Graphics backend: "} + ActiveGraphicsBackendName();
        g_font.Draw(c, x + 20, footer_y + 16, backend, 18, kColTextDim);
        g_font.Draw(c, x + 20, footer_y + 42, "Changes apply the next time you launch a game.", 18,
                    kColTextDim);

        if (focus == Focus::Rail) {
            DrawRailHints(c);
        } else {
            int hx = HintX();
            const int hy = ContentBottom() + (kHintH - 26) / 2;
            const bool modal = count > 0 && settings_rows[sel].modal != SettingsModal::None;
            if (modal) {
                hx += DrawHint(c, hx, hy, "A", "Configure") + 22;
            } else {
                hx += DrawHint(c, hx, hy, "<>", "Change") + 22;
                hx += DrawHint(c, hx, hy, "A", "Next") + 22;
            }
            hx += DrawHint(c, hx, hy, "L R", "Page") + 22;
            hx += DrawHint(c, hx, hy, "B", "Menu") + 22;
            DrawHint(c, hx, hy, "+ -", "Exit");
        }
    }

    void DrawConfirm(Canvas& c) {
        const int body = 86 + 26 * static_cast<int>(confirm->lines.size());
        const int note_y = confirm->note.empty() ? body : body + 8;
        const int hint_y = note_y + (confirm->note.empty() ? 4 : 30);
        const int h = hint_y + 38;
        const int w = std::min(620, g_screen_w - 48);
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

        g_font.Draw(c, x + 24, y + 46, confirm->title, 24, kColText);
        int line_y = y + 86;
        for (const std::string& line : confirm->lines) {
            g_font.Draw(c, x + 24, line_y, line, 18, kColTextDim);
            line_y += 26;
        }
        if (!confirm->note.empty()) {
            g_font.Draw(c, x + 24, y + note_y, confirm->note, 18, kColAccent);
        }

        int hx = x + 24;
        const int hy = y + hint_y;
        hx += DrawHint(c, hx, hy, "A", confirm->accept) + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    // A centred modal to choose which bundle of settings the reset row applies.
    void DrawPresetPicker(Canvas& c) {
        const int w = std::min(620, g_screen_w - 48);
        constexpr int row_h = 58;
        constexpr int top_pad = 78;    // Room for the title and subtitle.
        constexpr int bottom_pad = 82; // Room for the disclaimer and the button hints.
        const int h = top_pad + NumSettingsPresets * row_h + bottom_pad;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

        g_font.Draw(c, x + 24, y + 40, "Reset All Settings", 24, kColText);
        g_font.Draw(c, x + 24, y + 64, "Choose the settings to reset to", 16, kColTextDim);

        for (int i = 0; i < NumSettingsPresets; ++i) {
            const int ry = y + top_pad + i * row_h;
            const int rx = x + 16;
            const int rw = w - 32;
            if (i == preset_sel) {
                c.FillRoundRect(rx, ry, rw, row_h - 4, 8, kColSurfaceHi);
                c.FillRoundRect(rx, ry + 8, 4, row_h - 20, 2, kColAccent);
            }
            const auto preset = static_cast<SettingsPreset>(i);
            g_font.Draw(c, rx + 20, ry + 24, SettingsPresetName(preset), 20, kColText);
            g_font.Draw(c, rx + 20, ry + 46, SettingsPresetSummary(preset), 16, kColTextDim);
        }

        g_font.Draw(c, x + 24, y + h - 62,
                    "Performance and Ultra Performance can break some games.", 16, kColAccent);

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "A", "Choose") + 22;
        DrawHint(c, hx, hy, "B", "Cancel");
    }

    // A centred modal to choose which layouts R3 cycles through in-game.
    void DrawLayoutPicker(Canvas& c) {
        const int count = GetScreenLayoutCount();
        const int w = std::min(620, g_screen_w - 48);
        constexpr int row_h = 44;
        constexpr int top_pad = 78;    // Room for the title and subtitle.
        constexpr int bottom_pad = 56; // Room for the button hints.
        const int h = top_pad + count * row_h + bottom_pad;
        const int x = (g_screen_w - w) / 2;
        const int y = (g_screen_h - h) / 2;
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

        g_font.Draw(c, x + 24, y + 40, "R3 Screen Layouts", 24, kColText);
        g_font.Draw(c, x + 24, y + 64, "Choose which layouts R3 cycles through in-game", 16,
                    kColTextDim);

        for (int i = 0; i < count; ++i) {
            const int ry = y + top_pad + i * row_h;
            const int rx = x + 16;
            const int rw = w - 32;
            if (i == layout_picker_sel) {
                c.FillRoundRect(rx, ry, rw, row_h - 4, 8, kColSurfaceHi);
                c.FillRoundRect(rx, ry + 8, 4, row_h - 20, 2, kColAccent);
            }
            const bool enabled = (GetLayoutCycleMask() & (1u << i)) != 0;
            g_font.Draw(c, rx + 20, CenterBaseline(ry, row_h - 4, 20), GetScreenLayoutName(i), 20,
                        kColText);
            const char* state = enabled ? "On" : "Off";
            const int sw = g_font.Measure(state, 20);
            g_font.Draw(c, rx + rw - 24 - sw, CenterBaseline(ry, row_h - 4, 20), state, 20,
                        enabled ? kColAccent : kColTextDim);
        }

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "A", "Toggle") + 22;
        DrawHint(c, hx, hy, "B", "Done");
    }

    // A near-fullscreen modal that allows rebinding inputs.
    void DrawRemapPage(Canvas& c) {
        const int x = (g_screen_w - RemapW()) / 2;
        const int y = kRemapPanelY;
        const int w = RemapW();
        const int h = RemapPanelH();
        c.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xC0));
        c.RoundBorder(x, y, w, h, 14, 2, kColBadge, kColSurface);

        g_font.Draw(c, x + 24, y + 40, "Controller Mapping", 24, kColText);
        g_font.Draw(c, x + 24, y + 66,
                    "Controller changes apply the next time you launch a game.",
                    16, kColTextDim);

        const int list_top = y + kRemapTopPad;
        const int rx = x + 16;
        const int rw = w - 32;
        const int last = std::min(NumMappableControls, remap_scroll + RemapVisibleRows());
        for (int i = remap_scroll; i < last; ++i) {
            const int ry = list_top + (i - remap_scroll) * kRemapRowH;
            const bool on = i == remap_sel;
            if (on) {
                c.FillRoundRect(rx, ry, rw, kRemapRowH - 4, 8, kColSurfaceHi);
                c.FillRoundRect(rx, ry + 8, 4, kRemapRowH - 20, 2, kColAccent);
            }
            const auto control = static_cast<MappableControl>(i);
            g_font.Draw(c, rx + 20, CenterBaseline(ry, kRemapRowH - 4, 20), ControlName(control), 20,
                        kColText);
            const char* value = PhysicalButtonName(GetMapping(control));
            const int vw = g_font.Measure(value, 20);
            g_font.Draw(c, rx + rw - 24 - vw, CenterBaseline(ry, kRemapRowH - 4, 20), value, 20,
                        on ? kColAccent : kColTextDim);
        }
        DrawListScrollbar(c, x + w - 12, list_top, RemapVisibleRows(), kRemapRowH,
                          NumMappableControls, remap_scroll);

        int hx = x + 24;
        const int hy = y + h - 38;
        hx += DrawHint(c, hx, hy, "<>", "Change") + 22;
        hx += DrawHint(c, hx, hy, "A", "Next") + 22;
        hx += DrawHint(c, hx, hy, "X", "Unbind") + 22;
        hx += DrawHint(c, hx, hy, "Y", "Default") + 22;
        DrawHint(c, hx, hy, "B", "Back");
    }

    void DrawHintBar(Canvas& c) {
        c.FillRect(0, ContentBottom(), g_screen_w, kHintH, kColHintBar);
        c.FillRect(0, ContentBottom(), g_screen_w, 1, kColRail);
    }

    // Legend shown while the cursor sits on the Library/Settings rail.
    void DrawRailHints(Canvas& c) {
        int hx = HintX();
        const int hy = ContentBottom() + (kHintH - 26) / 2;
        hx += DrawHint(c, hx, hy, "^v", "Move") + 22;
        hx += DrawHint(c, hx, hy, "A", "Open") + 22;
        hx += DrawHint(c, hx, hy, "B", "Back") + 22;
        DrawHint(c, hx, hy, "+ -", "Exit");
    }

    // Full-frame busy indicator for the brief blocking scans.
    void ShowBusy(std::string_view msg) {
        Draw();
        canvas.FillRect(0, 0, g_screen_w, g_screen_h, MakeColor(0x10, 0x11, 0x13, 0xB0));
        const int tw = g_font.Measure(msg, 22);
        const int w = tw + 56, h = 56;
        const int x = (g_screen_w - w) / 2, y = (g_screen_h - h) / 2;
        canvas.FillRoundRect(x, y, w, h, 12, kColSurfaceHi);
        g_font.Draw(canvas, x + 28, CenterBaseline(y, h, 22), msg, 22, kColText);
        Present();
    }

    void EnsureFramebuffer() {
        if (fb_ready) {
            return;
        }
        framebufferCreate(&fb, nwindowGetDefault(), kPanelW, kPanelH, PIXEL_FORMAT_RGBA_8888, 2);
        framebufferMakeLinear(&fb);
        fb_ready = true;
    }

    // Picks up a rotation changed from the Settings tab and resizes the canvas to match.
    void ApplyRotation() {
        const int rotation = GetMenuRotation();
        if (rotation == g_rotation && canvas.Width() == g_screen_w) {
            return;
        }
        g_rotation = rotation;
        g_screen_w = RotatedUpright() ? kPanelH : kPanelW;
        g_screen_h = RotatedUpright() ? kPanelW : kPanelH;
        canvas.Resize(g_screen_w, g_screen_h);
        ScrollSelectionsIntoView();
    }

    void ScrollSelectionsIntoView() {
        EnsureVisible(ComputeGrid());
        ScrollSettingsIntoView();
        ScrollRemapIntoView();
    }

    void DrawLoading() {
        canvas.Clear(kColBg);
        const char* msg = "Loading library...";
        const int w = g_font.Measure(msg, 24);
        g_font.Draw(canvas, kContentX + (ContentW() - w) / 2, g_screen_h / 2, msg, 24, kColTextDim);
    }

    // Turns the canvas back onto the panel.
    void Present() {
        EnsureFramebuffer();
        u32 pitch = 0;
        auto* base = static_cast<u8*>(framebufferBegin(&fb, &pitch));
        auto* dst = reinterpret_cast<u32*>(base);
        const int stride = static_cast<int>(pitch / sizeof(u32));
        const u32* src = canvas.Data();
        const int cw = canvas.Width();
        const int ch = canvas.Height();
        constexpr int kTile = 32;
        switch (g_rotation) {
        case 90:
            for (int y0 = 0; y0 < ch; y0 += kTile) {
                for (int x0 = 0; x0 < cw; x0 += kTile) {
                    for (int y = y0, ymax = std::min(y0 + kTile, ch); y < ymax; ++y) {
                        for (int x = x0, xmax = std::min(x0 + kTile, cw); x < xmax; ++x) {
                            dst[x * stride + (ch - 1 - y)] = src[y * cw + x];
                        }
                    }
                }
            }
            break;
        case 180:
            for (int y = 0; y < ch; ++y) {
                u32* row = dst + (ch - 1 - y) * stride;
                for (int x = 0; x < cw; ++x) {
                    row[cw - 1 - x] = src[y * cw + x];
                }
            }
            break;
        case 270:
            for (int y0 = 0; y0 < ch; y0 += kTile) {
                for (int x0 = 0; x0 < cw; x0 += kTile) {
                    for (int y = y0, ymax = std::min(y0 + kTile, ch); y < ymax; ++y) {
                        for (int x = x0, xmax = std::min(x0 + kTile, cw); x < xmax; ++x) {
                            dst[(cw - 1 - x) * stride + y] = src[y * cw + x];
                        }
                    }
                }
            }
            break;
        default:
            for (int y = 0; y < ch; ++y) {
                std::memcpy(dst + y * stride, src + y * cw, static_cast<std::size_t>(cw) * 4);
            }
            break;
        }
        framebufferEnd(&fb);
    }

    Canvas canvas;
    bool touch_was_down = false;
    // Set by a second touch on the focused tile so Run() can escape the loop.
    std::optional<std::string> pending_launch;
};

} // namespace

MenuResult RunMenu(PadState& pad) {
    if (!g_font.Init()) {
        // Exit on no font found.
        return {MenuAction::Exit, {}};
    }
    Menu menu;
    return menu.Run(pad);
}

void SetMenuNotice(const std::string& text, bool error) {
    ShowNotice(text, error);
}

void ShutdownMenu() {
    g_font.Shutdown();
}

} // namespace SwitchFrontend
