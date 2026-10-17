#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include "RepairDeskApi.h"
#include "Settings.h"
#include "Updater.h"
#include "Widgets.h"

enum class Page { Inventory, POS, Tickets, Invoices, Customers, Communication, Settings, Search };

class App {
public:
    App(HWND hwnd, float dpiScale);
    ~App();

    // Renders one frame of UI (between ImGui::NewFrame / ImGui::Render).
    void frame();

    // Settings / theme ---------------------------------------------------
    Settings settings;
    void applyTheme();                 // re-applies colours from `settings`
    void saveSettings();
    // Font size changed: the main loop rebuilds the atlas before the next frame.
    void requestFontRebuild() { fontsDirty_ = true; }
    bool takeFontRebuildRequest() { bool d = fontsDirty_; fontsDirty_ = false; return d; }

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

    // Top progress bar for writes to RepairDesk (product saves, bulk actions, stock take
    // submit, invoices, SMS). Begin a job with the number of requests it will make, let the
    // worker thread bump `done` after each one and finish the job from the continuation.
    // While a job runs the bar along the top edge fills like the inventory refresh does.
    struct WriteJob {
        std::atomic<size_t> done{0};
        size_t total = 1;
    };
    std::shared_ptr<WriteJob> beginWriteJob(size_t total = 1);
    void finishWriteJob(const std::shared_ptr<WriteJob>& job);

    // Finds an item by scanned code: the first exact (case-insensitive) SKU match, otherwise
    // the first exact UPC match, otherwise the first exact name match. Returns -1 when
    // nothing matches; partial matches are never returned.
    int findInventoryByCode(const std::string& code) const;
    // Indices of items where every space-separated term of `text` occurs in some field
    // (same rule as the inventory table search). At most `limit` results (0 = all).
    std::vector<int> searchInventory(const std::string& text, size_t limit = 0) const;
    // Distinct non-empty text values of a flattened field across the loaded inventory,
    // sorted case-insensitively (e.g. every category in use).
    std::vector<std::string> inventoryValues(const std::string& key) const;

    // Reference data (manufacturers, devices, suppliers, tax classes, conditions,
    // physical locations). Loaded with the first inventory refresh; shared with
    // the API clients so names typed in the UI can be converted to ids.
    std::shared_ptr<const Lookups> lookups = std::make_shared<Lookups>();
    bool lookupsLoading = false;
    void refreshLookups();
    // Adds freshly created references (brand/model) to the cache.
    void mergeLookups(const Lookups& more);

    // Updates (GitHub repository, see Updater.h) ---------------------------
    enum class UpdateState { Idle, Checking, UpToDate, Available, Downloading, Installing, Failed };
    UpdateState updateState = UpdateState::Idle;
    updater::UpdateInfo updateInfo;    // newest published release (valid from Available on)
    std::string updateStatus;          // outcome of the last check, shown in Settings
    std::string updateError;           // last download/launch failure (empty = none)
    std::atomic<unsigned long long> updateReceived{0};   // bytes downloaded so far (worker thread)
    bool updateDismissed = false;      // "Later" pressed: no start-up toast again this session
    bool updateAvailable() const { return updateState == UpdateState::Available; }
    // `manual` = button press: failures are reported with a toast as well.
    void checkForUpdates(bool manual);
    void startUpdate();                // download, verify and launch the installer
    void cancelUpdate();               // aborts a running download

    // Feedback -------------------------------------------------------------
    void notify(const std::string& message, ui::InfoKind kind = ui::InfoKind::Info);

    // Navigation ----------------------------------------------------------
    Page page = Page::Inventory;
    bool stockTakeOpen = false;
    bool stockTakeBulkEdit = false;     // bulk-edit panel next to the stock take sidebar

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
    void drawLoadProgressBar();        // thin accent bar along the top edge (refresh, writes, update download)
    void restartProgressBar();         // starts the bar from the left unless another source is showing
    void holdProgressBar(float solid, float dotted);   // shows the finished bar briefly, then fades
    void drawToasts();
    void updateWindowChrome();
    void applyTokens(const OAuthTokens& tokens);
    void maintainOAuthToken();
    void pollInstaller();              // watches the launched Setup process
    std::string oauthState_;
    double nextRefreshCheck_ = 0.0;
    std::shared_ptr<std::atomic<bool>> updateCancel_;
    HANDLE updateProcess_ = nullptr;

    // Animated state of the top progress bar (fractions of the window width).
    float progressSolid_ = 0.0f;
    float progressDotted_ = 0.0f;
    double progressHideAt_ = 0.0;      // keep the finished bar visible until this time
    float progressHoldSolid_ = 1.0f;   // what the finished bar shows during that time
    float progressHoldDotted_ = 0.0f;
    std::vector<std::shared_ptr<WriteJob>> writeJobs_;   // RepairDesk writes in flight

    HWND hwnd_;
    std::vector<Toast> toasts_;
    bool themeDirty_ = true;
    bool fontsDirty_ = false;
    double layoutSaveAt_ = 0.0;        // debounce for persisting table layouts
};
