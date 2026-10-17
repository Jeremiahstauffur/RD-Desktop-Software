#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "App.h"
#include "Async.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

namespace {

struct ScanEntry {
    long long id = 0;
    std::string name;
    std::string sku;
    std::string upc;
    int dbQty = 0;        // quantity currently stored in RepairDesk
    int scanned = 0;      // how many times the item has been scanned
};

constexpr double kSuggestDelay = 1.0;   // seconds without typing before the product list appears
constexpr size_t kSuggestLimit = 25;
constexpr float kPanelWidth = 380.0f;      // stock take sidebar (logical px)
constexpr float kBulkPanelWidth = 300.0f;  // bulk-edit panel to its left
constexpr float kPanelGap = 16.0f;

// Bulk edit of the scanned items: one drop-down per field, -1 = leave the field alone.
struct BulkEdit {
    int category = -1;
    int location = -1;
    int supplier = -1;
    std::vector<std::string> categories, locations, suppliers;   // values in use in the inventory
    size_t inventorySize = (size_t)-1;
    unsigned inventoryRevision = 0;
    const void* lookups = nullptr;

    bool any() const { return category >= 0 || location >= 0 || supplier >= 0; }
    int count() const { return (category >= 0) + (location >= 0) + (supplier >= 0); }
    void reset() { category = location = supplier = -1; }
    // Fields sent with every scanned item (names; the API layer resolves ids).
    json overrides() const {
        json o = json::object();
        if (category >= 0 && category < (int)categories.size()) o["item_type"] = categories[category];
        if (location >= 0 && location < (int)locations.size()) o["physical_location"] = locations[location];
        if (supplier >= 0 && supplier < (int)suppliers.size()) o["supplier"] = suppliers[supplier];
        return o;
    }
};

struct StockTakeState {
    std::string input;
    std::string lastInput;          // input as of the previous frame (detects typing)
    double typedAt = 0.0;           // time of the last keystroke in the scan field
    std::vector<int> suggestions;   // inventory indices shown under the field
    bool suggestReady = false;      // the pause elapsed and `suggestions` reflects `input`
    unsigned suggestRevision = 0;   // inventoryRevision the suggestions were built for
    std::vector<ScanEntry> entries;
    bool focusInput = true;
    double errorUntil = 0.0;
    std::string errorText;
    bool applying = false;
    long long editingId = 0;        // entry whose scanned count is being typed
    std::string editText;
    bool focusEdit = false;
    BulkEdit bulk;
};

StockTakeState& S() {
    static StockTakeState s;
    return s;
}

void clearSuggestions(StockTakeState& s) {
    s.suggestions.clear();
    s.suggestReady = false;
    s.typedAt = 0.0;
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Adds `extra` names that are not already in `values` (case-insensitive) and re-sorts.
void mergeValues(std::vector<std::string>& values, const std::vector<std::string>& extra) {
    for (const std::string& e : extra) {
        if (e.empty()) continue;
        bool known = false;
        for (const std::string& v : values) {
            if (lowerAscii(v) == lowerAscii(e)) { known = true; break; }
        }
        if (!known) values.push_back(e);
    }
    std::sort(values.begin(), values.end(), [](const std::string& a, const std::string& b) { return lowerAscii(a) < lowerAscii(b); });
}

// Keeps the bulk-edit drop-down lists in step with the inventory and the reference data.
// A selection is kept by name across rebuilds so that a refresh does not drop it.
void updateBulkLists(App& app, BulkEdit& b) {
    const Lookups& L = *app.lookups;
    if (b.inventorySize == app.inventory.size() && b.inventoryRevision == app.inventoryRevision && b.lookups == &L) return;
    b.inventorySize = app.inventory.size();
    b.inventoryRevision = app.inventoryRevision;
    b.lookups = &L;

    auto keep = [](const std::vector<std::string>& list, int idx) { return idx >= 0 && idx < (int)list.size() ? list[idx] : std::string(); };
    auto restore = [](const std::vector<std::string>& list, const std::string& name) {
        if (name.empty()) return -1;
        for (size_t i = 0; i < list.size(); ++i) if (lowerAscii(list[i]) == lowerAscii(name)) return (int)i;
        return -1;
    };
    const std::string cat = keep(b.categories, b.category), loc = keep(b.locations, b.location), sup = keep(b.suppliers, b.supplier);

    b.categories = app.inventoryValues("item_type");
    b.locations = app.inventoryValues("physical_location");
    mergeValues(b.locations, L.locations);
    b.suppliers = app.inventoryValues("supplier");
    std::vector<std::string> supplierNames;
    for (const NamedId& v : L.suppliers) supplierNames.push_back(v.name);
    mergeValues(b.suppliers, supplierNames);

    b.category = restore(b.categories, cat);
    b.location = restore(b.locations, loc);
    b.supplier = restore(b.suppliers, sup);
}

// Adds one scan of `item` to the list (or bumps its count) and moves it to the top.
void addScan(const InventoryItem& item) {
    StockTakeState& s = S();
    s.errorUntil = 0.0;
    for (ScanEntry& e : s.entries) {
        if (e.id == item.id) {
            e.scanned++;
            e.dbQty = item.inStock;
            // Move the most recently scanned entry to the top.
            ScanEntry copy = e;
            s.entries.erase(s.entries.begin() + (&e - &s.entries[0]));
            s.entries.insert(s.entries.begin(), copy);
            return;
        }
    }
    ScanEntry e;
    e.id = item.id;
    e.name = item.name;
    e.sku = item.sku;
    e.upc = item.upc;
    e.dbQty = item.inStock;
    e.scanned = 1;
    s.entries.insert(s.entries.begin(), e);
}

// Enter pressed: exact SKU, then exact UPC, then exact name.
void handleScan(App& app) {
    StockTakeState& s = S();
    std::string code = s.input;
    s.input.clear();
    s.lastInput.clear();
    s.focusInput = true;
    clearSuggestions(s);
    if (code.find_first_not_of(" \t\r\n") == std::string::npos) return;

    int idx = app.findInventoryByCode(code);
    if (idx < 0) {
        MessageBeep(MB_ICONHAND);
        s.errorText = "No item found";
        s.errorUntil = ImGui::GetTime() + 4.0;
        return;
    }
    addScan(app.inventory[idx]);
}

// A product picked from the suggestion list under the scan field.
void pickSuggestion(App& app, int idx) {
    StockTakeState& s = S();
    s.input.clear();
    s.lastInput.clear();
    s.focusInput = true;
    clearSuggestions(s);
    if (idx >= 0 && idx < (int)app.inventory.size()) addScan(app.inventory[idx]);
}

// Rebuilds the suggestion list once the user has paused typing for kSuggestDelay.
void updateSuggestions(App& app) {
    StockTakeState& s = S();
    if (s.input != s.lastInput) {
        s.lastInput = s.input;
        clearSuggestions(s);
        if (s.input.find_first_not_of(" \t\r\n") != std::string::npos) s.typedAt = ImGui::GetTime();
        return;
    }
    if (s.typedAt <= 0.0 || ImGui::GetTime() - s.typedAt < kSuggestDelay) return;
    if (!s.suggestReady || s.suggestRevision != app.inventoryRevision) {
        s.suggestions = app.searchInventory(s.input, kSuggestLimit);
        s.suggestRevision = app.inventoryRevision;
        s.suggestReady = true;
    }
}

// Drop-down window under the scan field listing the products that match the typed text.
// Returns the inventory index of a clicked product, or -1.
int drawSuggestions(App& app, const ImVec2& fieldMin, const ImVec2& fieldMax) {
    StockTakeState& s = S();
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;
    if (!s.suggestReady) return -1;

    const float rowH = ImGui::GetTextLineHeight() * 2 + 10 * dpi;
    const size_t rows = s.suggestions.empty() ? 1 : s.suggestions.size();
    const float listH = std::min((float)rows, 7.0f) * rowH + 8 * dpi;
    ImGui::SetNextWindowPos(ImVec2(fieldMin.x, fieldMax.y + 2.0f));
    ImGui::SetNextWindowSize(ImVec2(fieldMax.x - fieldMin.x, listH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, P.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, P.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * dpi, 4 * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##stockSuggest", nullptr, flags);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    const bool hovering = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows);

    int picked = -1;
    if (s.suggestions.empty()) {
        ui::SecondaryText("No products contain this text. Press Enter to match an exact SKU, UPC or name.");
    }
    for (size_t i = 0; i < s.suggestions.size(); ++i) {
        const int idx = s.suggestions[i];
        if (idx < 0 || idx >= (int)app.inventory.size()) continue;
        const InventoryItem& it = app.inventory[idx];
        ImGui::PushID((int)i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH - 4 * dpi))) picked = idx;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float x = pos.x + 6 * dpi;
        const float right = pos.x + ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x;
        dl->AddText(ImVec2(x, pos.y + 3 * dpi), Theme::u32(P.text), it.name.c_str());
        std::string codes = it.sku;
        if (!it.upc.empty()) codes += (codes.empty() ? "" : "  |  ") + std::string("UPC ") + it.upc;
        dl->AddText(ImVec2(x, pos.y + 3 * dpi + ImGui::GetTextLineHeight()), Theme::u32(P.textSecondary), codes.c_str());
        char qty[48];
        std::snprintf(qty, sizeof(qty), "%d in stock", it.inStock);
        float qtyW = ImGui::CalcTextSize(qty).x;
        dl->AddText(ImVec2(right - qtyW - 6 * dpi, pos.y + 3 * dpi), Theme::u32(P.textSecondary), qty);
        ImGui::PopID();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // A click anywhere else (or Escape) hides the list until the user types again.
    bool clickedOutside = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovering &&
                          !ImGui::IsMouseHoveringRect(fieldMin, fieldMax);
    if (clickedOutside || ImGui::IsKeyPressed(ImGuiKey_Escape)) clearSuggestions(s);
    return picked;
}

// Commits the typed scanned count of the entry being edited (if any).
void commitCountEdit(StockTakeState& s) {
    if (s.editingId == 0) return;
    for (ScanEntry& e : s.entries) {
        if (e.id != s.editingId) continue;
        const std::string& t = s.editText;
        size_t a = t.find_first_not_of(" \t");
        if (a != std::string::npos) {
            char* end = nullptr;
            long v = std::strtol(t.c_str() + a, &end, 10);
            if (end != t.c_str() + a) e.scanned = (int)std::max(0L, std::min(v, 999999L));
        }
        break;
    }
    s.editingId = 0;
    s.editText.clear();
}

struct TargetEntry {
    InventoryItem item;
    int scanned = 0;   // counted quantity (what the stock should end up as)
    int change = 0;    // scanned - quantity stored in RepairDesk; this is what the API wants
};

void applyToRepairDesk(App& app) {
    StockTakeState& s = S();
    if (s.entries.empty() || s.applying) return;
    s.applying = true;
    // Bulk edit (category / location / supplier) applies to every scanned item on Submit.
    const json overrides = app.stockTakeBulkEdit ? s.bulk.overrides() : json::object();
    const bool bulkEdit = !overrides.empty();

    std::vector<TargetEntry> targets;
    targets.reserve(s.entries.size());
    for (const ScanEntry& e : s.entries) {
        TargetEntry t;
        t.scanned = e.scanned;
        int idx = -1;
        for (size_t i = 0; i < app.inventory.size(); ++i) {
            if (app.inventory[i].id == e.id) {
                idx = (int)i;
                break;
            }
        }
        if (idx >= 0) {
            t.item = app.inventory[idx];
        } else {
            t.item.id = e.id;
            t.item.name = e.name;
            t.item.sku = e.sku;
            t.item.upc = e.upc;
            t.item.inStock = e.dbQty;
        }
        t.change = t.scanned - t.item.inStock;
        targets.push_back(std::move(t));
    }

    RepairDeskApi client = app.api();

    struct Outcome {
        int ok = 0;
        int failed = 0;
        std::string firstError;
        std::vector<InventoryItem> refreshed;                     // items re-read from the server after the update
        std::vector<InventoryItem> refreshedFailed;               // re-read items that did not reach the scanned quantity
        std::vector<std::pair<long long, int>> updatedQuantities; // fallback when the re-read failed
        std::vector<std::string> notes;                           // distinct write notes (dropped values...)
        bool createdReferences = false;
    };
    auto job = app.beginWriteJob(targets.size());
    Async::run(
        [client, targets, job, overrides, bulkEdit] {
            Outcome out;
            for (const TargetEntry& t : targets) {
                if (t.change == 0 && !bulkEdit) {
                    // Already at the counted quantity; nothing to send.
                    out.ok++;
                    out.updatedQuantities.push_back({t.item.id, t.scanned});
                    job->done++;
                    continue;
                }
                // PUT /inventory/{id} treats `in_stock` as an adjustment; saveInventoryItem
                // lands the stock on the scanned quantity and re-reads the item to verify.
                // The bulk-edit fields ride along in the same request.
                WriteNotes notes;
                ApiResult<InventoryItem> r = client.saveInventoryItem(t.item.id, t.item.toUpdatePayload(overrides), t.item.inStock,
                                                                       t.scanned, &notes);
                for (const std::string& m : notes.messages) {
                    if (std::find(out.notes.begin(), out.notes.end(), m) == out.notes.end()) out.notes.push_back(m);
                }
                if (notes.createdReferences) out.createdReferences = true;
                if (r.ok && r.value.id == 0) {
                    out.ok++;   // written but could not be re-read; assume the server applied it
                    out.updatedQuantities.push_back({t.item.id, t.scanned});
                } else if (r.ok) {
                    out.ok++;
                    out.refreshed.push_back(std::move(r.value));
                } else {
                    out.failed++;
                    if (out.firstError.empty()) out.firstError = "\"" + t.item.name + "\": " + r.message;
                    if (r.value.id != 0) out.refreshedFailed.push_back(std::move(r.value));
                }
                job->done++;
            }
            return out;
        },
        [&app, job, bulkEdit](Outcome out) {
            StockTakeState& st = S();
            st.applying = false;
            app.finishWriteJob(job);
            for (const std::string& m : out.notes) app.notify(m, ui::InfoKind::Warning);
            if (out.createdReferences) app.refreshLookups();
            // Updated items leave the panel so that a re-submit only retries the failed ones.
            auto removeEntry = [&](long long id) {
                for (size_t i = 0; i < st.entries.size(); ++i) {
                    if (st.entries[i].id == id) {
                        st.entries.erase(st.entries.begin() + i);
                        break;
                    }
                }
            };
            for (const InventoryItem& fresh : out.refreshed) {
                app.replaceInventoryItem(fresh);
                removeEntry(fresh.id);
            }
            // Failed items stay in the panel; their database quantity reflects the server's state
            // so that a re-submit computes the right change.
            for (const InventoryItem& fresh : out.refreshedFailed) {
                app.replaceInventoryItem(fresh);
                for (ScanEntry& e : st.entries) {
                    if (e.id == fresh.id) e.dbQty = fresh.inStock;
                }
            }
            for (const auto& [id, newQty] : out.updatedQuantities) {
                for (InventoryItem& it : app.inventory) {
                    if (it.id == id) {
                        it.inStock = newQty;
                        it.fields["in_stock"] = std::to_string(newQty);
                        if (it.raw.is_object()) it.raw["in_stock"] = newQty;
                        app.inventoryRevision++;
                        break;
                    }
                }
                removeEntry(id);
            }
            char buf[256];
            if (out.failed == 0) {
                std::snprintf(buf, sizeof(buf), "Stock take submitted: %d item(s) updated%s.", out.ok,
                              bulkEdit ? " (quantities and bulk-edit fields)" : "");
                app.notify(buf, ui::InfoKind::Success);
                st.entries.clear();
                st.bulk.reset();   // the chosen values were applied; start the next count clean
            } else {
                std::snprintf(buf, sizeof(buf), "Stock take: %d updated, %d failed. %s", out.ok, out.failed,
                              out.firstError.c_str());
                app.notify(buf, ui::InfoKind::Error);
            }
        });
}

// Panel to the left of the stock take: three drop-downs whose values are written to every
// scanned item together with the quantities when Submit is pressed.
void drawBulkPanel(App& app) {
    StockTakeState& s = S();
    BulkEdit& b = s.bulk;
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;
    updateBulkLists(app, b);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, P.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::BeginChild("##stockBulk", ImVec2(kBulkPanelWidth * dpi, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ui::Subtitle("Bulk edit");
    ImGui::SameLine();
    ui::AlignRight(ImGui::GetFrameHeight());
    if (ui::IconButton("closeBulk", icons::Close, "Close bulk edit")) app.stockTakeBulkEdit = false;
    ImGui::PushTextWrapPos(0.0f);
    ui::SecondaryText("Applied to every scanned item when you press Submit. Fields left on \"No change\" keep their current value.");
    ImGui::PopTextWrapPos();
    ui::VerticalSpace(8 * dpi);

    // One drop-down per field; the x next to a chosen value puts it back to "No change".
    auto field = [&](const char* label, const char* id, int& index, const std::vector<std::string>& options, const char* fieldKey) {
        ImGui::PushID(id);
        ui::Label(label);
        const float clearW = index >= 0 ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x : 0.0f;
        const float w = ImGui::GetContentRegionAvail().x - clearW;
        if (options.empty()) {
            ImGui::BeginDisabled();
            int none = -1;
            ui::ComboField("##combo", none, options, app.inventory.empty() ? "Inventory not loaded" : "No values in the inventory", w);
            ImGui::EndDisabled();
        } else {
            ui::ComboField("##combo", index, options, "No change", w);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && index < 0) {
                char tip[128];
                std::snprintf(tip, sizeof(tip), "Every %s value already used in your inventory", fieldKey);
                ui::Tooltip(tip);
            }
        }
        if (index >= 0) {
            ImGui::SameLine();
            if (ui::IconButton("clear", icons::Close, "No change")) index = -1;
        }
        ui::VerticalSpace(6 * dpi);
        ImGui::PopID();
    };
    field("Category", "cat", b.category, b.categories, "category");
    field("Location", "loc", b.location, b.locations, "location");
    field("Supplier", "sup", b.supplier, b.suppliers, "supplier");

    ui::Divider();
    ui::VerticalSpace(4 * dpi);
    char summary[160];
    ImGui::PushTextWrapPos(0.0f);
    if (!b.any()) {
        ui::SecondaryText("Nothing selected: Submit only updates the quantities.");
    } else {
        std::snprintf(summary, sizeof(summary), "%d field%s will be set on %zu scanned item%s when you press Submit.", b.count(),
                      b.count() == 1 ? "" : "s", s.entries.size(), s.entries.size() == 1 ? "" : "s");
        ImGui::TextColored(P.accent, "%s", summary);
    }
    ImGui::PopTextWrapPos();
    if (b.any()) {
        ui::VerticalSpace(4 * dpi);
        if (ui::SubtleButton("Reset all to No change")) b.reset();
    }

    ImGui::EndChild();
}

} // namespace

namespace pages {

float stockTakeWidth(const App& app) {
    const float dpi = app.dpiScale;
    return kPanelWidth * dpi + (app.stockTakeBulkEdit ? (kBulkPanelWidth + kPanelGap) * dpi : 0.0f);
}

void stockTake(App& app) {
    StockTakeState& s = S();
    const Palette& P = Theme::get().palette();
    const Fonts& F = Theme::get().fonts();
    const float dpi = app.dpiScale;

    if (app.stockTakeBulkEdit) {
        drawBulkPanel(app);
        ImGui::SameLine(0, kPanelGap * dpi);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, P.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::BeginChild("##stockTake", ImVec2(kPanelWidth * dpi, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // Title row ------------------------------------------------------------
    ui::Subtitle("Stock take");
    ImGui::SameLine();
    ui::AlignRight(ImGui::GetFrameHeight());
    if (ui::IconButton("closeStock", icons::Close, "Close stock take")) app.stockTakeOpen = false;
    ui::SecondaryText("Scan a barcode or type a SKU, UPC or name, then press Enter.");
    ui::SecondaryText("Pause while typing to pick a product from a list.");
    ui::SecondaryText("Nothing is changed in RepairDesk until you press Submit.");
    ui::VerticalSpace(4 * dpi);

    // Scanner input --------------------------------------------------------
    if (s.focusInput) {
        ImGui::SetKeyboardFocusHere();
        s.focusInput = false;
    }
    ImGui::PushFont(F.bodyBold);
    ImGui::SetNextItemWidth(-1);
    bool entered = ImGui::InputTextWithHint("##scan", "Scan here...", &s.input,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopFont();
    const ImVec2 scanMin = ImGui::GetItemRectMin(), scanMax = ImGui::GetItemRectMax();
    if (ImGui::IsItemActive()) {
        ImGui::GetWindowDrawList()->AddLine(ImVec2(scanMin.x + 4, scanMax.y - 1), ImVec2(scanMax.x - 4, scanMax.y - 1), Theme::u32(P.accent), 2.0f);
    }
    if (entered) {
        handleScan(app);
    } else {
        updateSuggestions(app);
    }
    // Deferred: the list is a separate window, drawn after the panel so it overlaps the entries.
    int pickedSuggestion = -1;
    const bool showSuggestions = !entered && s.suggestReady;

    // "No item found" line (small red text)
    if (ImGui::GetTime() < s.errorUntil) {
        ImGui::PushStyleColor(ImGuiCol_Text, P.danger);
        ImGui::SetWindowFontScale(0.9f);
        ImGui::Text("%s  %s", icons::Error, s.errorText.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    } else {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    }

    if (app.inventory.empty()) {
        ui::InfoBar(ui::InfoKind::Warning, "Inventory is not loaded yet. Scans are matched against the loaded inventory list.");
    }

    // Summary --------------------------------------------------------------
    int totalChange = 0;
    for (const ScanEntry& e : s.entries) totalChange += e.scanned - e.dbQty;
    char summary[160];
    std::snprintf(summary, sizeof(summary), "%zu item(s) scanned", s.entries.size());
    ui::SecondaryText(summary);
    ImGui::SameLine();
    char changeText[48];
    std::snprintf(changeText, sizeof(changeText), "Net change %+d", totalChange);
    ui::AlignRight(ImGui::CalcTextSize(changeText).x);
    ImVec4 changeColor = totalChange > 0 ? P.success : (totalChange < 0 ? P.danger : P.textSecondary);
    ImGui::TextColored(changeColor, "%s", changeText);
    ui::Divider();

    // Scanned items --------------------------------------------------------
    float footerH = ImGui::GetFrameHeight() + 16 * dpi;
    ImGui::BeginChild("##scanList", ImVec2(0, ImGui::GetContentRegionAvail().y - footerH), ImGuiChildFlags_None);
    if (s.entries.empty()) {
        ui::VerticalSpace(24 * dpi);
        ui::EmptyState(icons::Scan, "Ready to scan", "Scanned items will appear here.");
    }
    int removeIndex = -1;
    for (size_t i = 0; i < s.entries.size(); ++i) {
        ScanEntry& e = s.entries[i];
        ImGui::PushID((int)i);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::withAlpha(P.text, Theme::get().isDark() ? 0.04f : 0.03f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * dpi, 10 * dpi));
        ImGui::BeginChild("##entry", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Name + remove
        ImGui::PushFont(F.bodyBold);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 8 * dpi);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SameLine();
        ui::AlignRight(ImGui::GetFrameHeight());
        if (ui::IconButton("rm", icons::Close, "Remove from stock take")) removeIndex = (int)i;

        std::string codes = e.sku;
        if (!e.upc.empty()) codes += (codes.empty() ? "" : "  |  ") + std::string("UPC ") + e.upc;
        if (!codes.empty()) ui::SecondaryText(codes.c_str());
        ui::VerticalSpace(2 * dpi);

        // Quantities row
        int change = e.scanned - e.dbQty;
        ImVec4 changeCol = change > 0 ? P.success : (change < 0 ? P.danger : P.textSecondary);

        ImGui::BeginGroup();
        ui::SecondaryText("In RepairDesk");
        ImGui::Text("%d", e.dbQty);
        ImGui::EndGroup();
        ImGui::SameLine(0, 24 * dpi);
        ImGui::BeginGroup();
        ui::SecondaryText("Change");
        ImGui::TextColored(changeCol, "%+d", change);
        ImGui::EndGroup();
        ImGui::SameLine(0, 24 * dpi);
        ImGui::BeginGroup();
        ui::SecondaryText("Scanned");
        ImGui::PushFont(F.subtitle);
        if (s.editingId == e.id) {
            // Typing a new count: Enter or clicking away commits, Esc cancels.
            if (s.focusEdit) { ImGui::SetKeyboardFocusHere(); s.focusEdit = false; }
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2);
            ImGui::PushStyleColor(ImGuiCol_Text, P.accent);
            bool done = ImGui::InputText("##count", &s.editText,
                                         ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                             ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopStyleColor();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                s.editingId = 0;
                s.editText.clear();
                s.focusInput = true;
            } else if (done || ImGui::IsItemDeactivated()) {
                commitCountEdit(s);
                s.focusInput = true;
            }
        } else {
            ImGui::TextColored(P.accent, "%d", e.scanned);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y), Theme::u32(P.accent), 1.0f);
                ui::Tooltip("Click to type the counted quantity");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    commitCountEdit(s);
                    s.editingId = e.id;
                    s.editText = std::to_string(e.scanned);
                    s.focusEdit = true;
                }
            }
        }
        ImGui::PopFont();
        ImGui::EndGroup();

        // Manual adjust buttons on the right
        ImGui::SameLine();
        float btn = ImGui::GetFrameHeight();
        ui::AlignRight(btn * 2 + ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * dpi);
        if (ui::IconButton("minus", icons::Remove, "Decrease scanned quantity")) e.scanned = e.scanned > 0 ? e.scanned - 1 : 0;
        ImGui::SameLine();
        if (ui::IconButton("plus", icons::Add, "Increase scanned quantity", true)) e.scanned++;

        ImGui::EndChild();
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        if (s.editingId == s.entries[removeIndex].id) { s.editingId = 0; s.editText.clear(); }
        s.entries.erase(s.entries.begin() + removeIndex);
    }
    ImGui::EndChild();

    // Footer ---------------------------------------------------------------
    ui::VerticalSpace(4 * dpi);
    ImGui::BeginDisabled(s.entries.empty() || s.applying);
    if (ui::Button("Clear")) s.entries.clear();
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::string applyLabel = std::string(icons::Save) + "  Submit";
    const float applyW = ImGui::CalcTextSize(applyLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
    const char* toggleLabel = "Bulk edit";
    const float toggleW = ImGui::GetFrameHeight() * 0.8f * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(toggleLabel).x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ui::AlignRight(toggleW + spacing * 2 + applyW + (s.applying ? 28 * dpi : 0));
    // Toggle just left of Submit: opens the bulk-edit panel for the scanned items.
    ImGui::BeginDisabled(s.applying);
    ui::ToggleSwitch(toggleLabel, &app.stockTakeBulkEdit);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ui::Tooltip("Also set the category, location and/or supplier of every scanned item when submitting");
    }
    ImGui::SameLine(0, spacing * 2);
    ImGui::BeginDisabled(s.entries.empty() || s.applying);
    if (s.applying) {
        ui::Spinner("##applySpin");
        ImGui::SameLine();
    }
    if (ui::AccentButton(applyLabel.c_str())) applyToRepairDesk(app);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !s.entries.empty()) {
        const bool bulk = app.stockTakeBulkEdit && s.bulk.any();
        ui::Tooltip(bulk ? "Submits the stock take: adjusts each item's in-stock quantity to the scanned quantity and sets the bulk-edit fields on every scanned item"
                         : "Submits the stock take: adjusts each item's in-stock quantity in RepairDesk by the change shown, so it ends up at the scanned quantity");
    }

    ImGui::EndChild();

    if (showSuggestions) pickedSuggestion = drawSuggestions(app, scanMin, scanMax);
    if (pickedSuggestion >= 0) pickSuggestion(app, pickedSuggestion);
}

} // namespace pages
