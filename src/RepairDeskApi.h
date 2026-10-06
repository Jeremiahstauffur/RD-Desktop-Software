#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Http.h"

using json = nlohmann::json;

// A single-level view of an arbitrary JSON record: nested objects are
// flattened to "parent.child" keys, arrays are summarised. Used by the
// generic data table so that *every* field RepairDesk returns can be shown.
using FlatRecord = std::map<std::string, std::string>;
FlatRecord flattenJson(const json& j, int maxDepth = 2);

// Formats a JSON scalar as display text.
std::string jsonToText(const json& v);

// ASCII lower-case copy (used for case-insensitive matching everywhere).
std::string toLower(std::string s);

// True when any value (or key) in the flattened record contains `needleLower`.
bool recordMatches(const FlatRecord& record, const std::string& needleLower);

struct InventoryItem {
    long long id = 0;
    std::string name;
    std::string sku;
    std::string upc;
    std::string category;
    std::string manufacturer;
    std::string device;
    double price = 0.0;
    double costPrice = 0.0;
    int inStock = 0;
    json raw;              // untouched record from the API
    FlatRecord fields;     // flattened record for the table

    static InventoryItem fromJson(const json& j);
};

template <typename T>
struct ApiResult {
    bool ok = false;
    int status = 0;
    std::string message;   // human readable error/success message
    T value{};
};

// Result of an OAuth2 token request.
struct OAuthTokens {
    std::string accessToken;
    std::string refreshToken;
    long long expiresAt = 0;   // unix time
};

class RepairDeskApi {
public:
    // `apiKey` is used as the api_key query parameter unless `accessToken` is
    // set, in which case requests carry "Authorization: Bearer <token>".
    RepairDeskApi(std::string baseUrl, std::string apiKey, std::string accessToken = {});

    bool hasKey() const { return !apiKey_.empty() || !accessToken_.empty(); }

    // OAuth2 ------------------------------------------------------------
    static std::string authorizeUrl(const std::string& baseUrl, const std::string& clientId,
                                    const std::string& redirectUri, const std::string& state);
    static ApiResult<OAuthTokens> exchangeCode(const std::string& baseUrl, const std::string& clientId,
                                               const std::string& clientSecret, const std::string& code,
                                               const std::string& redirectUri);
    static ApiResult<OAuthTokens> refreshAccessToken(const std::string& baseUrl, const std::string& clientId,
                                                     const std::string& clientSecret, const std::string& refreshToken);
    static ApiResult<json> revokeToken(const std::string& baseUrl, const std::string& clientId,
                                       const std::string& clientSecret, const std::string& token);

    // Inventory --------------------------------------------------------
    ApiResult<std::vector<InventoryItem>> listInventory(const std::string& keyword, int pageSize) const;
    ApiResult<json> addInventoryItem(const json& fields) const;   // POST /inventory/addnewitem
    ApiResult<json> updateInventoryItem(long long id, const json& fields) const;
    ApiResult<json> deleteInventoryItem(long long id) const;

    // Tickets / Invoices -----------------------------------------------
    ApiResult<std::vector<json>> listTickets(const std::string& keyword, int page, int pageSize) const;
    ApiResult<std::vector<json>> listInvoices(const std::string& keyword, int page, int pageSize) const;
    ApiResult<json> createInvoice(const json& body) const;

    // Customers --------------------------------------------------------
    ApiResult<std::vector<json>> listCustomers(const std::string& keyword, int page, int pageSize) const;

    // Communication ----------------------------------------------------
    ApiResult<std::vector<json>> listSmsInbox(const std::string& fromDate, const std::string& toDate,
                                              const std::string& phone, int page, int perPage) const;
    ApiResult<json> sendTicketSms(long long ticketId, const std::string& mobile, const std::string& content) const;

    // Generic raw access ---------------------------------------------------
    ApiResult<json> get(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query = {}) const;
    ApiResult<json> send(const std::string& method, const std::string& path, const json& body) const;

private:
    std::string buildUrl(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query) const;
    static ApiResult<json> toResult(const http::Response& r);

    std::string authHeaders() const;

    std::string baseUrl_;
    std::string apiKey_;
    std::string accessToken_;
};

// Finds the most plausible list payload in a RepairDesk response
// (e.g. data.inventoryListData, data.ticketData, data.items, items ...).
std::vector<json> extractList(const json& response);
