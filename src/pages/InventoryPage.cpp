#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "App.h"
#include "Async.h"
#include "DataTable.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

namespace {

enum class BulkAction { None, Delete, EditCost, EditPrice, EditQuantity, EditCategory };

struct BulkOutcome {
    int succeeded = 0;
    int failed = 0;
    std::string firstError;
};

struct InventoryState {
    DataTable table{"inventoryTable"};
    std::string search;
    std::string serverKeyword;
    std::vector<int> view;                 // indices into app.inventory after filter + sort
    std::vector<bool> selectionVec;
    std::set<long long> selectedIds;
    size_t lastInventorySize = (size_t)-1;
    unsigned lastInventoryRevision = 0;
    std::string lastSearch;
    bool columnsInitialised = false;
    bool viewDirty = true;

    // bulk actions
    BulkAction pendingAction = BulkAction::None;
    bool openBulkModal = false;
    double bulkNumber = 0.0;
    int bulkInt = 0;
    std::string bulkText;
    bool bulkRunning = false;

    // product page
    long long openProductId = 0;
    bool creatingProduct = false;          // product page shown as a "New product" form
    struct Editor {
        std::string name, sku, upc, category, manufacturer, device, supplier, condition, location;
        std::string description, notes, reorderLevel, stockWarning, minimumPrice;
        int qty = 0;
        double cost = 0.0, price = 0.0;
        int valuationIndex = 0;            // index into kValuationMethods (FIFO by default)
        int taxClassIndex = -1;            // index into app.lookups->taxClasses (-1 = not set)
        bool dirty = false;
    } editor;
    bool productSaving = false;

    // Suggestion lists for the editor drop-downs, rebuilt when the inventory or lookups change.
    struct Suggestions {
        std::vector<std::string> categories, manufacturers, devices, suppliers, locations, conditions, taxClasses;
        size_t inventorySize = (size_t)-1;
        unsigned inventoryRevision = 0;
        const void* lookups = nullptr;
    } suggest;

    // Suggested-price formula editor (pencil next to the price pill).
    bool openFormulaPopup = false;
    double formulaMultiplier = 2.0, formulaRoundTo = 0.2, formulaSubtract = 0.05;
};

// Fields that may be edited inline in the table / product page and sent to PUT /inventory/{id}.
bool isEditableKey(const std::string& key) {
    static const char* keys[] = {"name", "sku", "upc_code", "item_type", "in_stock", "price", "cost_price",
                                 "manufacturer", "device", "supplier", "condition", "physical_location",
                                 "reorder_level", "stock_warning", "minimum_price", "description", "notes",
                                 "tax_class", "tax_inclusive", "valuation_method", "is_serialize"};
    for (const char* k : keys) if (key == k) return true;
    return false;
}

bool isNumericKey(const std::string& key) {
    static const char* keys[] = {"in_stock", "price", "cost_price", "minimum_price", "tax_class", "tax_inclusive",
                                 "valuation_method", "is_serialize", "reorder_level", "stock_warning"};
    for (const char* k : keys) if (key == k) return true;
    return false;
}

InventoryState& S() {
    static InventoryState s;
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Case-insensitive substring test without allocating (`needleLower` must already be lower-case).
bool containsNoCase(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    if (hay.size() < needleLower.size()) return false;
    auto it = std::search(hay.begin(), hay.end(), needleLower.begin(), needleLower.end(),
                          [](unsigned char a, unsigned char b) { return (char)std::tolower(a) == (char)b; });
    return it != hay.end();
}

// Splits the search box text into lower-case terms; every term must match some field.
std::vector<std::string> searchTerms(const std::string& text) {
    std::vector<std::string> terms;
    std::string cur;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            if (!cur.empty()) { terms.push_back(cur); cur.clear(); }
        } else {
            cur.push_back((char)std::tolower(c));
        }
    }
    if (!cur.empty()) terms.push_back(cur);
    return terms;
}

// True when every term occurs in at least one field value of the record (any field,
// including description, notes, location, tax class, nested values...).
bool itemMatchesTerms(const InventoryItem& it, const std::vector<std::string>& terms) {
    for (const std::string& term : terms) {
        bool found = false;
        for (const auto& [key, value] : it.fields) {
            if (containsNoCase(value, term)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

void setupColumns(DataTable& table) {
    table.setColumns({
        {"name", "Name", 280.0f, true},
        {"sku", "SKU", 150.0f, true},
        {"upc_code", "UPC", 150.0f, true},
        {"item_type", "Category", 150.0f, true},
        {"in_stock", "In stock", 100.0f, true},
        {"price", "Price", 110.0f, true},
        {"cost_price", "Cost", 110.0f, true},
        {"manufacturer", "Manufacturer", 150.0f, false},
        {"device", "Device", 150.0f, false},
        {"supplier", "Supplier", 140.0f, false},
        {"condition", "Condition", 120.0f, false},
        {"physical_location", "Location", 130.0f, false},
        {"tax_class", "Tax class", 110.0f, false},
        {"tax_inclusive", "Tax inclusive", 110.0f, false},
        {"reorder_level", "Reorder level", 110.0f, false},
        {"stock_warning", "Stock warning", 110.0f, false},
        {"minimum_price", "Minimum price", 120.0f, false},
        {"valuation_method", "Valuation", 110.0f, false},
        {"is_serialize", "Serialized", 100.0f, false},
        {"description", "Description", 240.0f, false},
        {"notes", "Notes", 240.0f, false},
        {"id", "ID", 100.0f, false},
    });
}

// Shows the adjustments the API layer made to a write (dropped unknown names, created brands...).
void reportWriteNotes(App& app, const WriteNotes& notes) {
    for (const std::string& m : notes.messages) app.notify(m, ui::InfoKind::Warning);
    if (notes.createdReferences) app.refreshLookups();
}

double toNumber(const std::string& s) {
    if (s.empty()) return 0.0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? 0.0 : v;
}

bool looksNumeric(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

void rebuildView(App& app) {
    InventoryState& s = S();
    s.view.clear();
    const std::vector<std::string> terms = searchTerms(s.search);
    const bool filtered = s.table.hasFilters();
    for (size_t i = 0; i < app.inventory.size(); ++i) {
        if (!terms.empty() && !itemMatchesTerms(app.inventory[i], terms)) continue;
        if (filtered && !s.table.passesFilters(app.inventory[i].fields)) continue;
        s.view.push_back((int)i);
    }

    if (!s.table.sortKey.empty()) {
        const std::string key = s.table.sortKey;
        const bool asc = s.table.sortAscending;
        auto valueOf = [&](int idx) -> const std::string& {
            static const std::string empty;
            auto it = app.inventory[idx].fields.find(key);
            return it == app.inventory[idx].fields.end() ? empty : it->second;
        };
        // Numeric sort when every value in the column parses as a number.
        bool numeric = !s.view.empty();
        for (int idx : s.view) {
            const std::string& v = valueOf(idx);
            if (!v.empty() && !looksNumeric(v)) { numeric = false; break; }
        }
        std::stable_sort(s.view.begin(), s.view.end(), [&](int a, int b) {
            const std::string& va = valueOf(a);
            const std::string& vb = valueOf(b);
            bool less = numeric ? toNumber(va) < toNumber(vb) : lower(va) < lower(vb);
            bool greater = numeric ? toNumber(vb) < toNumber(va) : lower(vb) < lower(va);
            return asc ? less : greater;
        });
    }
    s.viewDirty = false;
}

const char* bulkTitle(BulkAction a) {
    switch (a) {
        case BulkAction::Delete: return "Delete items";
        case BulkAction::EditCost: return "Edit cost";
        case BulkAction::EditPrice: return "Edit price";
        case BulkAction::EditQuantity: return "Edit quantity";
        case BulkAction::EditCategory: return "Edit category";
        default: return "";
    }
}

void runBulkAction(App& app) {
    InventoryState& s = S();
    std::vector<InventoryItem> targets;
    for (const InventoryItem& it : app.inventory) {
        if (s.selectedIds.count(it.id)) targets.push_back(it);
    }
    if (targets.empty()) return;

    BulkAction action = s.pendingAction;
    double number = s.bulkNumber;
    int integer = s.bulkInt;
    std::string text = s.bulkText;
    RepairDeskApi client = app.api();
    s.bulkRunning = true;
    auto job = app.beginWriteJob(targets.size());

    Async::run(
        [client, targets, action, number, integer, text, job] {
            BulkOutcome out;
            for (const InventoryItem& it : targets) {
                bool ok;
                std::string message;
                if (action == BulkAction::Delete) {
                    ApiResult<json> r = client.deleteInventoryItem(it.id);
                    ok = r.ok;
                    message = r.message;
                } else {
                    json overrides;
                    int targetStock = it.inStock;
                    switch (action) {
                        case BulkAction::EditCost: overrides["cost_price"] = number; break;
                        case BulkAction::EditPrice: overrides["price"] = number; break;
                        case BulkAction::EditQuantity: targetStock = integer; break;
                        case BulkAction::EditCategory: overrides["item_type"] = text; break;
                        default: break;
                    }
                    ApiResult<InventoryItem> r = client.saveInventoryItem(it.id, it.toUpdatePayload(overrides), it.inStock, targetStock);
                    ok = r.ok;
                    message = r.message;
                }
                if (ok) {
                    out.succeeded++;
                } else {
                    out.failed++;
                    if (out.firstError.empty()) out.firstError = message;
                }
                job->done++;
            }
            return out;
        },
        [&app, action, job](BulkOutcome out) {
            InventoryState& st = S();
            st.bulkRunning = false;
            app.finishWriteJob(job);
            st.pendingAction = BulkAction::None;
            char buf[256];
            if (out.failed == 0) {
                std::snprintf(buf, sizeof(buf), "%s: %d item(s) updated.", bulkTitle(action), out.succeeded);
                app.notify(buf, ui::InfoKind::Success);
                st.selectedIds.clear();
            } else {
                std::snprintf(buf, sizeof(buf), "%s: %d succeeded, %d failed. %s", bulkTitle(action), out.succeeded,
                              out.failed, out.firstError.c_str());
                app.notify(buf, ui::InfoKind::Error);
            }
            app.refreshInventory(st.serverKeyword);
        });
}

// Converts an edited string into the JSON value the API expects for `key`.
json fieldValue(const std::string& key, const std::string& text) {
    if (!isNumericKey(key)) return text;
    if (key == "in_stock" || key == "tax_class" || key == "tax_inclusive" || key == "valuation_method" || key == "is_serialize") {
        return (long long)toNumber(text);
    }
    return toNumber(text);
}

// Applies an edit locally (so the UI reacts immediately) and sends it to RepairDesk.
// `fields` may contain `in_stock` as the *new absolute quantity*; the API layer turns
// it into the adjustment RepairDesk expects and keeps the stock unchanged otherwise.
void saveFields(App& app, long long id, const json& fields, const std::string& what) {
    json payload;
    int currentStock = 0;
    int targetStock = 0;
    for (InventoryItem& it : app.inventory) {
        if (it.id != id) continue;
        currentStock = it.inStock;
        for (auto it2 = fields.begin(); it2 != fields.end(); ++it2) {
            it.raw[it2.key()] = it2.value();
        }
        it = InventoryItem::fromJson(it.raw);
        targetStock = it.inStock;
        payload = it.toUpdatePayload(fields);
        break;
    }
    S().viewDirty = true;
    if (payload.is_null() || payload.empty()) return;
    payload.erase("in_stock");

    RepairDeskApi client = app.api();
    auto job = app.beginWriteJob();
    Async::run(
        [client, id, payload, currentStock, targetStock] {
            std::pair<ApiResult<InventoryItem>, WriteNotes> out;
            out.second.createMissing = true;
            out.first = client.saveInventoryItem(id, payload, currentStock, targetStock, &out.second);
            return out;
        },
        [&app, what, job](std::pair<ApiResult<InventoryItem>, WriteNotes> out) {
            S().productSaving = false;
            app.finishWriteJob(job);
            reportWriteNotes(app, out.second);
            const ApiResult<InventoryItem>& r = out.first;
            if (r.ok) {
                app.notify(what + " saved.", ui::InfoKind::Success);
            } else {
                app.notify(what + " failed: " + r.message, ui::InfoKind::Error);
            }
            if (r.value.id != 0) app.replaceInventoryItem(r.value);
            else app.refreshInventory(S().serverKeyword);
        });
}

// Saves the cells edited inline in one row (one PUT for the whole row).
void saveRowEdits(App& app, const InventoryItem& item, const DataTable::RowEdits& edits) {
    if (edits.empty()) return;
    json fields;
    fields["name"] = item.name;   // the API requires a name on update
    std::string what;
    for (const auto& [key, text] : edits) {
        if (!isEditableKey(key)) continue;
        fields[key] = fieldValue(key, text);
        if (!what.empty()) what += ", ";
        what += DataTable::prettifyKey(key);
    }
    if (what.empty()) return;
    saveFields(app, item.id, fields, what);
}

const InventoryItem* findItem(const App& app, long long id) {
    for (const InventoryItem& it : app.inventory) if (it.id == id) return &it;
    return nullptr;
}

// Case-insensitive sorted unique list of the non-empty values of `key` across the inventory.
void collectValues(const App& app, const char* key, std::vector<std::string>& out) {
    std::set<std::string> seenLower;
    for (const std::string& v : out) seenLower.insert(lower(v));
    for (const InventoryItem& it : app.inventory) {
        auto f = it.fields.find(key);
        if (f == it.fields.end() || f->second.empty() || looksNumeric(f->second)) continue;
        if (seenLower.insert(lower(f->second)).second) out.push_back(f->second);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
}

void rebuildSuggestions(App& app, InventoryState& s) {
    InventoryState::Suggestions& g = s.suggest;
    const Lookups& L = *app.lookups;
    if (g.inventorySize == app.inventory.size() && g.inventoryRevision == app.inventoryRevision && g.lookups == &L) return;
    g.inventorySize = app.inventory.size();
    g.inventoryRevision = app.inventoryRevision;
    g.lookups = &L;

    g.categories.clear();
    g.manufacturers.clear();
    g.devices.clear();
    g.suppliers.clear();
    g.locations.clear();
    g.conditions.clear();
    g.taxClasses.clear();
    for (const NamedId& m : L.manufacturers) g.manufacturers.push_back(m.name);
    for (const DeviceInfo& d : L.devices) g.devices.push_back(d.name);
    for (const NamedId& v : L.suppliers) g.suppliers.push_back(v.name);
    g.locations = L.locations;
    g.conditions = L.conditions;
    for (const NamedId& t : L.taxClasses) g.taxClasses.push_back(t.name);
    collectValues(app, "item_type", g.categories);
    collectValues(app, "manufacturer", g.manufacturers);
    collectValues(app, "device", g.devices);
    collectValues(app, "supplier", g.suppliers);
    collectValues(app, "physical_location", g.locations);
    collectValues(app, "condition", g.conditions);
}

int valuationIndexOf(const std::string& text) {
    if (text.empty()) return 0;   // FIFO default
    std::string t = lower(text);
    for (int i = 0; i < (int)std::size(kValuationMethods); ++i) {
        if (std::to_string(kValuationMethods[i].code) == t) return i;
        if (lower(kValuationMethods[i].label).rfind(t, 0) == 0) return i;
    }
    return 0;
}

int taxClassIndexOf(const App& app, const InventoryItem& it) {
    const Lookups& L = *app.lookups;
    auto f = [&](const char* k) { auto i = it.fields.find(k); return i == it.fields.end() ? std::string() : i->second; };
    std::string id = f("tax_class.id");
    if (id.empty()) id = f("tax_class_id");
    std::string name = f("tax_class");
    for (int i = 0; i < (int)L.taxClasses.size(); ++i) {
        if (!id.empty() && std::to_string(L.taxClasses[i].id) == id) return i;
        if (!name.empty() && (lower(L.taxClasses[i].name) == lower(name) || std::to_string(L.taxClasses[i].id) == name)) return i;
    }
    return -1;
}

std::string usd(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "$%.2f", v);
    return buf;
}

void loadEditor(App& app, InventoryState& s, const InventoryItem& it) {
    auto f = [&](const char* k) { auto i = it.fields.find(k); return i == it.fields.end() ? std::string() : i->second; };
    InventoryState::Editor& e = s.editor;
    e.name = it.name;
    e.sku = it.sku;
    e.upc = it.upc;
    e.category = it.category;
    e.manufacturer = it.manufacturer;
    e.device = it.device;
    e.supplier = f("supplier");
    e.condition = f("condition");
    e.location = f("physical_location");
    e.description = f("description");
    e.notes = f("notes");
    e.reorderLevel = f("reorder_level");
    e.stockWarning = f("stock_warning");
    e.minimumPrice = f("minimum_price");
    e.qty = it.inStock;
    e.cost = it.costPrice;
    e.price = it.price;
    e.valuationIndex = valuationIndexOf(f("valuation_method"));
    e.taxClassIndex = taxClassIndexOf(app, it);
    // Lookups may return ids for manufacturer/device; show the names instead.
    if (looksNumeric(e.manufacturer)) {
        std::string n = app.lookups->manufacturerName((long long)toNumber(e.manufacturer));
        if (!n.empty()) e.manufacturer = n;
    }
    if (looksNumeric(e.device)) {
        std::string n = app.lookups->deviceName((long long)toNumber(e.device));
        if (!n.empty()) e.device = n;
    }
    e.dirty = false;
}

// Builds the JSON payload for the product editor (shared by update and create).
json editorFields(const App& app, const InventoryState::Editor& e) {
    json fields;
    fields["name"] = e.name;
    fields["sku"] = e.sku;
    fields["upc_code"] = e.upc;
    fields["item_type"] = e.category;
    fields["manufacturer"] = e.manufacturer;
    fields["device"] = e.device;
    fields["supplier"] = e.supplier;
    fields["condition"] = e.condition;
    fields["physical_location"] = e.location;
    fields["description"] = e.description;
    fields["notes"] = e.notes;
    fields["reorder_level"] = e.reorderLevel;
    fields["stock_warning"] = e.stockWarning;
    fields["minimum_price"] = e.minimumPrice;
    fields["in_stock"] = e.qty;
    fields["cost_price"] = e.cost;
    fields["price"] = e.price;
    if (e.valuationIndex >= 0 && e.valuationIndex < (int)std::size(kValuationMethods)) {
        fields["valuation_method"] = kValuationMethods[e.valuationIndex].code;
    }
    const Lookups& L = *app.lookups;
    if (e.taxClassIndex >= 0 && e.taxClassIndex < (int)L.taxClasses.size()) {
        fields["tax_class"] = L.taxClasses[e.taxClassIndex].id;
    }
    return fields;
}

void saveEditor(App& app, InventoryState& s, const InventoryItem& it) {
    InventoryState::Editor& e = s.editor;
    json fields = editorFields(app, e);
    // Only send fields that actually have a value or changed, to avoid wiping data with blanks.
    for (auto i = fields.begin(); i != fields.end();) {
        bool blank = i.value().is_string() && i.value().get<std::string>().empty();
        bool existed = it.fields.count(i.key()) && !it.fields.at(i.key()).empty();
        if (blank && !existed) i = fields.erase(i);
        else ++i;
    }
    s.productSaving = true;
    e.dirty = false;
    saveFields(app, it.id, fields, "Product \"" + e.name + "\"");
}

void startNewProduct(App& app, InventoryState& s) {
    s.editor = InventoryState::Editor{};
    s.editor.valuationIndex = 0;   // FIFO
    s.editor.taxClassIndex = app.lookups->taxClasses.empty() ? -1 : 0;
    s.openProductId = 0;
    s.creatingProduct = true;
    s.productSaving = false;
}

// Sends the editor contents to POST /inventory/addnewitem and refreshes the list on success.
void createProduct(App& app, InventoryState& s) {
    json fields = editorFields(app, s.editor);
    // Blank optional strings are omitted so RepairDesk applies its own defaults.
    for (auto i = fields.begin(); i != fields.end();) {
        bool blank = i.value().is_string() && i.value().get<std::string>().empty();
        if (blank && i.key() != "name") i = fields.erase(i);
        else ++i;
    }
    s.productSaving = true;
    const std::string name = s.editor.name;
    RepairDeskApi client = app.api();
    auto job = app.beginWriteJob();
    Async::run([client, fields] {
                   std::pair<ApiResult<json>, WriteNotes> out;
                   out.second.createMissing = true;
                   out.first = client.addInventoryItem(fields, &out.second);
                   return out;
               },
               [&app, name, job](std::pair<ApiResult<json>, WriteNotes> out) {
                   InventoryState& st = S();
                   st.productSaving = false;
                   app.finishWriteJob(job);
                   reportWriteNotes(app, out.second);
                   const ApiResult<json>& r = out.first;
                   if (r.ok) {
                       app.notify("Product \"" + name + "\" created.", ui::InfoKind::Success);
                       st.creatingProduct = false;
                       st.editor.dirty = false;
                       // Open the new product once the refreshed inventory arrives (if the API returned its id).
                       if (r.value.is_object() && r.value.contains("data") && r.value["data"].is_object()) {
                           const json& d = r.value["data"];
                           if (d.contains("id")) {
                               if (d["id"].is_number()) app.pendingProductId = d["id"].get<long long>();
                               else if (d["id"].is_string()) app.pendingProductId = (long long)toNumber(d["id"].get<std::string>());
                           }
                       }
                       app.refreshInventory(st.serverKeyword);
                   } else {
                       app.notify("Could not create product: " + r.message, ui::InfoKind::Error);
                   }
               });
}

// Popup opened by the pencil next to the suggested-price pill.
void drawFormulaPopup(App& app, InventoryState& s) {
    if (s.openFormulaPopup) {
        ImGui::OpenPopup("##priceFormula");
        s.openFormulaPopup = false;
    }
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;
    ImGui::SetNextWindowSize(ImVec2(380 * dpi, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, P.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
    if (ImGui::BeginPopupModal("##priceFormula", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                                              ImGuiWindowFlags_NoMove)) {
        ui::Subtitle("Suggested price formula");
        ui::SecondaryText("price = round(cost x multiplier to the nearest step) - subtract");
        ui::VerticalSpace(8 * dpi);
        ui::Label("Multiplier");
        ui::NumberField("##mult", s.formulaMultiplier, "%.2f", -1);
        ui::Label("Round to nearest");
        ui::NumberField("##round", s.formulaRoundTo, "%.2f", -1);
        ui::Label("Subtract");
        ui::NumberField("##sub", s.formulaSubtract, "%.2f", -1);
        if (s.formulaMultiplier < 0) s.formulaMultiplier = 0;
        if (s.formulaRoundTo < 0) s.formulaRoundTo = 0;

        Settings preview = app.settings;
        preview.priceMultiplier = s.formulaMultiplier;
        preview.priceRoundTo = s.formulaRoundTo;
        preview.priceSubtract = s.formulaSubtract;
        char line[128];
        std::snprintf(line, sizeof(line), "Preview: cost %s  ->  %s", usd(s.editor.cost).c_str(),
                      usd(preview.suggestedPrice(s.editor.cost)).c_str());
        ui::VerticalSpace(4 * dpi);
        ui::SecondaryText(line);

        ui::VerticalSpace(12 * dpi);
        float btnW = 110 * dpi;
        if (ui::SubtleButton("Reset to default")) {
            s.formulaMultiplier = 2.0;
            s.formulaRoundTo = 0.2;
            s.formulaSubtract = 0.05;
        }
        ImGui::SameLine();
        ui::AlignRight(btnW * 2 + ImGui::GetStyle().ItemSpacing.x);
        if (ui::Button("Cancel", ImVec2(btnW, 0))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ui::AccentButton("Save", ImVec2(btnW, 0))) {
            app.settings.priceMultiplier = s.formulaMultiplier;
            app.settings.priceRoundTo = s.formulaRoundTo;
            app.settings.priceSubtract = s.formulaSubtract;
            app.saveSettings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void drawProductPage(App& app) {
    InventoryState& s = S();
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;
    const bool creating = s.creatingProduct;
    const InventoryItem* item = creating ? nullptr : findItem(app, s.openProductId);

    // Header
    if (ui::IconButton("backToList", icons::Back, "Back to inventory")) {
        s.openProductId = 0;
        s.creatingProduct = false;
        return;
    }
    ImGui::SameLine();
    if (!creating && !item) {
        ui::PageTitle("Product");
        ui::InfoBar(ui::InfoKind::Warning, "This product is no longer in the loaded inventory.");
        return;
    }
    if (creating) ui::PageTitle(s.editor.name.empty() ? "New product" : s.editor.name.c_str());
    else ui::PageTitle(item->name.empty() ? "Product" : item->name.c_str());
    ImGui::SameLine();
    float saveW = 140 * dpi;
    ui::AlignRight(saveW * 2 + ImGui::GetStyle().ItemSpacing.x + (s.productSaving ? 28 * dpi : 0));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * dpi);
    if (s.productSaving) { ui::Spinner("##prodSpin"); ImGui::SameLine(); }
    if (creating) {
        ImGui::BeginDisabled(s.productSaving);
        if (ui::Button("Cancel", ImVec2(saveW, 0))) {
            s.creatingProduct = false;
            ImGui::EndDisabled();
            return;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool canCreate = !s.editor.name.empty() && !s.productSaving && app.connected();
        ImGui::BeginDisabled(!canCreate);
        if (ui::AccentButton((std::string(icons::Add) + "  Create product").c_str(), ImVec2(saveW, 0))) createProduct(app, s);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canCreate && !s.productSaving) {
            ui::Tooltip(app.connected() ? "Enter a product name first" : "Connect to RepairDesk in Settings first");
        }
    } else {
        ImGui::BeginDisabled(!s.editor.dirty || s.productSaving);
        if (ui::Button("Discard", ImVec2(saveW, 0))) loadEditor(app, s, *item);
        ImGui::SameLine();
        if (ui::AccentButton((std::string(icons::Save) + "  Save changes").c_str(), ImVec2(saveW, 0))) saveEditor(app, s, *item);
        ImGui::EndDisabled();
    }

    if (creating) {
        ui::SecondaryText("Fill in the details below and press Create product to add it to RepairDesk.");
    } else {
        char idText[64];
        std::snprintf(idText, sizeof(idText), "Product ID %lld", item->id);
        ui::SecondaryText(idText);
    }
    ui::VerticalSpace(6 * dpi);

    rebuildSuggestions(app, s);
    InventoryState::Editor& e = s.editor;
    const InventoryState::Suggestions& g = s.suggest;
    auto text = [&](const char* label, const char* id, std::string& v, float w) {
        ImGui::BeginGroup();
        ui::Label(label);
        if (ui::TextField(id, v, nullptr, w)) e.dirty = true;
        ImGui::EndGroup();
    };
    auto suggest = [&](const char* label, const char* id, std::string& v, const std::vector<std::string>& options, float w) {
        ImGui::BeginGroup();
        ui::Label(label);
        if (ui::SuggestField(id, v, options, "Select or type a new value", w)) e.dirty = true;
        ImGui::EndGroup();
    };

    ImGui::BeginChild("##productScroll", ImVec2(0, 0), ImGuiChildFlags_None);

    // Primary fields --------------------------------------------------------
    if (ui::BeginCard("##primary")) {
        ui::Subtitle("Product");
        float full = ImGui::GetContentRegionAvail().x;
        float third = (full - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
        text("Name", "##name", e.name, full);

        ImGui::BeginGroup();
        ui::Label("Quantity in stock");
        if (ui::IntField("##qty", e.qty, third)) e.dirty = true;
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ui::Label("Cost");
        if (ui::NumberField("##cost", e.cost, "%.2f", third)) e.dirty = true;
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ui::Label("Price");
        {
            // Price box, then a pill with the suggested price and a pencil to edit the formula.
            const double suggested = app.settings.suggestedPrice(e.cost);
            const std::string pillText = usd(suggested);
            const float pillW = ImGui::CalcTextSize(pillText.c_str()).x + 24 * dpi;
            const float pencilW = ImGui::GetFrameHeight();
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            float priceW = third - pillW - pencilW - spacing * 2;
            if (priceW < 90 * dpi) priceW = 90 * dpi;
            if (ui::NumberField("##price", e.price, "%.2f", priceW)) e.dirty = true;
            ImGui::SameLine();
            char tip[160];
            std::snprintf(tip, sizeof(tip), "Suggested price: round(cost x %g to nearest %.2f) - %.2f. Click to use it.",
                          app.settings.priceMultiplier, app.settings.priceRoundTo, app.settings.priceSubtract);
            if (ui::PillButton("##suggestPrice", pillText.c_str(), tip)) {
                if (e.price != suggested) { e.price = suggested; e.dirty = true; }
            }
            // Tiny pencil: a frame-high hit area with a reduced glyph.
            ImGui::SameLine(0, 2 * dpi);
            ImGui::SetWindowFontScale(0.75f);
            bool editFormula = ui::IconButton("editFormula", icons::Edit, "Edit the suggested-price formula");
            ImGui::SetWindowFontScale(1.0f);
            if (editFormula) {
                s.formulaMultiplier = app.settings.priceMultiplier;
                s.formulaRoundTo = app.settings.priceRoundTo;
                s.formulaSubtract = app.settings.priceSubtract;
                s.openFormulaPopup = true;
            }
        }
        ImGui::EndGroup();

        text("SKU", "##sku", e.sku, third);
        ImGui::SameLine();
        text("UPC", "##upc", e.upc, third);
        ImGui::SameLine();
        suggest("Category", "##cat", e.category, g.categories, third);

        ImGui::BeginGroup();
        ui::Label("Description");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, P.control);
        if (ImGui::InputTextMultiline("##desc", &e.description, ImVec2(full, 90 * dpi))) e.dirty = true;
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ui::EndCard();
    }
    ui::VerticalSpace(8 * dpi);

    // Details --------------------------------------------------------------
    if (ui::BeginCard("##details")) {
        ui::Subtitle("Details");
        float full = ImGui::GetContentRegionAvail().x;
        float third = (full - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
        suggest("Manufacturer", "##man", e.manufacturer, g.manufacturers, third);
        ImGui::SameLine();
        suggest("Device", "##dev", e.device, g.devices, third);
        ImGui::SameLine();
        suggest("Supplier", "##sup", e.supplier, g.suppliers, third);
        suggest("Condition", "##cond", e.condition, g.conditions, third);
        ImGui::SameLine();
        suggest("Physical location", "##loc", e.location, g.locations, third);
        ImGui::SameLine();
        text("Minimum price", "##minp", e.minimumPrice, third);

        ImGui::BeginGroup();
        ui::Label("Valuation method");
        {
            static const std::vector<std::string> valuationLabels = [] {
                std::vector<std::string> v;
                for (const ValuationMethod& m : kValuationMethods) v.push_back(m.label);
                return v;
            }();
            if (ui::ComboField("##valuation", e.valuationIndex, valuationLabels, "Select...", third)) e.dirty = true;
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ui::Label("Tax class");
        if (g.taxClasses.empty()) {
            ImGui::BeginDisabled();
            int none = -1;
            ui::ComboField("##tax", none, g.taxClasses, app.lookupsLoading ? "Loading..." : "No tax classes", third);
            ImGui::EndDisabled();
        } else if (ui::ComboField("##tax", e.taxClassIndex, g.taxClasses, "Select a tax class", third)) {
            e.dirty = true;
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        text("Reorder level", "##reorder", e.reorderLevel, third);
        text("Stock warning", "##warn", e.stockWarning, third);
        ImGui::BeginGroup();
        ui::Label("Notes");
        ImGui::PushStyleColor(ImGuiCol_FrameBg, P.control);
        if (ImGui::InputTextMultiline("##notes", &e.notes, ImVec2(full, 70 * dpi))) e.dirty = true;
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ui::EndCard();
    }
    ui::VerticalSpace(8 * dpi);

    // Everything else RepairDesk returned (read only) ----------------------
    if (!creating && ui::BeginCard("##allFields")) {
        ui::Subtitle("All information from RepairDesk");
        ui::SecondaryText("Every field returned by the API for this product.");
        ImGui::Spacing();
        if (ImGui::BeginTable("##fieldsTable", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.65f);
            for (const auto& [k, v] : item->fields) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(P.accent, "%s", DataTable::prettifyKey(k).c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::PushTextWrapPos();
                ImGui::TextUnformatted(v.empty() ? "\xE2\x80\x94" : v.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }
        ui::EndCard();
    }
    ImGui::EndChild();
    drawFormulaPopup(app, s);
}

void drawBulkModal(App& app) {
    InventoryState& s = S();
    if (s.openBulkModal) {
        ImGui::OpenPopup("##bulkModal");
        s.openBulkModal = false;
    }

    const Palette& P = Theme::get().palette();
    ImGui::SetNextWindowSize(ImVec2(440 * app.dpiScale, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, P.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
    if (ImGui::BeginPopupModal("##bulkModal", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                                           ImGuiWindowFlags_NoMove)) {
        ui::Subtitle(bulkTitle(s.pendingAction));
        char info[128];
        std::snprintf(info, sizeof(info), "%zu item(s) selected", s.selectedIds.size());
        ui::SecondaryText(info);
        ui::VerticalSpace(8);

        bool valid = true;
        switch (s.pendingAction) {
            case BulkAction::Delete:
                ImGui::PushTextWrapPos();
                ImGui::TextUnformatted("The selected items will be permanently removed from RepairDesk. "
                                       "This cannot be undone.");
                ImGui::PopTextWrapPos();
                break;
            case BulkAction::EditCost:
                ui::Label("New cost price");
                ui::NumberField("##cost", s.bulkNumber, "%.2f", -1);
                break;
            case BulkAction::EditPrice:
                ui::Label("New retail price");
                ui::NumberField("##price", s.bulkNumber, "%.2f", -1);
                break;
            case BulkAction::EditQuantity:
                ui::Label("New quantity in stock");
                ui::IntField("##qty", s.bulkInt, -1);
                break;
            case BulkAction::EditCategory:
                ui::Label("New category");
                ui::TextField("##cat", s.bulkText, "e.g. Accessories", -1);
                valid = !s.bulkText.empty();
                break;
            default: break;
        }

        ui::VerticalSpace(12);
        float btnW = 120 * app.dpiScale;
        ui::AlignRight(btnW * 2 + ImGui::GetStyle().ItemSpacing.x);
        if (s.bulkRunning) {
            ui::Spinner("##bulkspin");
            ImGui::SameLine();
            ui::SecondaryText("Applying changes...");
        } else {
            if (ui::Button("Cancel", ImVec2(btnW, 0))) {
                s.pendingAction = BulkAction::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!valid);
            bool confirm = s.pendingAction == BulkAction::Delete ? ui::DangerButton("Delete", ImVec2(btnW, 0))
                                                                 : ui::AccentButton("Apply", ImVec2(btnW, 0));
            ImGui::EndDisabled();
            if (confirm) runBulkAction(app);
        }
        if (s.pendingAction == BulkAction::None && !s.bulkRunning) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace

namespace pages {

void inventory(App& app) {
    InventoryState& s = S();
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;

    if (!s.columnsInitialised) {
        setupColumns(s.table);
        s.columnsInitialised = true;
    }
    if (s.lastInventorySize != app.inventory.size()) {
        std::vector<FlatRecord> recs;
        recs.reserve(app.inventory.size());
        for (const auto& it : app.inventory) recs.push_back(it.fields);
        setupColumns(s.table);
        s.table.discoverColumns(recs);
        s.lastInventorySize = app.inventory.size();
        s.viewDirty = true;
    }
    if (s.lastInventoryRevision != app.inventoryRevision) {
        s.lastInventoryRevision = app.inventoryRevision;
        s.viewDirty = true;   // items were replaced in place (e.g. after a stock take)
    }
    if (s.search != s.lastSearch) {
        s.lastSearch = s.search;
        s.viewDirty = true;
    }
    if (s.table.sortDirty) {
        s.table.sortDirty = false;
        s.viewDirty = true;
    }
    if (s.table.filterDirty) {
        s.table.filterDirty = false;
        s.viewDirty = true;
    }
    if (s.viewDirty) rebuildView(app);

    // Product page requested from elsewhere (e.g. global search)
    if (app.pendingProductId != 0) {
        const InventoryItem* it = findItem(app, app.pendingProductId);
        if (it) {
            s.openProductId = app.pendingProductId;
            app.pendingProductId = 0;
            loadEditor(app, s, *it);
        } else if (!app.inventoryLoading) {
            app.pendingProductId = 0;   // e.g. a freshly created item that the server did not return yet
        }
    }
    if (s.openProductId != 0 || s.creatingProduct) {
        drawProductPage(app);
        return;
    }

    // Prune selections of items that no longer exist.
    if (!s.selectedIds.empty()) {
        std::set<long long> existing;
        for (const auto& it : app.inventory) existing.insert(it.id);
        for (auto it = s.selectedIds.begin(); it != s.selectedIds.end();) {
            it = existing.count(*it) ? std::next(it) : s.selectedIds.erase(it);
        }
    }

    const float sidebarWidth = app.stockTakeOpen ? pages::stockTakeWidth(app) : 0.0f;
    const float gap = app.stockTakeOpen ? 16.0f * dpi : 0.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImGui::BeginChild("##inventoryMain", ImVec2(avail.x - sidebarWidth - gap, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // Header -----------------------------------------------------------
        ui::PageTitle("Inventory");
        ImGui::SameLine();
        if (app.inventoryLoading) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8 * dpi);
            ui::Spinner("##invspin");
        }

        // Toolbar row
        ImGui::BeginGroup();
        float searchW = 320.0f * dpi;
        if (ui::SearchField("invSearch", s.search, "Search any field (name, SKU, description...)", searchW)) {
            // Enter pressed. Filtering is local across every field; the server keyword
            // (which only matches id, name and SKU) is used only when the inventory is
            // capped in Settings and therefore not fully loaded.
            if (app.settings.inventoryPageSize > 0) {
                s.serverKeyword = s.search;
                app.refreshInventory(s.serverKeyword);
            }
        }
        ImGui::SameLine();
        if (ui::Button((std::string(icons::Refresh) + "  Refresh").c_str())) {
            app.refreshInventory(s.serverKeyword);
        }
        ImGui::SameLine();
        if (ui::AccentButton((std::string(icons::Add) + "  New product").c_str())) {
            startNewProduct(app, s);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ui::Tooltip("Add a new product to RepairDesk inventory");
        if (!s.serverKeyword.empty()) {
            ImGui::SameLine();
            if (ui::SubtleButton((std::string(icons::Close) + "  Clear server filter").c_str())) {
                s.serverKeyword.clear();
                s.search.clear();
                app.refreshInventory();
            }
        }

        // Right aligned actions
        std::string bulkLabel = std::string(icons::Edit) + "  Bulk actions  " + icons::ChevronDown;
        std::string stockLabel = std::string(icons::Scan) + "  Stock take";
        float bulkW = ImGui::CalcTextSize(bulkLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        float stockW = ImGui::CalcTextSize(stockLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine();
        ui::AlignRight(bulkW + stockW + ImGui::GetStyle().ItemSpacing.x);

        ImGui::BeginDisabled(s.selectedIds.empty() || s.bulkRunning);
        if (ui::Button(bulkLabel.c_str())) ImGui::OpenPopup("##bulkMenu");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && s.selectedIds.empty()) {
            ui::Tooltip("Select one or more items to enable bulk actions");
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        if (ImGui::BeginPopup("##bulkMenu")) {
            auto item = [&](const char* icon, const char* label, BulkAction action) {
                std::string text = std::string(icon) + "  " + label;
                if (ImGui::Selectable(text.c_str(), false, 0, ImVec2(200 * dpi, 30 * dpi))) {
                    s.pendingAction = action;
                    s.openBulkModal = true;
                    s.bulkNumber = 0.0;
                    s.bulkInt = 0;
                    s.bulkText.clear();
                }
            };
            item(icons::Edit, "Edit price", BulkAction::EditPrice);
            item(icons::Edit, "Edit cost", BulkAction::EditCost);
            item(icons::Edit, "Edit quantity", BulkAction::EditQuantity);
            item(icons::Edit, "Edit category", BulkAction::EditCategory);
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
            item(icons::Delete, "Delete", BulkAction::Delete);
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        ImGui::SameLine();
        if (app.stockTakeOpen ? ui::AccentButton(stockLabel.c_str()) : ui::Button(stockLabel.c_str())) {
            app.stockTakeOpen = !app.stockTakeOpen;
        }
        ImGui::EndGroup();

        ui::VerticalSpace(4 * dpi);

        if (!app.inventoryError.empty()) {
            ui::InfoBar(ui::InfoKind::Error, app.inventoryError);
            ui::VerticalSpace(4 * dpi);
        }

        // Table ------------------------------------------------------------
        float statusH = ImGui::GetTextLineHeightWithSpacing() + 8 * dpi;
        ImVec2 tableSize(0, ImGui::GetContentRegionAvail().y - statusH);

        if (app.inventory.empty() && !app.inventoryLoading) {
            ImGui::BeginChild("##emptyInv", tableSize);
            if (!app.connected()) {
                ui::EmptyState(icons::Key, "Connect to RepairDesk",
                               "Open Settings to enter an API key or sign in with OAuth2.");
            } else if (app.inventoryLoadedOnce) {
                ui::EmptyState(icons::Package, "No inventory items", "Try a different search or refresh.");
            }
            ImGui::EndChild();
        } else {
            s.selectionVec.resize(s.view.size());
            for (size_t i = 0; i < s.view.size(); ++i) {
                s.selectionVec[i] = s.selectedIds.count(app.inventory[s.view[i]].id) > 0;
            }

            DataTable::DrawOptions opts;
            opts.selectable = true;
            opts.selection = &s.selectionVec;
            opts.size = tableSize;
            opts.rowHeight = 40.0f * dpi;
            opts.cellRenderer = [&](int row, const std::string& key, const std::string& value) -> bool {
                const InventoryItem& it = app.inventory[s.view[row]];
                if (key == "price" || key == "cost_price" || key == "minimum_price" || key == "retail_price") {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "$%.2f", toNumber(value));
                    ImGui::TextUnformatted(buf);
                    return true;
                }
                if (key == "in_stock") {
                    int qty = (int)toNumber(value);   // `value` may be a staged (unsaved) edit
                    double warning = toNumber(it.fields.count("stock_warning") ? it.fields.at("stock_warning") : "");
                    ImVec4 color = P.text;
                    if (qty <= 0) color = P.danger;
                    else if (warning > 0 && qty <= warning) color = ImVec4(0.99f, 0.76f, 0.17f, 1.0f);
                    ImGui::TextColored(color, "%d", qty);
                    return true;
                }
                if (key == "name") {
                    ui::BoldText(value.c_str());
                    return true;
                }
                return false;
            };
            // Single click on a cell edits it in place (Tab moves along the row, the row is
            // saved when it is left); a double click opens the product page.
            opts.isEditable = [](int, const std::string& key) { return isEditableKey(key); };
            opts.onRowEdited = [&](int row, const DataTable::RowEdits& edits) {
                saveRowEdits(app, app.inventory[s.view[row]], edits);
            };
            opts.onRowDoubleClicked = [&](int row) {
                const InventoryItem& it = app.inventory[s.view[row]];
                s.openProductId = it.id;
                loadEditor(app, s, it);
            };
            // Header funnel icons: the values offered are those of the whole loaded inventory.
            opts.filterValues = [&](const std::string& key, std::vector<std::string>& values) {
                values.reserve(app.inventory.size());
                for (const InventoryItem& it : app.inventory) {
                    auto f = it.fields.find(key);
                    values.push_back(f == it.fields.end() ? std::string() : f->second);
                }
            };
            // Inline editing dropdowns: match the product page fields.
            opts.dropdownOptions = [&](const std::string& key, std::vector<std::string>& options) {
                if (key == "item_type") options = s.suggest.categories;
                else if (key == "manufacturer") options = s.suggest.manufacturers;
                else if (key == "device") options = s.suggest.devices;
                else if (key == "supplier") options = s.suggest.suppliers;
                else if (key == "condition") options = s.suggest.conditions;
                else if (key == "physical_location") options = s.suggest.locations;
                else if (key == "tax_class") options = s.suggest.taxClasses;
                else if (key == "valuation_method") {
                    for (const auto& m : kValuationMethods) options.push_back(m.label);
                }
            };
            s.table.draw((int)s.view.size(), [&](int row) -> const FlatRecord& { return app.inventory[s.view[row]].fields; }, opts);

            for (size_t i = 0; i < s.view.size(); ++i) {
                long long id = app.inventory[s.view[i]].id;
                if (s.selectionVec[i]) s.selectedIds.insert(id);
                else s.selectedIds.erase(id);
            }
        }

        // Status bar -------------------------------------------------------
        ui::VerticalSpace(2 * dpi);
        char status[256];
        std::snprintf(status, sizeof(status), "%zu of %zu items shown  |  %zu selected", s.view.size(),
                      app.inventory.size(), s.selectedIds.size());
        ui::SecondaryText(status);
        if (s.table.hasFilters()) {
            // Column filters set with the funnel icons in the headers.
            ImGui::SameLine();
            char clearLabel[64];
            std::snprintf(clearLabel, sizeof(clearLabel), "%s  Clear %d column filter%s", icons::Filter, s.table.filterCount(),
                          s.table.filterCount() == 1 ? "" : "s");
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
            if (ui::SubtleButton(clearLabel)) s.table.clearFilters();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ui::Tooltip("Removes the value filters set with the funnel icons in the column headers");
        }
        if (!app.inventoryStatus.empty()) {
            ImGui::SameLine();
            ui::AlignRight(ImGui::CalcTextSize(app.inventoryStatus.c_str()).x);
            ui::SecondaryText(app.inventoryStatus.c_str());
        }
    }
    ImGui::EndChild();

    if (app.stockTakeOpen) {
        ImGui::SameLine(0, gap);
        pages::stockTake(app);
    }

    drawBulkModal(app);
}

} // namespace pages
