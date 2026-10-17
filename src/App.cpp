#include "App.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <set>

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "Async.h"
#include "DataTable.h"
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
    DataTable::bindSettings(&settings);   // tables restore/persist their column layouts
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
    updater::cleanupDownloads();   // installer of a previous update, if any
    if (settings.checkUpdatesOnStartup) checkForUpdates(false);
}

App::~App() {
    if (updateProcess_) CloseHandle(updateProcess_);
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

// Accepts either a bare authorization code or the full redirect URL
// ("http://localhost:8731/callback?code=...&state=...") pasted by the user.
static std::string extractAuthCode(const std::string& raw) {
    std::string input = trim(raw);
    if (input.find("code=") == std::string::npos) return input;
    size_t q = input.find('?');
    std::string query = q == std::string::npos ? input : input.substr(q + 1);
    size_t hash = query.find('#');
    std::string fragment = hash == std::string::npos ? "" : query.substr(hash + 1);
    if (hash != std::string::npos) query.erase(hash);
    std::string code = http::queryParam(query, "code");
    if (code.empty()) code = http::queryParam(fragment, "code");
    return code.empty() ? input : code;
}

// Adds a troubleshooting hint for the most common token-exchange rejections.
static std::string tokenExchangeHint(const std::string& message, const std::string& redirectUri) {
    std::string lower = lowercase(message);
    if (lower.find("redirect") != std::string::npos || lower.find("invalid_grant") != std::string::npos ||
        lower.find("bad request") != std::string::npos || lower.find("invalid_request") != std::string::npos) {
        return " Check that the redirect URI registered in RepairDesk is exactly \"" + redirectUri +
               "\" and that the Client ID/Secret belong to that app, then sign in again (codes are single-use).";
    }
    if (lower.find("invalid_client") != std::string::npos) {
        return " Check the Client ID and Client Secret.";
    }
    return "";
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
                oauthStatus = "Token exchange failed: " + out.tokens.message +
                              tokenExchangeHint(out.tokens.message, settings.redirectUri);
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

void App::finishOAuthWithCode(const std::string& codeOrUrl) {
    const std::string code = extractAuthCode(codeOrUrl);
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
                       oauthStatus = "Token exchange failed: " + r.message + tokenExchangeHint(r.message, settings.redirectUri);
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
// Updates
// ---------------------------------------------------------------------------

void App::checkForUpdates(bool manual) {
    if (updateState == UpdateState::Checking || updateState == UpdateState::Downloading ||
        updateState == UpdateState::Installing) {
        return;
    }
    updateState = UpdateState::Checking;
    updateStatus = "Checking for updates...";
    updateError.clear();
    Async::run([] { return updater::check(); },
               [this, manual](updater::CheckResult r) {
                   if (updateState != UpdateState::Checking) return;
                   if (!r.ok) {
                       updateState = UpdateState::Failed;
                       updateStatus = "Could not check for updates: " + r.error;
                       if (manual) notify(updateStatus, ui::InfoKind::Warning);
                       return;
                   }
                   if (r.updateAvailable) {
                       updateInfo = r.latest;
                       updateState = UpdateState::Available;
                       updateStatus = "Version " + updateInfo.version + " is available.";
                       if (!manual && !updateDismissed) {
                           notify("RD Desktop Software " + updateInfo.version +
                                      " is available. Open Settings to install the update.",
                                  ui::InfoKind::Info);
                       }
                   } else {
                       updateState = UpdateState::UpToDate;
                       if (updater::compareVersions(updater::kCurrentVersion, r.latest.version) > 0) {
                           updateStatus = "You are running " + std::string(updater::kCurrentVersion) +
                                          ", which is newer than the published " + r.latest.version + ".";
                       } else {
                           updateStatus = "You're up to date (checked at " + timeNow() + ").";
                       }
                       if (manual) notify(updateStatus, ui::InfoKind::Success);
                   }
               });
}

void App::startUpdate() {
    if (updateState != UpdateState::Available) return;
    updateState = UpdateState::Downloading;
    updateError.clear();
    updateReceived.store(0);
    restartProgressBar();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    updateCancel_ = cancel;
    const updater::UpdateInfo info = updateInfo;
    std::atomic<unsigned long long>* received = &updateReceived;
    Async::run([info, received, cancel] { return updater::download(info, received, cancel.get()); },
               [this, cancel](updater::DownloadResult r) {
                   if (updateState != UpdateState::Downloading || updateCancel_ != cancel) return;
                   updateCancel_.reset();
                   if (cancel->load()) {
                       updateState = UpdateState::Available;
                       progressHideAt_ = 0.0;   // fade the top bar out without filling it
                       return;
                   }
                   if (!r.ok) {
                       updateState = UpdateState::Available;
                       updateError = r.error;
                       notify("Update: " + r.error, ui::InfoKind::Error);
                       progressHideAt_ = 0.0;
                       return;
                   }
                   holdProgressBar(1.0f, 0.0f);
                   saveSettings();   // everything Setup must preserve is on disk before it runs
                   std::string err;
                   updateProcess_ = updater::launchInstaller(r.path, err);
                   if (!updateProcess_) {
                       updateState = UpdateState::Available;
                       updateError = "Could not start the installer: " + err + ".";
                       notify(updateError, ui::InfoKind::Error);
                       return;
                   }
                   updateState = UpdateState::Installing;
                   updateStatus = "Installing version " + updateInfo.version + "...";
               });
}

void App::cancelUpdate() {
    if (updateState != UpdateState::Downloading || !updateCancel_) return;
    updateCancel_->store(true);
}

// Setup closes this process itself while it replaces the files and starts the new
// version afterwards, so normally this only ever sees the installer bail out early
// (UAC declined, cancelled, failed) - in which case the app simply keeps running.
void App::pollInstaller() {
    if (updateState != UpdateState::Installing || !updateProcess_) return;
    if (WaitForSingleObject(updateProcess_, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(updateProcess_, &code);
    CloseHandle(updateProcess_);
    updateProcess_ = nullptr;
    if (code == 0) {
        updateState = UpdateState::UpToDate;
        updateStatus = "Version " + updateInfo.version + " was installed. Restart the app to use it.";
        notify(updateStatus, ui::InfoKind::Success);
        return;
    }
    updateState = UpdateState::Available;
    // Inno Setup exit codes: 2/5 = cancelled by the user, 1223 = Windows cancelled (UAC prompt declined).
    if (code == 2 || code == 5 || code == 1223) {
        updateError = "The update was cancelled.";
    } else {
        updateError = "Setup did not complete (exit code " + std::to_string(code) +
                      "). See setup.log in %LOCALAPPDATA%\\RD Desktop Software\\updates.";
    }
    notify(updateError, ui::InfoKind::Warning);
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
    if (!lookups->loaded) refreshLookups();
    inventoryLoading = true;
    inventoryError.clear();
    // 100% mark of the progress bar: the current item count, or on the first load after
    // start-up the count persisted from the previous session.
    inventoryPreviousTotal = inventory.empty() ? (size_t)std::max(0, settings.inventoryLastCount) : inventory.size();
    inventoryLoadProgress.store(0);
    restartProgressBar();
    RepairDeskApi client = api();
    int maxItems = settings.inventoryPageSize;
    std::atomic<size_t>* progress = &inventoryLoadProgress;
    Async::run(
        [client, keyword, maxItems, progress] {
            return client.listInventory(keyword, maxItems, 100, [progress](size_t loaded) { progress->store(loaded); });
        },
        [this, keyword](ApiResult<std::vector<InventoryItem>> result) {
            inventoryLoading = false;
            inventoryLoadedOnce = true;
            if (result.ok) {
                inventory = std::move(result.value);
                inventoryLoadProgress.store(inventory.size());
                // Finished: a full bar, or the previous total plus the extra items as a dotted tail.
                const size_t loaded = inventory.size(), previous = inventoryPreviousTotal;
                if (previous == 0 || loaded <= previous) holdProgressBar(1.0f, 0.0f);
                else holdProgressBar((float)previous / (float)loaded, 1.0f - (float)previous / (float)loaded);
                inventoryStatus = "Loaded " + std::to_string(inventory.size()) + " items at " + timeNow();
                if (keyword.empty() && settings.inventoryLastCount != (int)inventory.size()) {
                    settings.inventoryLastCount = (int)inventory.size();
                    saveSettings();
                }
            } else {
                inventoryError = result.message;
                notify("Inventory: " + result.message, ui::InfoKind::Error);
                progressHideAt_ = 0.0;
            }
        });
}

std::shared_ptr<App::WriteJob> App::beginWriteJob(size_t total) {
    auto job = std::make_shared<WriteJob>();
    job->total = std::max<size_t>(1, total);
    restartProgressBar();
    writeJobs_.push_back(job);
    return job;
}

void App::finishWriteJob(const std::shared_ptr<WriteJob>& job) {
    for (size_t i = 0; i < writeJobs_.size(); ++i) {
        if (writeJobs_[i] == job) {
            writeJobs_.erase(writeJobs_.begin() + i);
            break;
        }
    }
    if (writeJobs_.empty()) holdProgressBar(1.0f, 0.0f);
}

void App::refreshLookups() {
    if (lookupsLoading || !connected()) return;
    lookupsLoading = true;
    RepairDeskApi client = api();
    Async::run([client] { return client.loadLookups(); },
               [this](ApiResult<Lookups> result) {
                   lookupsLoading = false;
                   if (result.ok) {
                       lookups = std::make_shared<Lookups>(std::move(result.value));
                   }
                   if (result.message != "OK") notify("Reference data: " + result.message, ui::InfoKind::Warning);
               });
}

void App::mergeLookups(const Lookups& more) {
    auto merged = std::make_shared<Lookups>(*lookups);
    for (const NamedId& m : more.manufacturers) {
        if (!merged->findManufacturer(std::to_string(m.id))) merged->manufacturers.push_back(m);
    }
    for (const DeviceInfo& d : more.devices) {
        if (!merged->findDevice(std::to_string(d.id))) merged->devices.push_back(d);
    }
    merged->loaded = true;
    lookups = merged;
}

void App::replaceInventoryItem(const InventoryItem& item) {
    bool found = false;
    for (InventoryItem& it : inventory) {
        if (it.id == item.id) {
            it = item;
            found = true;
            break;
        }
    }
    if (!found) inventory.push_back(item);
    inventoryRevision++;
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
    return matchField([](const InventoryItem& it) { return it.name; });
}

std::vector<int> App::searchInventory(const std::string& text, size_t limit) const {
    std::vector<std::string> terms;
    std::string cur;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            if (!cur.empty()) { terms.push_back(cur); cur.clear(); }
        } else {
            cur.push_back((char)std::tolower(c));
        }
    }
    if (!cur.empty()) terms.push_back(cur);

    std::vector<int> out;
    if (terms.empty()) return out;
    auto containsNoCase = [](const std::string& hay, const std::string& needleLower) {
        if (hay.size() < needleLower.size()) return false;
        auto it = std::search(hay.begin(), hay.end(), needleLower.begin(), needleLower.end(),
                              [](unsigned char a, unsigned char b) { return (char)std::tolower(a) == (char)b; });
        return it != hay.end();
    };
    for (size_t i = 0; i < inventory.size(); ++i) {
        bool all = true;
        for (const std::string& term : terms) {
            bool found = false;
            for (const auto& [key, value] : inventory[i].fields) {
                if (containsNoCase(value, term)) { found = true; break; }
            }
            if (!found) { all = false; break; }
        }
        if (!all) continue;
        out.push_back((int)i);
        if (limit && out.size() >= limit) break;
    }
    return out;
}

std::vector<std::string> App::inventoryValues(const std::string& key) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const InventoryItem& it : inventory) {
        auto f = it.fields.find(key);
        if (f == it.fields.end()) continue;
        const std::string value = trim(f->second);
        if (value.empty()) continue;
        if (seen.insert(lowercase(value)).second) out.push_back(value);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lowercase(a) < lowercase(b); });
    return out;
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
    pollInstaller();

    // Column layouts change while the user drags; persist them a moment after the last change.
    if (settings.layoutDirty) {
        const double now = ImGui::GetTime();
        if (layoutSaveAt_ <= 0.0) layoutSaveAt_ = now + 1.5;
        else if (now >= layoutSaveAt_) { saveSettings(); layoutSaveAt_ = 0.0; }
    } else {
        layoutSaveAt_ = 0.0;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // No NoSavedSettings here: tables inherit that flag from their root window and would
    // then ignore the column layouts DataTable restores through ImGui's table settings.
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
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
    drawLoadProgressBar();
    drawToasts();
}

void App::restartProgressBar() {
    // Another source is already showing the bar: just let it carry on.
    if (inventoryLoading || !writeJobs_.empty() || updateState == UpdateState::Downloading) return;
    progressSolid_ = 0.0f;
    progressDotted_ = 0.0f;
    progressHideAt_ = 0.0;
}

void App::holdProgressBar(float solid, float dotted) {
    progressHoldSolid_ = solid;
    progressHoldDotted_ = dotted;
    progressHideAt_ = ImGui::GetTime() + 0.8;
}

// Thin accent bar along the top edge of the window while something is in flight:
//  - inventory refresh: the previous item count is the 100% mark, the solid part travels
//    left to right and only reaches the right edge once at least as many items as before
//    have arrived; items beyond the previous total are shown as a dotted segment on the
//    right, sized in proportion to the solid (previous total) segment;
//  - writes to RepairDesk (WriteJob): requests completed / requests to make;
//  - update download: bytes received / installer size.
// Until the first unit arrives (or when no total is known) a segment slides across.
void App::drawLoadProgressBar() {
    const double now = ImGui::GetTime();
    const bool downloading = updateState == UpdateState::Downloading;
    const bool busy = inventoryLoading || !writeJobs_.empty() || downloading;
    const bool active = busy || now < progressHideAt_;
    if (!active && progressSolid_ <= 0.0f && progressDotted_ <= 0.0f) return;

    // Target fractions of the full width.
    float targetSolid = 0.0f, targetDotted = 0.0f;
    bool indeterminate = false;
    if (inventoryLoading) {
        const size_t loaded = inventoryLoadProgress.load();
        const size_t previous = inventoryPreviousTotal;
        if (previous == 0) {
            indeterminate = true;                  // first load ever: no reference total yet
        } else if (loaded <= previous) {
            targetSolid = (float)loaded / (float)previous;
        } else {
            targetSolid = (float)previous / (float)loaded;
            targetDotted = 1.0f - targetSolid;
        }
    } else if (!writeJobs_.empty()) {
        size_t done = 0, total = 0;
        for (const auto& job : writeJobs_) {
            done += std::min(job->done.load(), job->total);
            total += job->total;
        }
        if (done == 0) indeterminate = true;       // single saves slide until the request returns
        else targetSolid = (float)done / (float)total;
    } else if (downloading) {
        const unsigned long long received = updateReceived.load(), size = updateInfo.size;
        if (size == 0 || received == 0) indeterminate = true;
        else targetSolid = (float)std::min(1.0, (double)received / (double)size);
    } else if (active) {
        targetSolid = progressHoldSolid_;          // finished: show the final state for a moment
        targetDotted = progressHoldDotted_;
    }

    // Ease towards the target so page arrivals glide instead of jumping.
    const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 10.0f);
    progressSolid_ += (targetSolid - progressSolid_) * k;
    progressDotted_ += (targetDotted - progressDotted_) * k;
    if (!active && progressSolid_ < 0.002f && progressDotted_ < 0.002f) {
        progressSolid_ = progressDotted_ = 0.0f;
        return;
    }

    const Palette& P = Theme::get().palette();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float thickness = 3.0f * dpiScale;
    const float x0 = vp->WorkPos.x, y0 = vp->WorkPos.y, width = vp->WorkSize.x;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 accent = Theme::u32(P.accent);
    const ImU32 track = Theme::u32(Theme::withAlpha(P.accent, 0.18f));

    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + width, y0 + thickness), track);

    if (indeterminate) {
        // Sliding segment until the first page tells us how many items there are.
        const float segW = width * 0.25f;
        const float t = (float)std::fmod(now * 0.6, 1.0);
        const float sx = x0 - segW + (width + segW) * t;
        const float a = std::max(x0, sx), b = std::min(x0 + width, sx + segW);
        if (b > a) dl->AddRectFilled(ImVec2(a, y0), ImVec2(b, y0 + thickness), accent);
        return;
    }

    const float solidEnd = x0 + width * progressSolid_;
    if (progressSolid_ > 0.0f) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(solidEnd, y0 + thickness), accent);

    if (progressDotted_ > 0.0f) {
        const float dash = 6.0f * dpiScale, gap = 4.0f * dpiScale;
        const float end = std::min(x0 + width, solidEnd + width * progressDotted_);
        for (float x = solidEnd + gap; x < end; x += dash + gap) {
            dl->AddRectFilled(ImVec2(x, y0), ImVec2(std::min(x + dash, end), y0 + thickness), accent);
        }
    }
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
        if (item.page == Page::Settings && updateAvailable()) {
            // accent dot: an update is waiting on the Settings page
            dl->AddCircleFilled(ImVec2(pos.x + size.x - 16 * dpiScale, pos.y + h * 0.5f), 4.0f * dpiScale, Theme::u32(P.accent));
        }
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
