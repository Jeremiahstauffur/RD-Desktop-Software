#pragma once

#include <functional>
#include <map>
#include <memory>
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

struct NamedId {
    long long id = 0;
    std::string name;
};

struct DeviceInfo {
    long long id = 0;
    long long manufacturerId = 0;
    std::string name;
};

// Reference data used to translate the names shown in the UI into the ids the
// RepairDesk write endpoints require (manufacturer, device, supplier and
// tax_class must be integers; condition / physical_location must match names).
struct Lookups {
    std::vector<NamedId> manufacturers;    // GET /devices -> company / manufacturer_id
    std::vector<DeviceInfo> devices;       // GET /devices
    std::vector<NamedId> suppliers;        // GET /purchaseorders -> supplier / vendorID
    std::vector<NamedId> taxClasses;       // GET /taxclasses
    std::vector<std::string> conditions;   // GET /inventory/additionalinfo
    std::vector<std::string> locations;    // GET /inventory/additionalinfo
    bool loaded = false;

    // Case-insensitive name match; a purely numeric string matches by id.
    const NamedId* findManufacturer(const std::string& nameOrId) const;
    const DeviceInfo* findDevice(const std::string& nameOrId, long long manufacturerId = 0) const;
    const NamedId* findSupplier(const std::string& nameOrId) const;
    const NamedId* findTaxClass(const std::string& nameOrId) const;
    std::string manufacturerName(long long id) const;
    std::string deviceName(long long id) const;
    std::string taxClassName(long long id) const;
};

// Inventory valuation methods offered by RepairDesk. The API takes
// `valuation_method` as an integer; the codes follow the Swagger example (FIFO = 1).
// Note: the public API currently accepts but does not persist this field.
struct ValuationMethod {
    int code;
    const char* label;
};
inline constexpr ValuationMethod kValuationMethods[] = {
    {1, "FIFO (First In, First Out)"},
    {2, "LIFO (Last In, First Out)"},
    {3, "WAC (Weighted Average Cost)"},
};
inline constexpr int kDefaultValuationMethod = 1;

// Feedback from an inventory write about how the payload was adjusted.
struct WriteNotes {
    std::vector<std::string> messages;   // dropped fields, created references
    bool createdReferences = false;      // a manufacturer/device was created via the API
    bool createMissing = false;          // input: create unknown manufacturers/devices
};

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
    // Complete PUT body for this item (RepairDesk wipes every field that is not
    // sent). Lookup fields are emitted as names where the record has them and
    // must be run through resolveLookupIds() before sending. `in_stock` is NOT
    // included: RepairDesk treats it as an adjustment (see saveInventoryItem), so
    // callers must add it explicitly or use RepairDeskApi::saveInventoryItem.
    json toUpdatePayload(const json& overrides = json::object()) const;
};

// Converts name based lookup fields (manufacturer, device, supplier, tax_class)
// in `body` into ids and coerces numeric fields. Values that cannot be matched
// are removed (the API rejects the whole request otherwise) and reported in
// `notes` when given.
void resolveLookupIds(json& body, const Lookups& lookups, std::vector<std::string>* notes = nullptr);

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

    // Reference data used by the inventory write methods to turn names into ids.
    void setLookups(std::shared_ptr<const Lookups> lookups) { lookups_ = std::move(lookups); }

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
    // `progress` (optional) is invoked on the calling thread after every fetched page
    // with the number of items collected so far.
    using ProgressFn = std::function<void(size_t loaded)>;
    ApiResult<std::vector<InventoryItem>> listInventory(const std::string& keyword = "", int maxItems = 0, int pageSize = 100,
                                                        const ProgressFn& progress = nullptr) const;
    // Re-reads one item (GET /inventory?keyword=<id>; the keyword filter matches ids).
    ApiResult<InventoryItem> getInventoryItem(long long id) const;
    // Both write methods run the body through resolveLookupIds() (and, when
    // notes->createMissing is set, createMissingReferences()) before sending.
    ApiResult<json> addInventoryItem(const json& fields, WriteNotes* notes = nullptr) const;   // POST /inventory/addnewitem
    // Low-level PUT. `fields` must contain `in_stock` (absent = stock wiped; positive = added
    // to the current stock; zero/negative = stock replaced). Prefer saveInventoryItem.
    ApiResult<json> updateInventoryItem(long long id, const json& fields, WriteNotes* notes = nullptr) const;
    // Writes `fields` (a toUpdatePayload body) and leaves the item's stock at exactly
    // `targetStock`, working around the adjustment semantics of `in_stock`. The item
    // is re-read afterwards and returned (value.id == 0 when the re-read failed).
    ApiResult<InventoryItem> saveInventoryItem(long long id, const json& fields, int currentStock, int targetStock,
                                               WriteNotes* notes = nullptr) const;
    ApiResult<json> deleteInventoryItem(long long id) const;

    // Reference data -----------------------------------------------------
    ApiResult<Lookups> loadLookups() const;
    ApiResult<long long> createBrand(const std::string& name) const;                          // POST /createbrand
    ApiResult<long long> createModel(long long manufacturerId, const std::string& name) const; // POST /createmodel
    // Creates manufacturers / devices named in `body` that RepairDesk does not know
    // yet and adds them to `lookups`. Returns true when something was created.
    bool createMissingReferences(json& body, Lookups& lookups, std::vector<std::string>& notes) const;

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

    json prepareWriteBody(const json& fields, WriteNotes* notes) const;

    std::string baseUrl_;
    std::string apiKey_;
    std::string accessToken_;
    std::shared_ptr<const Lookups> lookups_;
};

// Finds the most plausible list payload in a RepairDesk response
// (e.g. data.inventoryListData, data.ticketData, data.items, items ...).
std::vector<json> extractList(const json& response);
