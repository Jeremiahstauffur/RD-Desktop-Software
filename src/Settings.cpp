#include "Settings.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string Settings::dataDirectory() {
    static std::string dir;
    if (!dir.empty()) return dir;

    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wpath))) {
        std::filesystem::path p(wpath);
        CoTaskMemFree(wpath);
        p /= "RD Desktop Software";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        dir = p.string();
    } else {
        dir = ".";
    }
    return dir;
}

std::string Settings::settingsPath() {
    return (std::filesystem::path(dataDirectory()) / "settings.json").string();
}

std::string Settings::layoutPath() {
    return (std::filesystem::path(dataDirectory()) / "layout.ini").string();
}

void Settings::load() {
    std::ifstream in(settingsPath());
    if (!in) return;
    try {
        json j = json::parse(in);
        apiKey = j.value("apiKey", apiKey);
        clientId = j.value("clientId", clientId);
        clientSecret = j.value("clientSecret", clientSecret);
        redirectUri = j.value("redirectUri", redirectUri);
        accessToken = j.value("accessToken", accessToken);
        refreshToken = j.value("refreshToken", refreshToken);
        tokenExpiresAt = j.value("tokenExpiresAt", tokenExpiresAt);
        baseUrl = j.value("baseUrl", baseUrl);
        darkTheme = j.value("darkTheme", darkTheme);
        inventoryPageSize = j.value("inventoryPageSize", inventoryPageSize);
        if (j.contains("accent") && j["accent"].is_array() && j["accent"].size() == 3) {
            for (int i = 0; i < 3; ++i) accent[i] = j["accent"][i].get<float>();
        }
    } catch (...) {
        // Corrupt settings file: keep defaults.
    }
}

void Settings::save() const {
    json j;
    j["apiKey"] = apiKey;
    j["clientId"] = clientId;
    j["clientSecret"] = clientSecret;
    j["redirectUri"] = redirectUri;
    j["accessToken"] = accessToken;
    j["refreshToken"] = refreshToken;
    j["tokenExpiresAt"] = tokenExpiresAt;
    j["baseUrl"] = baseUrl;
    j["darkTheme"] = darkTheme;
    j["inventoryPageSize"] = inventoryPageSize;
    j["accent"] = {accent[0], accent[1], accent[2]};
    std::ofstream out(settingsPath());
    out << j.dump(2);
}
