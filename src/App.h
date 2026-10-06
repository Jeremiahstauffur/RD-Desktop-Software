#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include "RepairDeskApi.h"
#include "Settings.h"
#include "Widgets.h"

enum class Page { Inventory, POS, Tickets, Invoices, Customers, Communication, Settings, Search };

class App {
public:
    App(HWND hwnd, float dpiScale);

    // Renders one frame of UI (between ImGui::NewFrame / ImGui::Render).
    void frame();

    // Settings / theme ---------------------------------------------------
    Settings settings;
    void applyTheme();                 // re-applies colours from `settings`
    void saveSettings();

    RepairDeskApi api() const { return RepairDeskApi(settings.baseUrl, settings.apiKey, settings.accessToken); }
    bool connected() const { return settings.hasAnyAuth(); }

    // OAuth2 ---------------------------------------------------------------
    // Opens the browser on RepairDesk's consent page, waits for the redirect
    // on the loopback redirect URI and exchanges the code for tokens.
    void startOAuthLogin();
    // Exchanges a manually pasted authorization code.
    void finishOAuthWithCode(const std::string& code);
    void refreshOAuthToken(bool force = false);
    void disconnectOAuth();
    bool oauthBusy = false;
    std::string oauthStatus;           // last login/refresh outcome shown in Settings
    bool oauthStatusOk = false;

    // Shared inventory cache (used by Inventory, Stock take and POS) ------
    std::vector<InventoryItem> inventory;
    bool inventoryLoading = false;
    bool inventoryLoadedOnce = false;
    std::string inventoryError;
    std::string inventoryStatus;       // e.g. "Loaded 312 items at 14:02"
    void refreshInventory(const std::string& keyword = "");

    // Finds an item by scanned code: SKU first, then UPC, then name.
    // Returns -1 when nothing matches.
    int findInventoryByCode(const std::string& code) const;

    // Feedback -------------------------------------------------------------
    void notify(const std::string& message, ui::InfoKind kind = ui::InfoKind::Info);

    // Navigation ----------------------------------------------------------
    Page page = Page::Inventory;
    bool stockTakeOpen = false;

    // Global search (bottom bar). Typing switches to the Search page.
    std::string globalSearch;
    bool globalSearchChanged = false;   // set for one frame when the text changes
    bool globalSearchSubmitted = false; // set for one frame when Enter is pressed
    Page pageBeforeSearch = Page::Inventory;

    // Opens the product page for the given inventory id (switches to Inventory).
    void openProduct(long long id);
    long long pendingProductId = 0;     // consumed by the inventory page

    float dpiScale = 1.0f;
    HWND hwnd() const { return hwnd_; }

private:
    struct Toast {
        std::string message;
        ui::InfoKind kind;
        double expiresAt;
    };

    void drawNavigation();
    void drawContent(float height);
    void drawBottomSearchBar();
    void drawToasts();
    void updateWindowChrome();
    void applyTokens(const OAuthTokens& tokens);
    void maintainOAuthToken();
    std::string oauthState_;
    double nextRefreshCheck_ = 0.0;

    HWND hwnd_;
    std::vector<Toast> toasts_;
    bool themeDirty_ = true;
};
