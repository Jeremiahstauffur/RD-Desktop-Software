#include "App.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "Async.h"
#include "Pages.h"
#include "Theme.h"

namespace {

constexpr float kNavWidth = 232.0f;

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string timeNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

} // namespace

App::App(HWND hwnd, float dpi) : dpiScale(dpi), hwnd_(hwnd) {
    settings.load();
    applyTheme();
    if (!connected()) {
        notify("Welcome! Open Settings to connect to RepairDesk with an API key or OAuth2 client credentials.",
               ui::InfoKind::Info);
    } else if (settings.hasOAuthToken() && settings.tokenExpiresAt - (long long)std::time(nullptr) < 120 &&
               settings.hasOAuthClient() && !settings.refreshToken.empty()) {
        refreshOAuthToken(true);   // token (nearly) expired: refresh first, inventory loads afterwards
    } else {
        refreshInventory();
    }
}

// ---------------------------------------------------------------------------
// OAuth2
// ---------------------------------------------------------------------------

static int portFromUri(const std::string& uri) {
    size_t scheme = uri.find("://");
    size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t slash = uri.find('/', start);
    std::string hostPort = uri.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    size_t colon = hostPort.rfind(':');
    if (colon == std::string::npos) return uri.rfind("https://", 0) == 0 ? 443 : 80;
    return std::atoi(hostPort.c_str() + colon + 1);
}

static bool isLoopbackUri(const std::string& uri) {
    return uri.find("://localhost") != std::string::npos || uri.find("://127.0.0.1") != std::string::npos;
}

void App::applyTokens(const OAuthTokens& tokens) {
    settings.accessToken = tokens.accessToken;
    if (!tokens.refreshToken.empty()) settings.refreshToken = tokens.refreshToken;
    settings.tokenExpiresAt = tokens.expiresAt;
    saveSettings();
}

void App::startOAuthLogin() {
    if (oauthBusy) return;
    if (!settings.hasOAuthClient()) {
        oauthStatus = "Enter the Client ID and Client Secret first.";
        oauthStatusOk = false;
        return;
    }
    oauthBusy = true;
    oauthStatusOk = false;

    // random state for CSRF protection
    char st[24];
    std::snprintf(st, sizeof(st), "%08x%08x", (unsigned)std::time(nullptr), (unsigned)GetTickCount());
    oauthState_ = st;

    std::string url = RepairDeskApi::authorizeUrl(settings.baseUrl, settings.clientId, settings.redirectUri, oauthState_);
    const std::string baseUrl = settings.baseUrl, clientId = settings.clientId, clientSecret = settings.clientSecret,
                      redirectUri = settings.redirectUri, expectedState = oauthState_;

    if (!isLoopbackUri(redirectUri)) {
        // Cannot listen for the redirect: open the browser and let the user paste the code.
        http::openInBrowser(url);
        oauthBusy = false;
        oauthStatus = "Browser opened. After approving access, paste the authorization code below.";
        return;
    }

    oauthStatus = "Waiting for you to approve access in the browser...";
    int port = portFromUri(redirectUri);
    http::openInBrowser(url);

    struct Outcome { ApiResult<OAuthTokens> tokens; std::string error; };
    Async::run(
        [port, baseUrl, clientId, clientSecret, redirectUri, expectedState] {
            Outcome out;
            std::string err;
            std::string query = http::waitForLoopbackRedirect(port, 300, err);
            if (query.empty()) { out.error = err; return out; }
            std::string oauthErr = http::queryParam(query, "error");
            if (!oauthErr.empty()) {
                std::string desc = http::queryParam(query, "error_description");
                out.error = desc.empty() ? oauthErr : oauthErr + ": " + desc;
                return out;
            }
            std::string state = http::queryParam(query, "state");
            if (!state.empty() && state != expectedState) { out.error = "State mismatch - please try again."; return out; }
            std::string code = http::queryParam(query, "code");
            if (code.empty()) { out.error = "No authorization code was returned."; return out; }
            out.tokens = RepairDeskApi::exchangeCode(baseUrl, clientId, clientSecret, code, redirectUri);
            return out;
        },
        [this](Outcome out) {
            oauthBusy = false;
            if (!out.error.empty()) {
                oauthStatus = "Sign-in failed: " + out.error;
                oauthStatusOk = false;
                notify(oauthStatus, ui::InfoKind::Error);
            } else if (!out.tokens.ok) {
                oauthStatus = "Token exchange failed: " + out.tokens.message;
                oauthStatusOk = false;
                notify(oauthStatus, ui::InfoKind::Error);
            } else {
                applyTokens(out.tokens.value);
                oauthStatus = "Connected with OAuth2. Access token obtained.";
                oauthStatusOk = true;
                notify("Connected to RepairDesk via OAuth2.", ui::InfoKind::Success);
                inventoryError.clear();
                refreshInventory();
            }
        });
}

void App::finishOAuthWithCode(const std::string& code) {
    if (oauthBusy || code.empty()) return;
    oauthBusy = true;
    const std::string baseUrl = settings.baseUrl, clientId = settings.clientId, clientSecret = settings.clientSecret,
                      redirectUri = settings.redirectUri;
    Async::run([baseUrl, clientId, clientSecret, code, redirectUri] {
                   return RepairDeskApi::exchangeCode(baseUrl, clientId, clientSecret, code, redirectUri);
               },
               [this](ApiResult<OAuthTokens> r) {
                   oauthBusy = false;
                   oauthStatusOk = r.ok;
                   if (r.ok) {
                       applyTokens(r.value);
                       oauthStatus = "Connected with OAuth2. Access token obtained.";
                       notify("Connected to RepairDesk via OAuth2.", ui::InfoKind::Success);
                       inventoryError.clear();
                       refreshInventory();
                   } else {
                       oauthStatus = "Token exchange failed: " + r.message;
                       notify(oauthStatus, ui::InfoKind::Error);
                   }
               });
}

void App::refreshOAuthToken(bool force) {
    if (oauthBusy || !settings.hasOAuthClient() || settings.refreshToken.empty()) return;
    long long now = (long long)std::time(nullptr);
    if (!force && settings.tokenExpiresAt - now > 120) return;
    oauthBusy = true;
    const std::string baseUrl = settings.baseUrl, clientId = settings.clientId, clientSecret = settings.clientSecret,
                      refreshToken = settings.refreshToken;
    Async::run([baseUrl, clientId, clientSecret, refreshToken] {
                   return RepairDeskApi::refreshAccessToken(baseUrl, clientId, clientSecret, refreshToken);
               },
               [this](ApiResult<OAuthTokens> r) {
                   oauthBusy = false;
                   if (r.ok) {
                       bool hadInventory = inventoryLoadedOnce;
                       applyTokens(r.value);
                       oauthStatus = "Access token refreshed.";
                       oauthStatusOk = true;
                       if (!hadInventory) refreshInventory();
                   } else {
                       oauthStatus = "Token refresh failed: " + r.message + ". Please sign in again.";
                       oauthStatusOk = false;
                       settings.accessToken.clear();
                       settings.refreshToken.clear();
                       settings.tokenExpiresAt = 0;
                       saveSettings();
                       notify(oauthStatus, ui::InfoKind::Error);
                   }
               });
}

void App::disconnectOAuth() {
    if (settings.hasOAuthClient() && !settings.accessToken.empty()) {
        const std::string baseUrl = settings.baseUrl, clientId = settings.clientId, clientSecret = settings.clientSecret,
                          token = settings.accessToken;
        Async::run([baseUrl, clientId, clientSecret, token] {
                       return RepairDeskApi::revokeToken(baseUrl, clientId, clientSecret, token);
                   },
                   [](ApiResult<json>) {});
    }
    settings.accessToken.clear();
    settings.refreshToken.clear();
    settings.tokenExpiresAt = 0;
    saveSettings();
    oauthStatus = "Disconnected.";
    oauthStatusOk = false;
}

void App::maintainOAuthToken() {
    double now = ImGui::GetTime();
    if (now < nextRefreshCheck_) return;
    nextRefreshCheck_ = now + 15.0;
    if (settings.hasOAuthToken()) refreshOAuthToken(false);
}

// ---------------------------------------------------------------------------
// Theme / settings
// ---------------------------------------------------------------------------

void App::applyTheme() {
    Theme::get().apply(settings.darkTheme, settings.accent);
    ImGui::GetStyle().ScaleAllSizes(dpiScale);
    themeDirty_ = true;
}

void App::saveSettings() {
    settings.save();
}

void App::updateWindowChrome() {
    if (!themeDirty_ || !hwnd_) return;
    themeDirty_ = false;
    BOOL dark = settings.darkTheme ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1+ / Windows 11)
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
    int corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    // DWMWA_BORDER_COLOR = 34 - tint the thin window border with the accent colour
    COLORREF border = RGB((int)(settings.accent[0] * 255), (int)(settings.accent[1] * 255), (int)(settings.accent[2] * 255));
    DwmSetWindowAttribute(hwnd_, 34, &border, sizeof(border));
    // DWMWA_CAPTION_COLOR = 35 - match the title bar with the app background
    const Palette& p = Theme::get().palette();
    COLORREF caption = RGB((int)(p.windowBg.x * 255), (int)(p.windowBg.y * 255), (int)(p.windowBg.z * 255));
    DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof(caption));
}

// ---------------------------------------------------------------------------
// Inventory cache
// ---------------------------------------------------------------------------

void App::refreshInventory(const std::string& keyword) {
    if (inventoryLoading) return;
    if (!connected()) {
        inventoryError = "Not connected. Open Settings and enter an API key or connect with OAuth2.";
        return;
    }
    inventoryLoading = true;
    inventoryError.clear();
    RepairDeskApi client = api();
    int pageSize = settings.inventoryPageSize;
    Async::run(
        [client, keyword, pageSize] { return client.listInventory(keyword, pageSize); },
        [this](ApiResult<std::vector<InventoryItem>> result) {
            inventoryLoading = false;
            inventoryLoadedOnce = true;
            if (result.ok) {
                inventory = std::move(result.value);
                inventoryStatus = "Loaded " + std::to_string(inventory.size()) + " items at " + timeNow();
            } else {
                inventoryError = result.message;
                notify("Inventory: " + result.message, ui::InfoKind::Error);
            }
        });
}

int App::findInventoryByCode(const std::string& rawCode) const {
    std::string code = lowercase(trim(rawCode));
    if (code.empty()) return -1;

    auto matchField = [&](auto getter) -> int {
        for (size_t i = 0; i < inventory.size(); ++i) {
            if (lowercase(trim(getter(inventory[i]))) == code) return (int)i;
        }
        return -1;
    };

    int idx = matchField([](const InventoryItem& it) { return it.sku; });
    if (idx >= 0) return idx;
    idx = matchField([](const InventoryItem& it) { return it.upc; });
    if (idx >= 0) return idx;
    idx = matchField([](const InventoryItem& it) { return it.name; });
    if (idx >= 0) return idx;
    // Finally, a partial name match (useful when typing instead of scanning).
    for (size_t i = 0; i < inventory.size(); ++i) {
        if (lowercase(inventory[i].name).find(code) != std::string::npos) return (int)i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Toasts
// ---------------------------------------------------------------------------

void App::notify(const std::string& message, ui::InfoKind kind) {
    double life = kind == ui::InfoKind::Error ? 9.0 : 5.0;
    toasts_.push_back({message, kind, ImGui::GetTime() + life});
    if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
}

void App::drawToasts() {
    double now = ImGui::GetTime();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [&](const Toast& t) { return t.expiresAt < now; }),
                  toasts_.end());
    if (toasts_.empty()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float width = 380.0f * dpiScale;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - width - 16.0f, vp->WorkPos.y + vp->WorkSize.y - 16.0f),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##toasts", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    for (size_t i = 0; i < toasts_.size(); ++i) {
        ImGui::PushID((int)i);
        bool open = true;
        ui::InfoBar(toasts_[i].kind, toasts_[i].message, &open);
        if (!open) toasts_[i].expiresAt = 0;
        ImGui::PopID();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void App::frame() {
    Async::pump();
    updateWindowChrome();
    maintainOAuthToken();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    drawNavigation();
    ImGui::SameLine(0.0f, 0.0f);

    const float barHeight = 56.0f * dpiScale;
    ImGui::BeginGroup();
    drawContent(ImGui::GetContentRegionAvail().y - barHeight);
    drawBottomSearchBar();
    ImGui::EndGroup();

    ImGui::End();
    drawToasts();
}

void App::openProduct(long long id) {
    pendingProductId = id;
    page = Page::Inventory;
}

void App::drawBottomSearchBar() {
    const Palette& P = Theme::get().palette();
    globalSearchChanged = false;
    globalSearchSubmitted = false;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surfaceAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * dpiScale, 10 * dpiScale));
    ImGui::BeginChild("##bottomBar", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // accent line on top of the bar
    ImVec2 mn = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(mn, ImVec2(mn.x + ImGui::GetWindowWidth(), mn.y), Theme::u32(P.accentLine), 1.0f);

    float clearW = globalSearch.empty() ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    float width = ImGui::GetContentRegionAvail().x - clearW;
    std::string before = globalSearch;
    bool submitted = ui::SearchField("globalSearch", globalSearch,
                                     "Search everything: products, invoices, tickets, customers...", width);
    if (globalSearch != before) {
        globalSearchChanged = true;
        if (!globalSearch.empty() && page != Page::Search) {
            pageBeforeSearch = page;
            page = Page::Search;
        }
    }
    if (submitted) {
        globalSearchSubmitted = true;
        if (!globalSearch.empty() && page != Page::Search) {
            pageBeforeSearch = page;
            page = Page::Search;
        }
    }
    if (!globalSearch.empty()) {
        ImGui::SameLine();
        if (ui::IconButton("clearGlobal", icons::Close, "Clear search")) {
            globalSearch.clear();
            globalSearchChanged = true;
            if (page == Page::Search) page = pageBeforeSearch == Page::Search ? Page::Inventory : pageBeforeSearch;
        }
    }
    ImGui::EndChild();
}

void App::drawNavigation() {
    const Palette& P = Theme::get().palette();
    const Fonts& F = Theme::get().fonts();
    float navWidth = kNavWidth * dpiScale;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surfaceAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * dpiScale, 16 * dpiScale));
    ImGui::BeginChild("##nav", ImVec2(navWidth, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Brand
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8 * dpiScale);
    ImGui::PushFont(F.subtitle);
    ImGui::TextColored(P.accent, "%s", icons::Repair);
    ImGui::SameLine(0, 10 * dpiScale);
    ImGui::TextColored(P.accent, "RepairDesk");
    ImGui::PopFont();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8 * dpiScale);
    ui::SecondaryText("Desktop");
    ui::VerticalSpace(12 * dpiScale);

    struct NavItem { Page page; const char* icon; const char* label; };
    const NavItem items[] = {
        {Page::Inventory, icons::Package, "Inventory"},
        {Page::POS, icons::Shop, "POS"},
        {Page::Tickets, icons::Repair, "Tickets"},
        {Page::Invoices, icons::Document, "Invoices"},
        {Page::Customers, icons::People, "Customers"},
        {Page::Communication, icons::Message, "Communication"},
    };

    auto navButton = [&](const NavItem& item) {
        bool selected = page == item.page;
        float h = 40.0f * dpiScale;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 size(ImGui::GetContentRegionAvail().x, h);
        ImGui::PushID(item.label);
        bool pressed = ImGui::InvisibleButton("##nav", size);
        ImGui::PopID();
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected || hovered) {
            ImVec4 bg = selected ? Theme::withAlpha(P.text, 0.08f) : Theme::withAlpha(P.text, 0.05f);
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::u32(bg), 6.0f * dpiScale);
        }
        if (selected) {
            // Windows 11 accent selection indicator
            float ih = 16.0f * dpiScale;
            dl->AddRectFilled(ImVec2(pos.x, pos.y + (h - ih) * 0.5f), ImVec2(pos.x + 3.0f * dpiScale, pos.y + (h + ih) * 0.5f),
                              Theme::u32(P.accent), 2.0f);
        }
        ImVec4 textColor = selected ? P.accent : P.text;
        ImGui::PushFont(F.iconsLarge);
        ImVec2 iconSize = ImGui::CalcTextSize(item.icon);
        dl->AddText(F.iconsLarge, F.iconsLarge->FontSize, ImVec2(pos.x + 14 * dpiScale, pos.y + (h - iconSize.y) * 0.5f),
                    Theme::u32(textColor), item.icon);
        ImGui::PopFont();
        ImVec2 labelSize = ImGui::CalcTextSize(item.label);
        dl->AddText(ImVec2(pos.x + 48 * dpiScale, pos.y + (h - labelSize.y) * 0.5f), Theme::u32(textColor), item.label);
        if (pressed) page = item.page;
        ImGui::Dummy(ImVec2(0, 2 * dpiScale));
    };

    for (const auto& item : items) navButton(item);

    // Settings pinned to the bottom
    float bottomY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - 42.0f * dpiScale;
    if (ImGui::GetCursorPosY() < bottomY) ImGui::SetCursorPosY(bottomY);
    navButton({Page::Settings, icons::Settings, "Settings"});

    ImGui::EndChild();
}

void App::drawContent(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * dpiScale, 20 * dpiScale));
    ImGui::BeginChild("##content", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    switch (page) {
        case Page::Inventory: pages::inventory(*this); break;
        case Page::POS: pages::pos(*this); break;
        case Page::Tickets: pages::tickets(*this); break;
        case Page::Invoices: pages::invoices(*this); break;
        case Page::Customers: pages::customers(*this); break;
        case Page::Communication: pages::communication(*this); break;
        case Page::Settings: pages::settings(*this); break;
        case Page::Search: pages::search(*this); break;
    }

    ImGui::EndChild();
}
