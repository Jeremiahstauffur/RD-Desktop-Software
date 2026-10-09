#include "Settings.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
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

double Settings::suggestedPrice(double cost) const {
    double v = cost * priceMultiplier;
    if (priceRoundTo > 0.0) v = std::round(v / priceRoundTo) * priceRoundTo;
    v -= priceSubtract;
    // Snap to cents to hide binary floating point noise (e.g. 9.9500000001).
    v = std::round(v * 100.0) / 100.0;
    return v < 0.0 ? 0.0 : v;
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
        fontSize = std::clamp(j.value("fontSize", fontSize), kMinFontSize, kMaxFontSize);
        inventoryPageSize = j.value("inventoryPageSize", inventoryPageSize);
        if (inventoryPageSize == 1000) inventoryPageSize = 0; // migrate legacy 1000 default to unlimited
        inventoryLastCount = std::max(0, j.value("inventoryLastCount", inventoryLastCount));
        checkUpdatesOnStartup = j.value("checkUpdatesOnStartup", checkUpdatesOnStartup);
        priceMultiplier = j.value("priceMultiplier", priceMultiplier);
        priceRoundTo = j.value("priceRoundTo", priceRoundTo);
        priceSubtract = j.value("priceSubtract", priceSubtract);
        if (j.contains("accent") && j["accent"].is_array() && j["accent"].size() == 3) {
            for (int i = 0; i < 3; ++i) accent[i] = j["accent"][i].get<float>();
        }
        tableLayouts.clear();
        if (j.contains("tableLayouts") && j["tableLayouts"].is_object()) {
            for (const auto& [tableId, lj] : j["tableLayouts"].items()) {
                if (!lj.is_object()) continue;
                TableLayout layout;
                layout.refScale = lj.value("refScale", 0.0f);
                layout.sortKey = lj.value("sortKey", "");
                layout.sortAscending = lj.value("sortAscending", true);
                for (const json& cj : lj.value("columns", json::array())) {
                    if (!cj.is_object()) continue;
                    TableLayout::Column c;
                    c.key = cj.value("key", "");
                    c.width = cj.value("width", 0.0f);
                    c.visible = cj.value("visible", true);
                    if (!c.key.empty()) layout.columns.push_back(std::move(c));
                }
                if (!layout.columns.empty()) tableLayouts[tableId] = std::move(layout);
            }
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
    j["fontSize"] = fontSize;
    j["inventoryPageSize"] = inventoryPageSize;
    j["inventoryLastCount"] = inventoryLastCount;
    j["checkUpdatesOnStartup"] = checkUpdatesOnStartup;
    j["priceMultiplier"] = priceMultiplier;
    j["priceRoundTo"] = priceRoundTo;
    j["priceSubtract"] = priceSubtract;
    j["accent"] = {accent[0], accent[1], accent[2]};
    json layouts = json::object();
    for (const auto& [tableId, layout] : tableLayouts) {
        json lj;
        lj["refScale"] = layout.refScale;
        lj["sortKey"] = layout.sortKey;
        lj["sortAscending"] = layout.sortAscending;
        json cols = json::array();
        for (const TableLayout::Column& c : layout.columns) {
            cols.push_back({{"key", c.key}, {"width", c.width}, {"visible", c.visible}});
        }
        lj["columns"] = std::move(cols);
        layouts[tableId] = std::move(lj);
    }
    j["tableLayouts"] = std::move(layouts);
    std::ofstream out(settingsPath());
    out << j.dump(2);
    layoutDirty = false;
}
