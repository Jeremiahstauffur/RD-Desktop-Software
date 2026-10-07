#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "App.h"
#include "Async.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

namespace {

struct SettingsState {
    bool showKey = false;
    bool showSecret = false;
    std::string authCode;
    bool testing = false;
    std::string testResult;
    bool testOk = false;
    std::string draftKey;
    bool draftLoaded = false;
};

SettingsState& S() {
    static SettingsState s;
    return s;
}

// Windows 11 accent palette presets.
struct Preset { const char* name; float r, g, b; };
const Preset kPresets[] = {
    {"Blue", 0.00f, 0.47f, 0.83f},   {"Teal", 0.00f, 0.60f, 0.60f},  {"Green", 0.06f, 0.55f, 0.24f},
    {"Mint", 0.00f, 0.70f, 0.56f},   {"Purple", 0.53f, 0.33f, 0.80f}, {"Violet", 0.40f, 0.20f, 0.75f},
    {"Pink", 0.89f, 0.28f, 0.58f},   {"Red", 0.91f, 0.27f, 0.25f},   {"Orange", 0.97f, 0.49f, 0.11f},
    {"Gold", 0.85f, 0.65f, 0.09f},   {"Slate", 0.42f, 0.49f, 0.58f}, {"Graphite", 0.47f, 0.47f, 0.47f},
};

void testConnection(App& app) {
    SettingsState& s = S();
    if (s.testing) return;
    s.testing = true;
    s.testResult.clear();
    // Test whatever would be used for real requests: OAuth token if present, otherwise the draft key.
    RepairDeskApi client(app.settings.baseUrl, s.draftKey, app.settings.accessToken);
    Async::run([client] { return client.listInventory("", 1); },
               [](ApiResult<std::vector<InventoryItem>> r) {
                   SettingsState& st = S();
                   st.testing = false;
                   st.testOk = r.ok;
                   st.testResult = r.ok ? "Connected successfully. Your API key works."
                                        : "Connection failed: " + r.message;
               });
}

} // namespace

namespace pages {

void settings(App& app) {
    SettingsState& s = S();
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;

    if (!s.draftLoaded) {
        s.draftKey = app.settings.apiKey;
        s.draftLoaded = true;
    }

    ui::PageTitle("Settings");
    ui::VerticalSpace(4 * dpi);

    ImGui::BeginChild("##settingsScroll", ImVec2(0, 0), ImGuiChildFlags_None);
    float cardW = std::min(ImGui::GetContentRegionAvail().x, 760.0f * dpi);

    // RepairDesk connection ---------------------------------------------------
    if (ui::BeginCard("##connection", ImVec2(cardW, 0))) {
        ui::Subtitle((std::string(icons::Key) + "  RepairDesk connection").c_str());

        // Status line
        {
            const char* status;
            ImVec4 color;
            if (app.settings.hasOAuthToken()) {
                long long remaining = app.settings.tokenExpiresAt - (long long)std::time(nullptr);
                static char buf[96];
                if (remaining > 0) std::snprintf(buf, sizeof(buf), "Connected with OAuth2 - access token valid for %lld min", remaining / 60);
                else std::snprintf(buf, sizeof(buf), "Connected with OAuth2 - access token expired, refreshing...");
                status = buf;
                color = P.success;
            } else if (!app.settings.apiKey.empty()) {
                status = "Connected with API key";
                color = P.success;
            } else {
                status = "Not connected";
                color = P.textSecondary;
            }
            ImGui::TextColored(color, "%s", status);
        }
        ui::VerticalSpace(4 * dpi);

        ui::Label("API base URL");
        if (ui::TextField("##baseurl", app.settings.baseUrl, nullptr, -1)) app.saveSettings();
        ui::VerticalSpace(8 * dpi);
        ui::Divider();
        ui::VerticalSpace(8 * dpi);

        // --- OAuth2 ------------------------------------------------------------
        ui::BoldText("OAuth 2.0 (recommended)");
        ui::SecondaryText("Create an OAuth app in RepairDesk to get a Client ID and Client Secret. Register the redirect URI "
                          "below in that app. The access token is refreshed automatically.");
        ui::VerticalSpace(4 * dpi);

        float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        ImGui::BeginGroup();
        ui::Label("Client ID");
        if (ui::TextField("##clientId", app.settings.clientId, "Client ID", half)) app.saveSettings();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ui::Label("Client Secret");
        float eyeW = ImGui::GetFrameHeight();
        if (ui::TextField("##clientSecret", app.settings.clientSecret, "Client Secret", half - eyeW - ImGui::GetStyle().ItemSpacing.x,
                          s.showSecret ? 0 : ImGuiInputTextFlags_Password)) {
            app.saveSettings();
        }
        ImGui::SameLine();
        if (ui::IconButton("toggleSecret", s.showSecret ? icons::Close : icons::Search, s.showSecret ? "Hide" : "Show")) {
            s.showSecret = !s.showSecret;
        }
        ImGui::EndGroup();

        ui::Label("Redirect URI (registered with your OAuth app)");
        if (ui::TextField("##redirect", app.settings.redirectUri, "http://localhost:8731/callback", -1)) app.saveSettings();

        ui::VerticalSpace(6 * dpi);
        ImGui::BeginDisabled(!app.settings.hasOAuthClient() || app.oauthBusy);
        if (ui::AccentButton((std::string(icons::Key) + "  Sign in with RepairDesk").c_str(), ImVec2(220 * dpi, 0))) {
            app.startOAuthLogin();
        }
        ImGui::EndDisabled();
        if (app.settings.hasOAuthToken()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(app.oauthBusy);
            if (ui::Button("Refresh token", ImVec2(140 * dpi, 0))) app.refreshOAuthToken(true);
            ImGui::SameLine();
            if (ui::Button("Disconnect", ImVec2(120 * dpi, 0))) app.disconnectOAuth();
            ImGui::EndDisabled();
        }
        if (app.oauthBusy) {
            ImGui::SameLine();
            ui::Spinner("##oauthSpin");
        }

        // Manual code entry (for non-loopback redirect URIs)
        ImGui::BeginGroup();
        ui::Label("Or paste an authorization code");
        float goW = 90 * dpi;
        ui::TextField("##authCode", s.authCode, "code from the redirect URL", -goW - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        ImGui::BeginDisabled(s.authCode.empty() || !app.settings.hasOAuthClient() || app.oauthBusy);
        if (ui::Button("Exchange", ImVec2(goW, 0))) {
            app.finishOAuthWithCode(s.authCode);
            s.authCode.clear();
        }
        ImGui::EndDisabled();
        ImGui::EndGroup();

        if (!app.oauthStatus.empty()) {
            ui::VerticalSpace(4 * dpi);
            ui::InfoBar(app.oauthStatusOk ? ui::InfoKind::Success : (app.oauthBusy ? ui::InfoKind::Info : ui::InfoKind::Warning),
                        app.oauthStatus);
        }

        ui::VerticalSpace(8 * dpi);
        ui::Divider();
        ui::VerticalSpace(8 * dpi);

        // --- API key --------------------------------------------------------------
        ui::BoldText("API key (legacy)");
        ui::SecondaryText("Used only when no OAuth2 token is present. Create one in RepairDesk under Store Settings > Integrations > API.");
        ui::VerticalSpace(4 * dpi);
        ui::Label("API key");
        float btnW = ImGui::GetFrameHeight();
        ui::TextField("##apikey", s.draftKey, "Paste your RepairDesk API key", -btnW - ImGui::GetStyle().ItemSpacing.x,
                      s.showKey ? 0 : ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        if (ui::IconButton("toggleKey", s.showKey ? icons::Close : icons::Search, s.showKey ? "Hide key" : "Show key")) {
            s.showKey = !s.showKey;
        }

        ui::VerticalSpace(6 * dpi);
        bool changed = s.draftKey != app.settings.apiKey;
        ImGui::BeginDisabled(!changed);
        if (ui::AccentButton((std::string(icons::Save) + "  Save key").c_str(), ImVec2(140 * dpi, 0))) {
            app.settings.apiKey = s.draftKey;
            app.saveSettings();
            app.inventoryError.clear();
            app.refreshInventory();
            app.notify("API key saved.", ui::InfoKind::Success);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled((s.draftKey.empty() && !app.settings.hasOAuthToken()) || s.testing);
        if (ui::Button("Test connection", ImVec2(160 * dpi, 0))) testConnection(app);
        ImGui::EndDisabled();
        if (s.testing) {
            ImGui::SameLine();
            ui::Spinner("##testSpin");
        }
        if (!s.testResult.empty()) {
            ui::VerticalSpace(4 * dpi);
            ui::InfoBar(s.testOk ? ui::InfoKind::Success : ui::InfoKind::Error, s.testResult);
        }
        ui::EndCard();
    }
    ui::VerticalSpace(8 * dpi);

    // Appearance ---------------------------------------------------------------
    if (ui::BeginCard("##appearance", ImVec2(cardW, 0))) {
        ui::Subtitle((std::string(icons::Color) + "  Appearance").c_str());
        ui::VerticalSpace(4 * dpi);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(app.settings.darkTheme ? icons::Moon : icons::Sun);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Theme");
        ImGui::SameLine();
        ui::AlignRight(ImGui::GetFrameHeight() * 0.8f * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Dark mode").x);
        bool dark = app.settings.darkTheme;
        if (ui::ToggleSwitch(dark ? "Dark mode" : "Light mode", &dark)) {
            app.settings.darkTheme = dark;
            app.applyTheme();
            app.saveSettings();
        }
        ui::VerticalSpace(8 * dpi);
        ui::Divider();
        ui::VerticalSpace(8 * dpi);

        ImGui::TextUnformatted("Accent color");
        ui::SecondaryText("Used for header text, buttons, lines and fills across the app.");
        ui::VerticalSpace(4 * dpi);

        // Preset swatches
        float sw = 34.0f * dpi;
        for (size_t i = 0; i < std::size(kPresets); ++i) {
            const Preset& p = kPresets[i];
            if (i > 0) ImGui::SameLine();
            ImGui::PushID((int)i);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##swatch", ImVec2(sw, sw));
            bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(p.r, p.g, p.b, 1.0f));
            dl->AddRectFilled(pos, ImVec2(pos.x + sw, pos.y + sw), col, 6.0f * dpi);
            bool selected = std::abs(app.settings.accent[0] - p.r) < 0.01f && std::abs(app.settings.accent[1] - p.g) < 0.01f &&
                            std::abs(app.settings.accent[2] - p.b) < 0.01f;
            if (selected || hovered) {
                dl->AddRect(ImVec2(pos.x - 2, pos.y - 2), ImVec2(pos.x + sw + 2, pos.y + sw + 2),
                            Theme::u32(selected ? P.text : P.textSecondary), 7.0f * dpi, 0, 2.0f);
            }
            if (hovered) ui::Tooltip(p.name);
            if (clicked) {
                app.settings.accent[0] = p.r;
                app.settings.accent[1] = p.g;
                app.settings.accent[2] = p.b;
                app.applyTheme();
                app.saveSettings();
            }
            ImGui::PopID();
        }
        ui::VerticalSpace(8 * dpi);

        // Full color scale picker
        ImGui::SetNextItemWidth(cardW - 32 * dpi);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, P.control);
        if (ImGui::ColorPicker3("##accentPicker", app.settings.accent,
                                ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueBar |
                                    ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoAlpha)) {
            app.applyTheme();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemDeactivatedAfterEdit()) app.saveSettings();
        ui::EndCard();
    }
    ui::VerticalSpace(8 * dpi);

    // Data ---------------------------------------------------------------------
    if (ui::BeginCard("##data", ImVec2(cardW, 0))) {
        ui::Subtitle((std::string(icons::Package) + "  Data").c_str());
        ui::Label("Max inventory items to load (0 for all items)");
        int ps = app.settings.inventoryPageSize;
        if (ui::IntField("##pagesize", ps, 160 * dpi)) {
            app.settings.inventoryPageSize = std::max(0, ps);
            app.saveSettings();
        }
        ui::VerticalSpace(4 * dpi);
        std::string path = "Settings are stored in " + Settings::settingsPath();
        ui::SecondaryText(path.c_str());
        ui::EndCard();
    }

    ImGui::EndChild();
}

} // namespace pages
