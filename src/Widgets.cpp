#include "Widgets.h"

#include <cmath>
#include <cstdarg>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "Theme.h"

namespace ui {

namespace {

const Palette& P() { return Theme::get().palette(); }
const Fonts& F() { return Theme::get().fonts(); }

// Draws the Windows 11 "focus underline" on the last item when it is active.
void focusUnderline() {
    if (!ImGui::IsItemActive()) return;
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().FrameRounding;
    dl->AddLine(ImVec2(min.x + r, max.y - 1.0f), ImVec2(max.x - r, max.y - 1.0f), Theme::u32(P().accent), 2.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void PageTitle(const char* text) {
    ImGui::PushFont(F().title);
    ImGui::TextColored(P().accent, "%s", text);
    ImGui::PopFont();
}

void Subtitle(const char* text) {
    ImGui::PushFont(F().subtitle);
    ImGui::TextColored(P().accent, "%s", text);
    ImGui::PopFont();
}

void SecondaryText(const char* text) {
    ImGui::TextColored(P().textSecondary, "%s", text);
}

void ColoredText(const ImVec4& color, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void BoldText(const char* text) {
    ImGui::PushFont(F().bodyBold);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

bool AccentButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, P().accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, P().accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, P().accentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, P().accentText);
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::withAlpha(P().accent, 0.0f));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return pressed;
}

bool Button(const char* label, const ImVec2& size) {
    return ImGui::Button(label, size);
}

bool DangerButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, P().danger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::lighten(P().danger, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::lighten(P().danger, -0.08f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return pressed;
}

bool SubtleButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::withAlpha(P().text, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::withAlpha(P().text, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool IconButton(const char* id, const char* icon, const char* tooltip, bool accent) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::withAlpha(P().text, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::withAlpha(P().text, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, accent ? P().accent : P().text);
    float h = ImGui::GetFrameHeight();
    bool pressed = ImGui::Button(icon, ImVec2(h, h));
    ImGui::PopStyleColor(5);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) Tooltip(tooltip);
    ImGui::PopID();
    return pressed;
}

// ---------------------------------------------------------------------------
// Form controls
// ---------------------------------------------------------------------------

bool ToggleSwitch(const char* label, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const float height = ImGui::GetFrameHeight() * 0.8f;
    const float width = height * 2.0f;
    const float radius = height * 0.5f;
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    float totalWidth = width + (labelSize.x > 0 ? ImGui::GetStyle().ItemInnerSpacing.x + labelSize.x : 0.0f);
    ImRect bb(pos, ImVec2(pos.x + totalWidth, pos.y + ImGui::GetFrameHeight()));
    ImGui::ItemSize(bb);
    ImGuiID id = window->GetID(label);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) *value = !*value;

    // Animation
    float t = *value ? 1.0f : 0.0f;
    ImGuiContext& g = *GImGui;
    const float animSpeed = 0.10f;
    if (g.LastActiveId == id && g.LastActiveIdTimer < animSpeed) {
        float a = ImSaturate(g.LastActiveIdTimer / animSpeed);
        t = *value ? a : 1.0f - a;
    }

    float yOff = (ImGui::GetFrameHeight() - height) * 0.5f;
    ImVec2 trackMin(pos.x, pos.y + yOff);
    ImVec2 trackMax(pos.x + width, pos.y + yOff + height);
    ImDrawList* dl = window->DrawList;

    ImVec4 onColor = hovered ? P().accentHover : P().accent;
    if (*value) {
        dl->AddRectFilled(trackMin, trackMax, Theme::u32(onColor), radius);
    } else {
        dl->AddRectFilled(trackMin, trackMax, Theme::u32(hovered ? P().controlHover : P().control), radius);
        dl->AddRect(trackMin, trackMax, Theme::u32(Theme::withAlpha(P().text, 0.55f)), radius, 0, 1.0f);
    }
    float knobR = radius - 4.0f;
    if (hovered) knobR += 1.0f;
    float knobX = trackMin.x + radius + t * (width - 2.0f * radius);
    ImU32 knobColor = *value ? Theme::u32(P().accentText) : Theme::u32(Theme::withAlpha(P().text, 0.8f));
    dl->AddCircleFilled(ImVec2(knobX, trackMin.y + radius), knobR, knobColor);

    if (labelSize.x > 0) {
        ImVec2 textPos(trackMax.x + ImGui::GetStyle().ItemInnerSpacing.x,
                       pos.y + (ImGui::GetFrameHeight() - labelSize.y) * 0.5f);
        ImGui::RenderText(textPos, label);
    }
    return pressed;
}

static bool drawCheckbox(const char* label, int state, bool* outClicked) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    const float box = 20.0f;
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    float h = ImMax(box, ImGui::GetFrameHeight());
    float totalWidth = box + (labelSize.x > 0 ? ImGui::GetStyle().ItemInnerSpacing.x + labelSize.x : 0.0f);
    ImRect bb(pos, ImVec2(pos.x + totalWidth, pos.y + h));
    ImGui::ItemSize(bb);
    ImGuiID id = window->GetID(label);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    *outClicked = pressed;

    float yOff = (h - box) * 0.5f;
    ImVec2 bmin(pos.x, pos.y + yOff);
    ImVec2 bmax(pos.x + box, pos.y + yOff + box);
    ImDrawList* dl = window->DrawList;
    float r = 4.0f;

    if (state != 0) {
        dl->AddRectFilled(bmin, bmax, Theme::u32(hovered ? P().accentHover : P().accent), r);
        ImU32 mark = Theme::u32(P().accentText);
        if (state == 1) {
            // check mark
            float pad = box * 0.25f;
            ImVec2 a(bmin.x + pad, bmin.y + box * 0.52f);
            ImVec2 b(bmin.x + box * 0.42f, bmax.y - pad);
            ImVec2 c(bmax.x - pad, bmin.y + pad + 1.0f);
            dl->AddLine(a, b, mark, 2.0f);
            dl->AddLine(b, c, mark, 2.0f);
        } else {
            // indeterminate dash
            float pad = box * 0.28f;
            float y = bmin.y + box * 0.5f;
            dl->AddLine(ImVec2(bmin.x + pad, y), ImVec2(bmax.x - pad, y), mark, 2.0f);
        }
    } else {
        dl->AddRectFilled(bmin, bmax, Theme::u32(hovered ? P().controlHover : P().control), r);
        dl->AddRect(bmin, bmax, Theme::u32(Theme::withAlpha(P().text, hovered ? 0.7f : 0.5f)), r, 0, 1.0f);
    }

    if (labelSize.x > 0) {
        ImVec2 textPos(bmax.x + ImGui::GetStyle().ItemInnerSpacing.x, pos.y + (h - labelSize.y) * 0.5f);
        ImGui::RenderText(textPos, label);
    }
    return true;
}

bool Checkbox(const char* label, bool* value) {
    bool clicked = false;
    drawCheckbox(label, *value ? 1 : 0, &clicked);
    if (clicked) *value = !*value;
    return clicked;
}

bool TriStateCheckbox(const char* id, int state) {
    bool clicked = false;
    drawCheckbox(id, state, &clicked);
    return clicked;
}

bool TextField(const char* id, std::string& value, const char* hint, float width, ImGuiInputTextFlags flags) {
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    bool changed = hint ? ImGui::InputTextWithHint(id, hint, &value, flags) : ImGui::InputText(id, &value, flags);
    focusUnderline();
    return changed;
}

bool SearchField(const char* id, std::string& value, const char* hint, float width) {
    ImGui::PushID(id);
    ImVec2 start = ImGui::GetCursorScreenPos();
    float frameH = ImGui::GetFrameHeight();
    float iconW = frameH;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + iconW - 4.0f,
                                                           ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputTextWithHint("##search", hint, &value, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    focusUnderline();

    // search glyph on the left
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 textSize = ImGui::CalcTextSize(icons::Search);
    dl->AddText(ImVec2(start.x + 10.0f, start.y + (frameH - textSize.y) * 0.5f),
                Theme::u32(P().textSecondary), icons::Search);
    ImGui::PopID();
    return changed;
}

bool NumberField(const char* id, double& value, const char* format, float width) {
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputDouble(id, &value, 0.0, 0.0, format);
    focusUnderline();
    return changed;
}

bool IntField(const char* id, int& value, float width) {
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputInt(id, &value, 0, 0);
    focusUnderline();
    return changed;
}

void Label(const char* text) {
    ImGui::TextColored(P().textSecondary, "%s", text);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

bool BeginCard(const char* id, const ImVec2& size, bool border) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P().surface);
    ImGui::PushStyleColor(ImGuiCol_Border, border ? P().border : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                                                (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return open;
}

void EndCard() {
    ImGui::EndChild();
}

void Divider(float thickness) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(pos, ImVec2(pos.x + w, pos.y), Theme::u32(P().accentLine), thickness);
    ImGui::Dummy(ImVec2(w, thickness));
}

void VerticalSpace(float px) {
    ImGui::Dummy(ImVec2(0, px));
}

void AlignRight(float widthOfContent) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > widthOfContent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - widthOfContent);
}

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

void InfoBar(InfoKind kind, const std::string& message, bool* open) {
    if (open && !*open) return;
    ImVec4 color;
    const char* icon;
    switch (kind) {
        case InfoKind::Success: color = P().success; icon = icons::Check; break;
        case InfoKind::Warning: color = ImVec4(0.99f, 0.76f, 0.17f, 1.0f); icon = icons::Warning; break;
        case InfoKind::Error:   color = P().danger; icon = icons::Error; break;
        default:                color = P().accent; icon = icons::Info; break;
    }
    ImGui::PushID(message.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::withAlpha(color, Theme::get().isDark() ? 0.16f : 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::withAlpha(color, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild("##infobar", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(color, "%s", icon);
    ImGui::SameLine();
    float closeW = open ? ImGui::GetFrameHeight() + 8.0f : 0.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeW);
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopTextWrapPos();
    if (open) {
        ImGui::SameLine();
        AlignRight(ImGui::GetFrameHeight());
        if (IconButton("close", icons::Close, "Dismiss")) *open = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

void Spinner(const char* id, float radius, float thickness) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 size(radius * 2, ImGui::GetFrameHeight());
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, window->GetID(id))) return;

    float t = (float)ImGui::GetTime();
    ImVec2 center(pos.x + radius, pos.y + size.y * 0.5f);
    float start = t * 6.0f;
    float sweep = IM_PI * 1.3f;
    window->DrawList->PathArcTo(center, radius - thickness * 0.5f, start, start + sweep, 24);
    window->DrawList->PathStroke(Theme::u32(P().accent), 0, thickness);
}

void Badge(const char* text, const ImVec4& fill, const ImVec4& textColor) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 pad(8, 2);
    ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::u32(fill), size.y * 0.5f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), Theme::u32(textColor), text);
    ImGui::Dummy(size);
}

void Tooltip(const char* text) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

void EmptyState(const char* icon, const char* title, const char* subtitle) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.3f);
    auto centered = [&](ImFont* font, const ImVec4& col, const char* s) {
        ImGui::PushFont(font);
        float w = ImGui::CalcTextSize(s).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f);
        ImGui::TextColored(col, "%s", s);
        ImGui::PopFont();
    };
    centered(F().title, Theme::withAlpha(P().accent, 0.8f), icon);
    centered(F().subtitle, P().text, title);
    centered(F().body, P().textSecondary, subtitle);
}

} // namespace ui
