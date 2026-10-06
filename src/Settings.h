#pragma once

#include <string>

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
    int inventoryPageSize = 1000;

    // Directory used for settings.json and imgui layout persistence.
    static std::string dataDirectory();
    static std::string settingsPath();
    static std::string layoutPath();

    void load();
    void save() const;
};
