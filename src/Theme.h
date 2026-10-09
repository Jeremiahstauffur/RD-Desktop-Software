#pragma once

#include <imgui.h>

// Segoe Fluent Icons / Segoe MDL2 Assets glyphs (UTF-8 encoded).
namespace icons {
inline constexpr const char* Settings     = "\xEE\x9C\x93";   // U+E713
inline constexpr const char* Package      = "\xEE\x9E\xB8";   // U+E7B8
inline constexpr const char* Shop         = "\xEE\x9C\x99";   // U+E719
inline constexpr const char* Repair       = "\xEE\xA4\x8F";   // U+E90F
inline constexpr const char* Document     = "\xEE\xA2\xA5";   // U+E8A5
inline constexpr const char* Message      = "\xEE\xA2\xBD";   // U+E8BD
inline constexpr const char* Refresh      = "\xEE\x9C\xAC";   // U+E72C
inline constexpr const char* Search       = "\xEE\x9C\xA1";   // U+E721
inline constexpr const char* Scan         = "\xEE\xB4\x94";   // U+ED14 (QR code)
inline constexpr const char* Delete       = "\xEE\x9D\x8D";   // U+E74D
inline constexpr const char* Edit         = "\xEE\x9C\x8F";   // U+E70F
inline constexpr const char* Check        = "\xEE\x9C\xBE";   // U+E73E
inline constexpr const char* Close        = "\xEE\x9C\x91";   // U+E711
inline constexpr const char* ChevronDown  = "\xEE\x9C\x8D";   // U+E70D
inline constexpr const char* ChevronRight = "\xEE\x9D\xAC";   // U+E76C
inline constexpr const char* ChevronLeft  = "\xEE\x9D\xAB";   // U+E76B
inline constexpr const char* More         = "\xEE\x9C\x92";   // U+E712
inline constexpr const char* Warning      = "\xEE\x9E\xBA";   // U+E7BA
inline constexpr const char* Sun          = "\xEE\x9C\x86";   // U+E706
inline constexpr const char* Moon         = "\xEE\x9C\x88";   // U+E708
inline constexpr const char* Add          = "\xEE\x9C\x90";   // U+E710
inline constexpr const char* Remove       = "\xEE\x9C\xB8";   // U+E738
inline constexpr const char* Save         = "\xEE\x9D\x8E";   // U+E74E
inline constexpr const char* Send         = "\xEE\x9C\xA4";   // U+E724
inline constexpr const char* Cart         = "\xEE\x9E\xBF";   // U+E7BF
inline constexpr const char* Key          = "\xEE\xA1\x92";   // U+E852
inline constexpr const char* Color        = "\xEE\x9E\x90";   // U+E790
inline constexpr const char* Info         = "\xEE\xA5\x86";   // U+E946
inline constexpr const char* Error        = "\xEE\xA8\xB9";   // U+EA39
inline constexpr const char* People       = "\xEE\x9C\x96";   // U+E716
inline constexpr const char* Back         = "\xEE\x9C\xAB";   // U+E72B
inline constexpr const char* Contact      = "\xEE\x9D\xBB";   // U+E77B

// Code points of every glyph above (used to build the font glyph ranges).
inline constexpr unsigned short CodePoints[] = {
    0xE713, 0xE7B8, 0xE719, 0xE90F, 0xE8A5, 0xE8BD, 0xE72C, 0xE721, 0xED14, 0xE74D,
    0xE70F, 0xE73E, 0xE711, 0xE70D, 0xE76C, 0xE712, 0xE7BA, 0xE706, 0xE708, 0xE710,
    0xE738, 0xE74E, 0xE724, 0xE7BF, 0xE852, 0xE790, 0xE946, 0xEA39, 0xE76B, 0xE716,
    0xE72B, 0xE77B,
};
}

struct Palette {
    ImVec4 windowBg;      // app background (Mica-like)
    ImVec4 surface;       // cards / panels
    ImVec4 surfaceAlt;    // nav rail
    ImVec4 control;       // buttons, text fields
    ImVec4 controlHover;
    ImVec4 controlActive;
    ImVec4 border;        // neutral border for controls
    ImVec4 text;          // body text
    ImVec4 textSecondary;
    ImVec4 textDisabled;
    ImVec4 accent;        // user chosen accent
    ImVec4 accentHover;
    ImVec4 accentActive;
    ImVec4 accentText;    // text drawn on top of accent fills
    ImVec4 accentLine;    // dividers / table lines tinted with accent
    ImVec4 success;
    ImVec4 danger;
};

struct Fonts {
    ImFont* body = nullptr;       // Segoe UI 15px + icons
    ImFont* bodyBold = nullptr;   // Segoe UI Semibold 15px
    ImFont* title = nullptr;      // Segoe UI Semibold 26px (page titles)
    ImFont* subtitle = nullptr;   // Segoe UI Semibold 18px
    ImFont* iconsLarge = nullptr; // Icons 20px (navigation rail)
};

class Theme {
public:
    static Theme& get();

    // (Re)builds the font atlas. `fontScale` multiplies every size (user font-size preference).
    // Must be called between frames; the renderer backend has to re-upload the atlas afterwards.
    void loadFonts(float dpiScale, float fontScale = 1.0f);
    void apply(bool dark, const float accent[3]);

    const Palette& palette() const { return palette_; }
    const Fonts& fonts() const { return fonts_; }
    bool isDark() const { return dark_; }

    static ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
    static ImVec4 withAlpha(ImVec4 c, float a) { c.w = a; return c; }
    static ImVec4 lighten(const ImVec4& c, float amount);

private:
    Palette palette_{};
    Fonts fonts_{};
    bool dark_ = true;
};
