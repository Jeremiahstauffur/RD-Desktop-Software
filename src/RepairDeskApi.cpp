#include "RepairDeskApi.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unordered_set>

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

std::string jsonToText(const json& v) {
    if (v.is_null()) return "";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "Yes" : "No";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", v.get<double>());
        return buf;
    }
    return v.dump();
}

static void flattenInto(FlatRecord& out, const std::string& prefix, const json& j, int depth, int maxDepth) {
    if (j.is_object()) {
        if (depth >= maxDepth) {
            out[prefix] = j.dump();
            return;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
            flattenInto(out, key, it.value(), depth + 1, maxDepth);
        }
    } else if (j.is_array()) {
        if (j.empty()) {
            out[prefix] = "";
        } else if (j.front().is_primitive()) {
            std::string joined;
            for (const auto& e : j) {
                if (!joined.empty()) joined += ", ";
                joined += jsonToText(e);
            }
            out[prefix] = joined;
        } else {
            out[prefix] = std::to_string(j.size()) + " item(s)";
        }
    } else {
        out[prefix] = jsonToText(j);
    }
}

FlatRecord flattenJson(const json& j, int maxDepth) {
    FlatRecord out;
    if (j.is_object()) flattenInto(out, "", j, 0, maxDepth);
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool recordMatches(const FlatRecord& record, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    for (const auto& [k, v] : record) {
        if (toLower(v).find(needleLower) != std::string::npos) return true;
        if (toLower(k).find(needleLower) != std::string::npos) return true;
    }
    return false;
}

static std::string pickString(const json& j, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = j.find(k);
        if (it != j.end() && !it->is_null()) return jsonToText(*it);
    }
    return "";
}

static double pickNumber(const json& j, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) continue;
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) {
            const std::string& s = it->get_ref<const std::string&>();
            if (!s.empty()) return std::atof(s.c_str());
        }
    }
    return 0.0;
}

InventoryItem InventoryItem::fromJson(const json& j) {
    InventoryItem item;
    item.raw = j;
    item.fields = flattenJson(j);
    item.id = (long long)pickNumber(j, {"id", "item_id", "product_id"});
    item.name = pickString(j, {"name", "item_name", "title"});
    item.sku = pickString(j, {"sku", "SKU"});
    item.upc = pickString(j, {"upc_code", "upc", "barcode"});
    item.category = pickString(j, {"item_type", "category", "type", "category_name"});
    item.manufacturer = pickString(j, {"manufacturer", "manufacturer_name", "brand"});
    item.device = pickString(j, {"device", "device_name", "model"});
    item.price = pickNumber(j, {"price", "retail_price", "sale_price"});
    item.costPrice = pickNumber(j, {"cost_price", "cost"});
    item.inStock = (int)pickNumber(j, {"in_stock", "quantity", "stock", "qty"});

    // Make sure the canonical keys used by the default table columns exist,
    // even when the live API names a field differently.
    auto ensure = [&](const char* key, const std::string& value) {
        if (!item.fields.count(key)) item.fields[key] = value;
    };
    char num[64];
    ensure("id", std::to_string(item.id));
    ensure("name", item.name);
    ensure("sku", item.sku);
    ensure("upc_code", item.upc);
    ensure("item_type", item.category);
    ensure("manufacturer", item.manufacturer);
    ensure("device", item.device);
    ensure("in_stock", std::to_string(item.inStock));
    std::snprintf(num, sizeof(num), "%.2f", item.price);
    ensure("price", num);
    std::snprintf(num, sizeof(num), "%.2f", item.costPrice);
    ensure("cost_price", num);
    return item;
}

// True when the whole string is a (signed) integer literal.
static bool isIntegerText(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// Reduces a value from the list record to something PUT /inventory/{id} accepts:
// scalars pass through, reference objects ({id, name, ...}) collapse to their id,
// anything else (arrays, other objects, null) yields null.
static json scalarOf(const json& v) {
    if (v.is_number() || v.is_boolean()) return v;
    if (v.is_string()) return v.get<std::string>().empty() ? json() : v;
    if (v.is_object()) {
        for (const char* k : {"id", "value"}) {
            auto it = v.find(k);
            if (it != v.end() && (it->is_number() || it->is_string())) return *it;
        }
    }
    return json();
}

// Request body for PUT /inventory/{id}. Only the fields documented for the API's
// Item schema are sent: echoing the complete list record (nested objects, computed
// fields, customer group prices, ...) makes RepairDesk fail with HTTP 500.
json InventoryItem::toUpdatePayload(const json& overrides) const {
    const json rawObj = raw.is_object() ? raw : json::object();

    // Looks up `key` (then `altKeys`) in the raw record, falling back to the flattened fields.
    auto lookup = [&](const char* key, std::initializer_list<const char*> altKeys) -> json {
        for (const char* k : altKeys) {
            auto it = rawObj.find(k);
            if (it != rawObj.end()) {
                json v = scalarOf(*it);
                if (!v.is_null()) return v;
            }
        }
        auto it = rawObj.find(key);
        if (it != rawObj.end()) {
            json v = scalarOf(*it);
            if (!v.is_null()) return v;
        }
        auto f = fields.find(key);
        if (f != fields.end() && !f->second.empty()) return f->second;
        return json();
    };
    auto asInteger = [](json v) -> json {
        if (v.is_string() && isIntegerText(v.get_ref<const std::string&>())) return std::stoll(v.get<std::string>());
        if (v.is_number_float()) return (long long)v.get<double>();
        if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
        return v;
    };

    json body = json::object();
    body["id"] = id;
    body["name"] = name;
    if (!sku.empty()) body["sku"] = sku;
    if (!upc.empty()) body["upc_code"] = upc;
    if (!category.empty()) body["item_type"] = category;
    body["in_stock"] = inStock;
    body["price"] = price;
    body["cost_price"] = costPrice;

    // References RepairDesk stores as ids; the list may return the id, the name or an object.
    json v = asInteger(lookup("manufacturer", {"manufacturer_id"}));
    if (v.is_null() && !manufacturer.empty()) v = manufacturer;
    if (!v.is_null()) body["manufacturer"] = v;
    v = lookup("device", {"device_id"});
    if (v.is_null() && !device.empty()) v = device;
    if (!v.is_null()) body["device"] = v;

    static const char* integerKeys[] = {"supplier", "tax_class", "tax_inclusive", "valuation_method", "is_serialize"};
    for (const char* k : integerKeys) {
        const std::string idKey = std::string(k) + "_id";
        v = asInteger(lookup(k, {idKey.c_str()}));
        if (!v.is_null()) body[k] = v;
    }
    static const char* textKeys[] = {"reorder_level", "stock_warning", "minimum_price", "description", "notes",
                                     "condition", "physical_location"};
    for (const char* k : textKeys) {
        v = lookup(k, {});
        if (!v.is_null()) body[k] = v;
    }

    if (overrides.is_object()) {
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    return body;
}

static const json* findFirstArray(const json& j, int depth) {
    if (j.is_array()) return &j;
    if (!j.is_object() || depth > 3) return nullptr;

    // Prefer well known RepairDesk payload names.
    static const char* preferred[] = {"inventoryListData", "ticketData", "invoiceData", "customerData", "items",
                                      "inventory", "tickets", "invoices", "customers", "data", "list"};
    for (const char* k : preferred) {
        auto it = j.find(k);
        if (it != j.end()) {
            if (it->is_array()) return &*it;
            if (const json* nested = findFirstArray(*it, depth + 1)) return nested;
        }
    }
    for (const auto& [k, v] : j.items()) {
        if (v.is_array() && !v.empty() && v.front().is_object()) return &v;
    }
    for (const auto& [k, v] : j.items()) {
        if (v.is_object()) {
            if (const json* nested = findFirstArray(v, depth + 1)) return nested;
        }
    }
    return nullptr;
}

std::vector<json> extractList(const json& response) {
    std::vector<json> out;
    const json* arr = findFirstArray(response, 0);
    if (!arr) return out;
    out.reserve(arr->size());
    for (const auto& e : *arr) {
        if (e.is_object()) out.push_back(e);
    }
    return out;
}

// ---------------------------------------------------------------------------
// RepairDeskApi
// ---------------------------------------------------------------------------

RepairDeskApi::RepairDeskApi(std::string baseUrl, std::string apiKey, std::string accessToken)
    : baseUrl_(std::move(baseUrl)), apiKey_(std::move(apiKey)), accessToken_(std::move(accessToken)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

std::string RepairDeskApi::authHeaders() const {
    if (!accessToken_.empty()) return "Authorization: Bearer " + accessToken_ + "\r\n";
    return "";
}

// ---------------------------------------------------------------------------
// OAuth2
// ---------------------------------------------------------------------------

static std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string RepairDeskApi::authorizeUrl(const std::string& baseUrl, const std::string& clientId,
                                        const std::string& redirectUri, const std::string& state) {
    return trimSlash(baseUrl) + "/oauth2/authorize?response_type=code&client_id=" + http::urlEncode(clientId) +
           "&redirect_uri=" + http::urlEncode(redirectUri) + "&state=" + http::urlEncode(state);
}

static std::string bodySnippet(const std::string& body) {
    std::string s = body.substr(0, 240);
    for (char& c : s) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return body.size() > 240 ? s + "..." : s;
}

// RepairDesk's OAuth endpoints answer HTTP 200 even on failure and wrap the
// RFC 6749 payload in {success, statusCode, message, data}; the tokens or the
// error/error_description live inside "data". Plain (unwrapped) responses are
// accepted too.
static ApiResult<OAuthTokens> tokenRequest(const std::string& baseUrl, const std::string& form) {
    ApiResult<OAuthTokens> out;
    http::Response r = http::request("POST", trimSlash(baseUrl) + "/oauth2/token", form,
                                     "application/x-www-form-urlencoded");
    out.status = r.status;
    if (!r.error.empty()) {
        out.message = r.error;
        return out;
    }
    json j;
    try {
        j = json::parse(r.body);
    } catch (...) {
        out.message = "Token endpoint returned a non-JSON response (HTTP " + std::to_string(r.status) + "): " + bodySnippet(r.body);
        return out;
    }
    if (!j.is_object()) {
        out.message = "Token endpoint returned an unexpected response (HTTP " + std::to_string(r.status) + "): " + bodySnippet(r.body);
        return out;
    }

    const json& payload = j.contains("data") && j["data"].is_object() ? j["data"] : j;
    bool success = r.status >= 200 && r.status < 300;
    if (j.contains("success") && j["success"].is_boolean() && !j["success"].get<bool>()) success = false;
    int innerStatus = (int)pickNumber(j, {"statusCode", "status_code", "status"});
    if (innerStatus >= 400) {
        success = false;
        out.status = innerStatus;
    }

    std::string accessToken = pickString(payload, {"access_token", "accessToken"});
    if (success && !accessToken.empty()) {
        out.ok = true;
        out.value.accessToken = accessToken;
        out.value.refreshToken = pickString(payload, {"refresh_token", "refreshToken"});
        long long expiresIn = (long long)pickNumber(payload, {"expires_in", "expiresIn"});
        if (expiresIn <= 0) expiresIn = 3600;
        out.value.expiresAt = (long long)std::time(nullptr) + expiresIn;
        out.message = "OK";
        return out;
    }

    std::string err = pickString(payload, {"error"});
    if (err.empty()) err = pickString(j, {"error"});
    std::string desc = pickString(payload, {"error_description", "message"});
    if (desc.empty()) desc = pickString(j, {"error_description", "message"});
    std::string hint = pickString(payload, {"hint"});
    if (hint.empty()) hint = pickString(j, {"hint"});

    if (success) {
        // 2xx / success:true but no token anywhere in the response
        out.message = "Token endpoint did not return an access token: " + bodySnippet(r.body);
        return out;
    }
    if (err.empty() && desc.empty()) {
        out.message = "Token request failed (HTTP " + std::to_string(out.status) + "): " + bodySnippet(r.body);
        return out;
    }
    out.message = desc.empty() ? err : (err.empty() || err == desc ? desc : err + ": " + desc);
    if (!hint.empty() && hint != desc) out.message += " (" + hint + ")";
    return out;
}

ApiResult<OAuthTokens> RepairDeskApi::exchangeCode(const std::string& baseUrl, const std::string& clientId,
                                                   const std::string& clientSecret, const std::string& code,
                                                   const std::string& redirectUri) {
    std::string form = "grant_type=authorization_code&client_id=" + http::urlEncode(clientId) +
                       "&client_secret=" + http::urlEncode(clientSecret) + "&code=" + http::urlEncode(code) +
                       "&redirect_uri=" + http::urlEncode(redirectUri);
    return tokenRequest(baseUrl, form);
}

ApiResult<OAuthTokens> RepairDeskApi::refreshAccessToken(const std::string& baseUrl, const std::string& clientId,
                                                         const std::string& clientSecret, const std::string& refreshToken) {
    std::string form = "grant_type=refresh_token&client_id=" + http::urlEncode(clientId) +
                       "&client_secret=" + http::urlEncode(clientSecret) + "&refresh_token=" + http::urlEncode(refreshToken);
    ApiResult<OAuthTokens> r = tokenRequest(baseUrl, form);
    if (r.ok && r.value.refreshToken.empty()) r.value.refreshToken = refreshToken; // refresh tokens may be reused
    return r;
}

ApiResult<json> RepairDeskApi::revokeToken(const std::string& baseUrl, const std::string& clientId,
                                           const std::string& clientSecret, const std::string& token) {
    std::string form = "token=" + http::urlEncode(token) + "&client_id=" + http::urlEncode(clientId) +
                       "&client_secret=" + http::urlEncode(clientSecret);
    return toResult(http::request("POST", trimSlash(baseUrl) + "/oauth2/revoke", form, "application/x-www-form-urlencoded"));
}

std::string RepairDeskApi::buildUrl(const std::string& path,
                                    const std::vector<std::pair<std::string, std::string>>& query) const {
    std::string url = baseUrl_;
    if (path.empty() || path.front() != '/') url += '/';
    url += path;
    bool first = path.find('?') == std::string::npos;
    if (accessToken_.empty()) {
        url += first ? "?api_key=" : "&api_key=";
        url += http::urlEncode(apiKey_);
        first = false;
    }
    for (const auto& [k, v] : query) {
        if (v.empty()) continue;
        url += first ? '?' : '&';
        first = false;
        url += http::urlEncode(k);
        url += '=';
        url += http::urlEncode(v);
    }
    return url;
}

ApiResult<json> RepairDeskApi::toResult(const http::Response& r) {
    ApiResult<json> out;
    out.status = r.status;
    if (!r.error.empty()) {
        out.message = r.error;
        return out;
    }
    if (!r.body.empty()) {
        try {
            out.value = json::parse(r.body);
        } catch (const std::exception&) {
            out.message = "Server returned a non-JSON response (HTTP " + std::to_string(r.status) + ")";
            return out;
        }
    }
    out.ok = r.status >= 200 && r.status < 300;
    if (out.value.is_object()) {
        if (out.value.contains("success") && out.value["success"].is_boolean() && !out.value["success"].get<bool>()) {
            out.ok = false;
        }
        if (out.value.contains("message") && out.value["message"].is_string()) {
            out.message = out.value["message"].get<std::string>();
        }
        if (!out.ok && out.value.contains("data") && out.value["data"].is_object()) {
            const json& d = out.value["data"];
            if (d.contains("message") && d["message"].is_string()) out.message = d["message"].get<std::string>();
        }
    }
    if (!out.ok && out.message.empty()) out.message = "Request failed (HTTP " + std::to_string(r.status) + ")";
    if (out.ok && out.message.empty()) out.message = "OK";
    return out;
}

ApiResult<json> RepairDeskApi::get(const std::string& path,
                                   const std::vector<std::pair<std::string, std::string>>& query) const {
    if (!hasKey()) {
        ApiResult<json> r;
        r.message = "Not connected. Open Settings and enter an API key or connect with OAuth2.";
        return r;
    }
    return toResult(http::request("GET", buildUrl(path, query), {}, "application/json", authHeaders()));
}

ApiResult<json> RepairDeskApi::send(const std::string& method, const std::string& path, const json& body) const {
    if (!hasKey()) {
        ApiResult<json> r;
        r.message = "Not connected. Open Settings and enter an API key or connect with OAuth2.";
        return r;
    }
    return toResult(http::request(method, buildUrl(path, {}), body.is_null() ? std::string() : body.dump(),
                                  "application/json", authHeaders()));
}

ApiResult<std::vector<InventoryItem>> RepairDeskApi::listInventory(const std::string& keyword, int maxItems, int pageSize,
                                                                    const ProgressFn& progress) const {
    ApiResult<std::vector<InventoryItem>> out;
    out.ok = true;
    out.status = 200;
    out.message = "OK";

    if (pageSize <= 0) pageSize = 100;
    std::unordered_set<long long> seenIds;
    int page = 1;
    const int maxPages = 2000;

    while (page <= maxPages) {
        std::vector<std::pair<std::string, std::string>> query = {
            {"page", std::to_string(page)},
            {"pagesize", std::to_string(pageSize)}
        };
        if (!keyword.empty()) {
            query.push_back({"keyword", keyword});
        }

        auto raw = get("/inventory", query);
        if (!raw.ok) {
            if (page == 1) {
                out.ok = false;
                out.status = raw.status;
                out.message = raw.message;
                return out;
            }
            break;
        }

        std::vector<json> items = extractList(raw.value);
        if (items.empty()) {
            break;
        }

        size_t newItemsCount = 0;
        for (const json& j : items) {
            InventoryItem item = InventoryItem::fromJson(j);
            if (item.id != 0) {
                if (seenIds.count(item.id)) continue;
                seenIds.insert(item.id);
            }
            out.value.push_back(std::move(item));
            newItemsCount++;
            if (maxItems > 0 && (int)out.value.size() >= maxItems) {
                break;
            }
        }
        if (progress) progress(out.value.size());

        if (maxItems > 0 && (int)out.value.size() >= maxItems) {
            break;
        }

        if (newItemsCount == 0) {
            break;
        }

        if ((int)items.size() < pageSize && (int)items.size() < 100) {
            break;
        }

        page++;
    }

    return out;
}

ApiResult<InventoryItem> RepairDeskApi::getInventoryItem(long long id) const {
    ApiResult<InventoryItem> out;
    ApiResult<json> raw = get("/inventory", {{"keyword", std::to_string(id)}, {"page", "1"}, {"pagesize", "50"}});
    out.status = raw.status;
    out.message = raw.message;
    if (!raw.ok) return out;

    // The keyword also matches names/SKUs, so pick the record with the exact id.
    for (const json& j : extractList(raw.value)) {
        InventoryItem item = InventoryItem::fromJson(j);
        if (item.id == id) {
            out.ok = true;
            out.value = std::move(item);
            return out;
        }
    }
    out.message = "Item " + std::to_string(id) + " was not returned by the server.";
    return out;
}

ApiResult<json> RepairDeskApi::addInventoryItem(const json& fields) const {
    return send("POST", "/inventory/addnewitem", fields);
}

ApiResult<json> RepairDeskApi::updateInventoryItem(long long id, const json& fields) const {
    json body = fields;
    body["id"] = id;
    return send("PUT", "/inventory/" + std::to_string(id), body);
}

ApiResult<json> RepairDeskApi::deleteInventoryItem(long long id) const {
    return send("DELETE", "/inventory/" + std::to_string(id), json());
}

static ApiResult<std::vector<json>> toList(const ApiResult<json>& raw) {
    ApiResult<std::vector<json>> out;
    out.ok = raw.ok;
    out.status = raw.status;
    out.message = raw.message;
    if (raw.ok) out.value = extractList(raw.value);
    return out;
}

ApiResult<std::vector<json>> RepairDeskApi::listTickets(const std::string& keyword, int page, int pageSize) const {
    return toList(get("/tickets", {{"keyword", keyword}, {"page", std::to_string(page)}, {"pagesize", std::to_string(pageSize)}}));
}

ApiResult<std::vector<json>> RepairDeskApi::listInvoices(const std::string& keyword, int page, int pageSize) const {
    return toList(get("/invoices", {{"keyword", keyword}, {"page", std::to_string(page)}, {"pagesize", std::to_string(pageSize)}}));
}

ApiResult<json> RepairDeskApi::createInvoice(const json& body) const {
    return send("POST", "/invoices", body);
}

ApiResult<std::vector<json>> RepairDeskApi::listCustomers(const std::string& keyword, int page, int pageSize) const {
    return toList(get("/customers", {{"keyword", keyword}, {"page", std::to_string(page)}, {"pagesize", std::to_string(pageSize)}}));
}

ApiResult<std::vector<json>> RepairDeskApi::listSmsInbox(const std::string& fromDate, const std::string& toDate,
                                                        const std::string& phone, int page, int perPage) const {
    return toList(get("/v1/sms-inbox", {{"from_date", fromDate}, {"to_date", toDate}, {"phone_number", phone},
                                        {"page", std::to_string(page)}, {"per_page", std::to_string(perPage)}}));
}

ApiResult<json> RepairDeskApi::sendTicketSms(long long ticketId, const std::string& mobile, const std::string& content) const {
    json body;
    body["ticketId"] = ticketId;
    body["mobile"] = mobile;
    body["content"] = content;
    return send("POST", "/v1/ticket/sendsms", body);
}
