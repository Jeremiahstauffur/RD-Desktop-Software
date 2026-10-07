#pragma once

#include <atomic>
#include <memory>
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

    // API client carrying the current credentials and the cached reference data.
    RepairDeskApi api() const {
        RepairDeskApi c(settings.baseUrl, settings.apiKey, settings.accessToken);
        c.setLookups(lookups);
        return c;
    }
    bool connected() const { return settings.hasAnyAuth(); }

    // OAuth2 ---------------------------------------------------------------
    // Opens the browser on RepairDesk's consent page, waits for the redirect
    // on the loopback redirect URI and exchanges the code for tokens.
    void startOAuthLogin();
    // Exchanges a manually pasted authorization code (or full redirect URL).
    void finishOAuthWithCode(const std::string& codeOrUrl);
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
    // Refresh progress: items fetched so far (written from the worker thread) and the
    // item count of the previous load, which the top progress bar uses as its 100% mark.
    std::atomic<size_t> inventoryLoadProgress{0};
    size_t inventoryPreviousTotal = 0;
    void refreshInventory(const std::string& keyword = "");
    // Replaces (or appends) a single cached item without reloading the whole list.
    void replaceInventoryItem(const InventoryItem& item);
    // Incremented whenever cached items change in place; pages compare it to re-sort/filter.
    unsigned inventoryRevision = 0;

    // Finds an item by scanned code: SKU first, then UPC, then name.
    // Returns -1 when nothing matches.
    int findInventoryByCode(const std::string& code) const;

    // Reference data (manufacturers, devices, suppliers, tax classes, conditions,
    // physical locations). Loaded with the first inventory refresh; shared with
    // the API clients so names typed in the UI can be converted to ids.
    std::shared_ptr<const Lookups> lookups = std::make_shared<Lookups>();
    bool lookupsLoading = false;
    void refreshLookups();
    // Adds freshly created references (brand/model) to the cache.
    void mergeLookups(const Lookups& more);

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
    void drawLoadProgressBar();        // thin accent bar along the top edge while inventory refreshes
    void drawToasts();
    void updateWindowChrome();
    void applyTokens(const OAuthTokens& tokens);
    void maintainOAuthToken();
    std::string oauthState_;
    double nextRefreshCheck_ = 0.0;

    // Animated state of the top progress bar (fractions of the window width).
    float progressSolid_ = 0.0f;
    float progressDotted_ = 0.0f;
    double progressHideAt_ = 0.0;      // keep the finished bar visible until this time

    HWND hwnd_;
    std::vector<Toast> toasts_;
    bool themeDirty_ = true;
};
