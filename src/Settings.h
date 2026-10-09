#pragma once

#include <map>
#include <string>
#include <vector>

// Persisted layout of one DataTable: the columns in display order, each with its
// width and visibility. Keyed by field name so the layout survives columns being
// added or removed by the API or by newer app versions.
struct TableLayout {
    struct Column {
        std::string key;
        float width = 0.0f;
        bool visible = true;
    };
    std::vector<Column> columns;
    float refScale = 0.0f;   // font size the widths were recorded at (0 = unknown)
    std::string sortKey;     // sorted column (empty = unsorted)
    bool sortAscending = true;
};

struct Settings {
    // Authentication. RepairDesk supports either an API key (sent as the
    // api_key query parameter) or OAuth2 (Client ID + Client Secret ->
    // Bearer access token). When an OAuth2 access token is present it is used.
    std::string apiKey;
    std::string clientId;
    std::string clientSecret;
    std::string redirectUri = "http://localhost:8731/callback";
    std::string accessToken;
    std::string refreshToken;
    long long tokenExpiresAt = 0;      // unix time (seconds)

    std::string baseUrl = "https://api.repairdesk.co/api/web/v1";

    bool hasOAuthClient() const { return !clientId.empty() && !clientSecret.empty(); }
    bool hasOAuthToken() const { return !accessToken.empty(); }
    bool hasAnyAuth() const { return hasOAuthToken() || !apiKey.empty(); }
    bool darkTheme = true;
    float accent[3] = {0.0f, 0.47f, 0.83f};   // Windows 11 default blue (#0078D4)
    int fontSize = kDefaultFontSize;           // body font size in logical pixels (before DPI scaling)
    static constexpr int kDefaultFontSize = 15;
    static constexpr int kMinFontSize = 11;
    static constexpr int kMaxFontSize = 24;
    float fontScale() const { return (float)fontSize / (float)kDefaultFontSize; }
    int inventoryPageSize = 0;                 // 0 = load all items (unlimited)
    // Item count of the last successful inventory load; the top progress bar uses
    // it as its 100% mark on the first refresh after start-up.
    int inventoryLastCount = 0;
    // Look for a newer release on GitHub when the app starts (result shown in Settings).
    bool checkUpdatesOnStartup = true;

    // Column order / widths / visibility per table id (see DataTable).
    std::map<std::string, TableLayout> tableLayouts;
    // Set by DataTable when a layout changed; App saves a little later (not persisted).
    mutable bool layoutDirty = false;

    // Suggested-price formula shown next to the price box on the product page:
    //   round(cost * multiplier to the nearest roundTo) - subtract
    double priceMultiplier = 2.0;
    double priceRoundTo = 0.20;
    double priceSubtract = 0.05;
    double suggestedPrice(double cost) const;

    // Directory used for settings.json and imgui layout persistence.
    static std::string dataDirectory();
    static std::string settingsPath();
    static std::string layoutPath();

    void load();
    void save() const;
};
