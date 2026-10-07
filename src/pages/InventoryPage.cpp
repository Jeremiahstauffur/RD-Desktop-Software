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
        bool dirty = false;
    } editor;
    bool productSaving = false;
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

bool contains(const std::string& hay, const std::string& needle) {
    return lower(hay).find(needle) != std::string::npos;
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
    std::string needle = lower(s.search);
    for (size_t i = 0; i < app.inventory.size(); ++i) {
        const InventoryItem& it = app.inventory[i];
        if (!needle.empty() && !(contains(it.name, needle) || contains(it.sku, needle) || contains(it.upc, needle) ||
                                 contains(it.category, needle) || contains(it.manufacturer, needle) ||
                                 contains(it.device, needle))) {
            continue;
        }
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

    Async::run(
        [client, targets, action, number, integer, text] {
            BulkOutcome out;
            for (const InventoryItem& it : targets) {
                ApiResult<json> r;
                if (action == BulkAction::Delete) {
                    r = client.deleteInventoryItem(it.id);
                } else {
                    json overrides;
                    switch (action) {
                        case BulkAction::EditCost: overrides["cost_price"] = number; break;
                        case BulkAction::EditPrice: overrides["price"] = number; break;
                        case BulkAction::EditQuantity: overrides["in_stock"] = integer; break;
                        case BulkAction::EditCategory: overrides["item_type"] = text; break;
                        default: break;
                    }
                    json fields = it.toUpdatePayload(overrides);
                    r = client.updateInventoryItem(it.id, fields);
                }
                if (r.ok) {
                    out.succeeded++;
                } else {
                    out.failed++;
                    if (out.firstError.empty()) out.firstError = r.message;
                }
            }
            return out;
        },
        [&app, action](BulkOutcome out) {
            InventoryState& st = S();
            st.bulkRunning = false;
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
void saveFields(App& app, long long id, const json& fields, const std::string& what) {
    json payload;
    for (InventoryItem& it : app.inventory) {
        if (it.id != id) continue;
        for (auto it2 = fields.begin(); it2 != fields.end(); ++it2) {
            it.raw[it2.key()] = it2.value();
        }
        it = InventoryItem::fromJson(it.raw);
        payload = it.toUpdatePayload(fields);
        break;
    }
    S().viewDirty = true;

    if (payload.is_null() || payload.empty()) {
        payload = fields;
    }

    RepairDeskApi client = app.api();
    Async::run([client, id, payload] { return client.updateInventoryItem(id, payload); },
               [&app, what](ApiResult<json> r) {
                   S().productSaving = false;
                   if (r.ok) {
                       app.notify(what + " saved.", ui::InfoKind::Success);
                   } else {
                       app.notify(what + " failed: " + r.message, ui::InfoKind::Error);
                       app.refreshInventory(S().serverKeyword);
                   }
               });
}

void saveSingleField(App& app, const InventoryItem& item, const std::string& key, const std::string& text) {
    json fields;
    fields["name"] = item.name;   // the API requires a name on update
    fields[key] = fieldValue(key, text);
    saveFields(app, item.id, fields, DataTable::prettifyKey(key));
}

const InventoryItem* findItem(const App& app, long long id) {
    for (const InventoryItem& it : app.inventory) if (it.id == id) return &it;
    return nullptr;
}

void loadEditor(InventoryState& s, const InventoryItem& it) {
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
    e.dirty = false;
}

// Builds the JSON payload for the product editor (shared by update and create).
json editorFields(const InventoryState::Editor& e) {
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
    return fields;
}

void saveEditor(App& app, InventoryState& s, const InventoryItem& it) {
    InventoryState::Editor& e = s.editor;
    json fields = editorFields(e);
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

void startNewProduct(InventoryState& s) {
    s.editor = InventoryState::Editor{};
    s.openProductId = 0;
    s.creatingProduct = true;
    s.productSaving = false;
}

// Sends the editor contents to POST /inventory/addnewitem and refreshes the list on success.
void createProduct(App& app, InventoryState& s) {
    json fields = editorFields(s.editor);
    // Blank optional strings are omitted so RepairDesk applies its own defaults.
    for (auto i = fields.begin(); i != fields.end();) {
        bool blank = i.value().is_string() && i.value().get<std::string>().empty();
        if (blank && i.key() != "name") i = fields.erase(i);
        else ++i;
    }
    s.productSaving = true;
    const std::string name = s.editor.name;
    RepairDeskApi client = app.api();
    Async::run([client, fields] { return client.addInventoryItem(fields); },
               [&app, name](ApiResult<json> r) {
                   InventoryState& st = S();
                   st.productSaving = false;
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

void drawProductPage(App& app) {
    InventoryState& s = S();
    const Palette& P = Theme::get().palette();
    const Fonts& F = Theme::get().fonts();
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
        if (ui::Button("Discard", ImVec2(saveW, 0))) loadEditor(s, *item);
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

    InventoryState::Editor& e = s.editor;
    auto text = [&](const char* label, const char* id, std::string& v, float w) {
        ImGui::BeginGroup();
        ui::Label(label);
        if (ui::TextField(id, v, nullptr, w)) e.dirty = true;
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
        if (ui::NumberField("##price", e.price, "%.2f", third)) e.dirty = true;
        ImGui::EndGroup();

        text("SKU", "##sku", e.sku, third);
        ImGui::SameLine();
        text("UPC", "##upc", e.upc, third);
        ImGui::SameLine();
        text("Category", "##cat", e.category, third);

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
        text("Manufacturer", "##man", e.manufacturer, third);
        ImGui::SameLine();
        text("Device", "##dev", e.device, third);
        ImGui::SameLine();
        text("Supplier", "##sup", e.supplier, third);
        text("Condition", "##cond", e.condition, third);
        ImGui::SameLine();
        text("Physical location", "##loc", e.location, third);
        ImGui::SameLine();
        text("Minimum price", "##minp", e.minimumPrice, third);
        text("Reorder level", "##reorder", e.reorderLevel, third);
        ImGui::SameLine();
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
    (void)F;
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
    if (s.viewDirty) rebuildView(app);

    // Product page requested from elsewhere (e.g. global search)
    if (app.pendingProductId != 0) {
        const InventoryItem* it = findItem(app, app.pendingProductId);
        if (it) {
            s.openProductId = app.pendingProductId;
            app.pendingProductId = 0;
            loadEditor(s, *it);
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

    const float sidebarWidth = app.stockTakeOpen ? 380.0f * dpi : 0.0f;
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
        if (ui::SearchField("invSearch", s.search, "Search name, SKU, UPC, category...", searchW)) {
            // Enter pressed: also ask the server (keyword works on id, name and SKU)
            s.serverKeyword = s.search;
            app.refreshInventory(s.serverKeyword);
        }
        ImGui::SameLine();
        if (ui::Button((std::string(icons::Refresh) + "  Refresh").c_str())) {
            app.refreshInventory(s.serverKeyword);
        }
        ImGui::SameLine();
        if (ui::AccentButton((std::string(icons::Add) + "  New product").c_str())) {
            startNewProduct(s);
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
                    int qty = it.inStock;
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
            opts.isEditable = [](int, const std::string& key) { return isEditableKey(key); };
            opts.onCellEdited = [&](int row, const std::string& key, const std::string& newValue) {
                saveSingleField(app, app.inventory[s.view[row]], key, newValue);
            };
            opts.onRowClicked = [&](int row) {
                const InventoryItem& it = app.inventory[s.view[row]];
                s.openProductId = it.id;
                loadEditor(s, it);
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
