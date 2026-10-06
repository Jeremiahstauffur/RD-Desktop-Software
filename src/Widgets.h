#pragma once

#include <string>

#include <imgui.h>

// Windows 11 ("Fluent") flavoured widgets built on top of Dear ImGui.
namespace ui {

enum class InfoKind { Info, Success, Warning, Error };

// Text -------------------------------------------------------------------
void PageTitle(const char* text);                     // large accent coloured header text
void Subtitle(const char* text);                      // medium accent coloured header text
void SecondaryText(const char* text);                 // dimmed body text
void ColoredText(const ImVec4& color, const char* fmt, ...);
void BoldText(const char* text);

// Buttons ----------------------------------------------------------------
bool AccentButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool Button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0));
bool IconButton(const char* id, const char* icon, const char* tooltip = nullptr, bool accent = false);
bool SubtleButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Form controls ----------------------------------------------------------
bool ToggleSwitch(const char* label, bool* value);
bool Checkbox(const char* label, bool* value);
bool TriStateCheckbox(const char* id, int state); // state: 0 off, 1 on, 2 mixed. Returns true when clicked.
bool TextField(const char* id, std::string& value, const char* hint = nullptr, float width = 0.0f,
               ImGuiInputTextFlags flags = 0);
bool SearchField(const char* id, std::string& value, const char* hint, float width);
bool NumberField(const char* id, double& value, const char* format = "%.2f", float width = 0.0f);
bool IntField(const char* id, int& value, float width = 0.0f);
void Label(const char* text);                         // label placed above a form control

// Layout -----------------------------------------------------------------
bool BeginCard(const char* id, const ImVec2& size = ImVec2(0, 0), bool border = true);
void EndCard();
void Divider(float thickness = 1.0f);                 // accent tinted horizontal line
void VerticalSpace(float px);
void AlignRight(float widthOfContent);                // moves cursor so the next item ends at the right edge

// Feedback ---------------------------------------------------------------
void InfoBar(InfoKind kind, const std::string& message, bool* open = nullptr);
void Spinner(const char* id, float radius = 8.0f, float thickness = 2.5f);
void Badge(const char* text, const ImVec4& fill, const ImVec4& textColor);
void Tooltip(const char* text);
void EmptyState(const char* icon, const char* title, const char* subtitle);

} // namespace ui
