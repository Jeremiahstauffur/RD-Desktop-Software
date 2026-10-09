#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace {

ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

float luminance(const ImVec4& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

const char* firstExisting(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        if (std::filesystem::exists(c)) return c;
    }
    return nullptr;
}

// Glyph ranges for the icon font: one [cp, cp] pair per used glyph, 0-terminated.
const ImWchar* iconRanges() {
    static std::vector<ImWchar> ranges;
    if (ranges.empty()) {
        std::vector<unsigned short> cps(std::begin(icons::CodePoints), std::end(icons::CodePoints));
        std::sort(cps.begin(), cps.end());
        for (unsigned short cp : cps) {
            ranges.push_back(cp);
            ranges.push_back(cp);
        }
        ranges.push_back(0);
    }
    return ranges.data();
}

} // namespace

Theme& Theme::get() {
    static Theme instance;
    return instance;
}

ImVec4 Theme::lighten(const ImVec4& c, float amount) {
    return ImVec4(std::clamp(c.x + amount, 0.0f, 1.0f),
                  std::clamp(c.y + amount, 0.0f, 1.0f),
                  std::clamp(c.z + amount, 0.0f, 1.0f), c.w);
}

void Theme::loadFonts(float dpiScale, float fontScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Every font is scaled by the user's font-size preference (1.0 = 15px body text).
    dpiScale *= fontScale > 0.0f ? fontScale : 1.0f;

    const char* regular = firstExisting({"C:\\Windows\\Fonts\\segoeui.ttf"});
    const char* semibold = firstExisting({"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
                                          "C:\\Windows\\Fonts\\segoeui.ttf"});
    const char* iconFont = firstExisting({"C:\\Windows\\Fonts\\SegoeIcons.ttf", "C:\\Windows\\Fonts\\segmdl2.ttf"});

    auto addFont = [&](const char* path, float size) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        if (!path) return io.Fonts->AddFontDefault();
        return io.Fonts->AddFontFromFileTTF(path, size * dpiScale, &cfg);
    };
    auto mergeIcons = [&](float size) {
        if (!iconFont) return;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = size * dpiScale;
        cfg.GlyphOffset = ImVec2(0.0f, 2.0f * dpiScale);
        io.Fonts->AddFontFromFileTTF(iconFont, size * dpiScale, &cfg, iconRanges());
    };

    fonts_.body = addFont(regular, 15.0f);
    mergeIcons(14.0f);
    fonts_.bodyBold = addFont(semibold, 15.0f);
    mergeIcons(14.0f);
    fonts_.subtitle = addFont(semibold, 18.0f);
    mergeIcons(17.0f);
    fonts_.title = addFont(semibold, 26.0f);
    mergeIcons(24.0f);
    fonts_.iconsLarge = addFont(regular, 20.0f);
    mergeIcons(19.0f);

    io.FontDefault = fonts_.body;
}

void Theme::apply(bool dark, const float accentRgb[3]) {
    dark_ = dark;
    Palette& p = palette_;

    if (dark) {
        p.windowBg = rgb(32, 32, 32);
        p.surface = rgb(44, 44, 44);
        p.surfaceAlt = rgb(38, 38, 38);
        p.control = rgb(55, 55, 55);
        p.controlHover = rgb(63, 63, 63);
        p.controlActive = rgb(48, 48, 48);
        p.border = rgb(72, 72, 72);
        p.text = rgb(255, 255, 255);
        p.textSecondary = rgb(255, 255, 255, 0.62f);
        p.textDisabled = rgb(255, 255, 255, 0.36f);
    } else {
        p.windowBg = rgb(243, 243, 243);
        p.surface = rgb(251, 251, 251);
        p.surfaceAlt = rgb(238, 238, 238);
        p.control = rgb(255, 255, 255);
        p.controlHover = rgb(246, 246, 246);
        p.controlActive = rgb(240, 240, 240);
        p.border = rgb(220, 220, 220);
        p.text = rgb(27, 27, 27);
        p.textSecondary = rgb(27, 27, 27, 0.62f);
        p.textDisabled = rgb(27, 27, 27, 0.36f);
    }

    p.accent = ImVec4(accentRgb[0], accentRgb[1], accentRgb[2], 1.0f);
    p.accentHover = lighten(p.accent, dark ? 0.08f : -0.06f);
    p.accentActive = lighten(p.accent, dark ? -0.08f : -0.12f);
    p.accentText = luminance(p.accent) > 0.55f ? rgb(0, 0, 0) : rgb(255, 255, 255);
    p.accentLine = withAlpha(p.accent, dark ? 0.45f : 0.55f);
    p.success = rgb(108, 203, 95);
    p.danger = rgb(255, 99, 97);
    if (!dark) {
        p.success = rgb(15, 123, 15);
        p.danger = rgb(196, 43, 28);
    }

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();   // reset to defaults before customising

    // Windows 11 geometry
    s.WindowRounding = 8.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(12, 7);
    s.CellPadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 12.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    s.AntiAliasedLines = true;
    s.AntiAliasedFill = true;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDisabled;
    c[ImGuiCol_WindowBg] = p.windowBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = dark ? rgb(44, 44, 44) : rgb(249, 249, 249);
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.control;
    c[ImGuiCol_FrameBgHovered] = p.controlHover;
    c[ImGuiCol_FrameBgActive] = p.controlActive;
    c[ImGuiCol_TitleBg] = p.windowBg;
    c[ImGuiCol_TitleBgActive] = p.windowBg;
    c[ImGuiCol_TitleBgCollapsed] = p.windowBg;
    c[ImGuiCol_MenuBarBg] = p.surfaceAlt;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = withAlpha(p.text, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = withAlpha(p.text, 0.40f);
    c[ImGuiCol_ScrollbarGrabActive] = withAlpha(p.text, 0.55f);
    c[ImGuiCol_CheckMark] = p.accentText;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accentActive;
    c[ImGuiCol_Button] = p.control;
    c[ImGuiCol_ButtonHovered] = p.controlHover;
    c[ImGuiCol_ButtonActive] = p.controlActive;
    c[ImGuiCol_Header] = withAlpha(p.accent, 0.35f);
    c[ImGuiCol_HeaderHovered] = withAlpha(p.text, 0.06f);
    c[ImGuiCol_HeaderActive] = withAlpha(p.text, 0.10f);
    c[ImGuiCol_Separator] = p.accentLine;
    c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = withAlpha(p.accent, 0.3f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(p.accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_Tab] = p.control;
    c[ImGuiCol_TabHovered] = p.controlHover;
    c[ImGuiCol_TabSelected] = p.surface;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TabDimmed] = p.control;
    c[ImGuiCol_TabDimmedSelected] = p.surface;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderStrong] = p.accentLine;
    c[ImGuiCol_TableBorderLight] = p.accentLine;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg] = withAlpha(p.accent, 0.45f);
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_NavCursor] = withAlpha(p.accent, 0.8f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark ? 0.55f : 0.35f);
    c[ImGuiCol_PlotLines] = p.accent;
    c[ImGuiCol_PlotHistogram] = p.accent;
}
