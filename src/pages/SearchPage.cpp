#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "App.h"
#include "Async.h"
#include "DataTable.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

// Global search page. Matches the query against *every* field (label, value or
// nested detail) of products, invoices, tickets and customers.
//
// Products are matched against the already-loaded inventory. Invoices,
// tickets and customers are fetched from RepairDesk with the keyword and then
// filtered again client-side so that any field - not just the ones the server
// indexes - can produce a hit.

namespace {

struct RemoteSection {
    const char* title;
    const char* icon;
    DataTable table;
    std::vector<json> rows;          // raw records from the server
    std::vector<FlatRecord> records; // flattened
    std::vector<int> hits;           // indices into records that match
    bool loading = false;
    std::string error;
    std::string loadedFor;           // the query these rows were fetched for
    std::vector<ColumnSpec> columns;
    int generation = 0;

    RemoteSection(const char* t, const char* i, const char* tableId) : title(t), icon(i), table(tableId) {}
};

struct SearchState {
    std::string lastQuery;
    double lastTypedAt = 0.0;
    bool pendingFetch = false;

    // products
    DataTable productTable{"searchProducts"};
    std::vector<int> productHits;
    size_t productInventorySize = (size_t)-1;

    RemoteSection invoices{"Invoices", icons::Document, "searchInvoices"};
    RemoteSection tickets{"Tickets", icons::Repair, "searchTickets"};
    RemoteSection customers{"Customers", icons::People, "searchCustomers"};

    int activeTab = 0; // 0 all, 1 products, 2 invoices, 3 tickets, 4 customers
    bool init = false;
};

SearchState& S() {
    static SearchState s;
    return s;
}

void filterSection(RemoteSection& sec, const std::string& needle) {
    sec.hits.clear();
    for (size_t i = 0; i < sec.records.size(); ++i) {
        if (recordMatches(sec.records[i], needle)) sec.hits.push_back((int)i);
    }
}

template <typename Fetch>
void fetchSection(App& app, RemoteSection& sec, const std::string& query, Fetch fetch) {
    if (!app.connected()) return;
    sec.loading = true;
    sec.error.clear();
    int gen = ++sec.generation;
    RepairDeskApi client = app.api();
    Async::run([client, query, fetch] { return fetch(client, query); },
               [&sec, gen, query](ApiResult<std::vector<json>> r) {
                   if (gen != sec.generation) return; // stale
                   sec.loading = false;
                   sec.loadedFor = query;
                   if (r.ok) {
                       sec.rows = std::move(r.value);
                       sec.records.clear();
                       for (const json& j : sec.rows) sec.records.push_back(flattenJson(j, 3));
                       sec.table.setColumns(sec.columns);
                       sec.table.discoverColumns(sec.records);
                       filterSection(sec, toLower(query));
                   } else {
                       sec.error = r.message;
                   }
               });
}

void startFetches(App& app, const std::string& query) {
    SearchState& s = S();
    fetchSection(app, s.invoices, query, [](const RepairDeskApi& api, const std::string& q) { return api.listInvoices(q, 1, 200); });
    fetchSection(app, s.tickets, query, [](const RepairDeskApi& api, const std::string& q) { return api.listTickets(q, 1, 200); });
    fetchSection(app, s.customers, query, [](const RepairDeskApi& api, const std::string& q) { return api.listCustomers(q, 1, 200); });
}

void filterProducts(App& app, const std::string& needle) {
    SearchState& s = S();
    s.productHits.clear();
    for (size_t i = 0; i < app.inventory.size(); ++i) {
        if (recordMatches(app.inventory[i].fields, needle)) s.productHits.push_back((int)i);
    }
    s.productInventorySize = app.inventory.size();
}

void sectionHeader(const char* icon, const char* title, size_t count, bool loading) {
    const Palette& P = Theme::get().palette();
    ImGui::PushFont(Theme::get().fonts().subtitle);
    ImGui::TextColored(P.accent, "%s  %s", icon, title);
    ImGui::PopFont();
    ImGui::SameLine();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu", count);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
    ui::Badge(buf, Theme::withAlpha(P.accent, 0.2f), P.accent);
    if (loading) {
        ImGui::SameLine();
        ui::Spinner("##secspin", 7.0f);
    }
}

void drawRemoteSection(App& app, RemoteSection& sec, float height) {
    sectionHeader(sec.icon, sec.title, sec.hits.size(), sec.loading);
    if (!sec.error.empty()) {
        ui::InfoBar(ui::InfoKind::Error, sec.error);
        return;
    }
    if (sec.hits.empty()) {
        ui::SecondaryText(sec.loading ? "Searching..." : "No matches.");
        return;
    }
    DataTable::DrawOptions opts;
    opts.size = ImVec2(0, height);
    opts.rowHeight = 36.0f * app.dpiScale;
    opts.onRowClicked = [&](int row) { pages::openRecordViewer(sec.title, sec.rows[sec.hits[row]]); };
    sec.table.draw((int)sec.hits.size(), [&](int row) -> const FlatRecord& { return sec.records[sec.hits[row]]; }, opts);
}

} // namespace

namespace pages {

void search(App& app) {
    SearchState& s = S();
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;

    if (!s.init) {
        s.init = true;
        s.productTable.setColumns({
            {"name", "Name", 260.0f, true}, {"sku", "SKU", 140.0f, true}, {"upc_code", "UPC", 140.0f, true},
            {"item_type", "Category", 140.0f, true}, {"in_stock", "In stock", 90.0f, true}, {"price", "Price", 100.0f, true},
        });
        s.invoices.columns = {
            {"summary.order_id", "Invoice #", 110.0f, true}, {"summary.customer.fullName", "Customer", 200.0f, true},
            {"summary.total", "Total", 100.0f, true}, {"summary.status", "Status", 120.0f, true},
            {"summary.created_date", "Created", 160.0f, true},
        };
        s.tickets.columns = {
            {"summary.order_id", "Ticket #", 110.0f, true}, {"summary.customer.fullName", "Customer", 200.0f, true},
            {"summary.customer.mobile", "Mobile", 140.0f, true}, {"summary.total", "Total", 100.0f, true},
            {"summary.created_date", "Created", 160.0f, true},
        };
        s.customers.columns = {
            {"fullName", "Name", 220.0f, true}, {"mobile", "Mobile", 150.0f, true}, {"email", "Email", 220.0f, true},
            {"organization", "Organization", 160.0f, true},
        };
        s.invoices.table.setColumns(s.invoices.columns);
        s.tickets.table.setColumns(s.tickets.columns);
        s.customers.table.setColumns(s.customers.columns);
    }

    const std::string& query = app.globalSearch;
    std::string needle = toLower(query);

    // Local filtering is instant; remote fetches are debounced while typing.
    if (query != s.lastQuery || s.productInventorySize != app.inventory.size()) {
        if (query != s.lastQuery) {
            s.lastQuery = query;
            s.lastTypedAt = ImGui::GetTime();
            s.pendingFetch = !query.empty();
        }
        filterProducts(app, needle);
        filterSection(s.invoices, needle);
        filterSection(s.tickets, needle);
        filterSection(s.customers, needle);
    }
    if (s.pendingFetch && (app.globalSearchSubmitted || ImGui::GetTime() - s.lastTypedAt > 0.45)) {
        s.pendingFetch = false;
        startFetches(app, query);
    }

    // Header -----------------------------------------------------------------
    if (ui::IconButton("backFromSearch", icons::Back, "Back")) {
        app.page = app.pageBeforeSearch == Page::Search ? Page::Inventory : app.pageBeforeSearch;
        return;
    }
    ImGui::SameLine();
    ui::PageTitle("Search");
    if (query.empty()) {
        ui::EmptyState(icons::Search, "Search everything",
                       "Type in the search bar at the bottom to find products, invoices, tickets and customers.");
        return;
    }
    char sub[256];
    size_t total = s.productHits.size() + s.invoices.hits.size() + s.tickets.hits.size() + s.customers.hits.size();
    std::snprintf(sub, sizeof(sub), "%zu result(s) for \"%s\"", total, query.c_str());
    ui::SecondaryText(sub);

    // Tabs -------------------------------------------------------------------
    struct Tab { const char* label; size_t count; };
    Tab tabs[] = {{"All", total}, {"Products", s.productHits.size()}, {"Invoices", s.invoices.hits.size()},
                  {"Tickets", s.tickets.hits.size()}, {"Customers", s.customers.hits.size()}};
    for (int i = 0; i < 5; ++i) {
        if (i > 0) ImGui::SameLine();
        char label[64];
        std::snprintf(label, sizeof(label), "%s (%zu)##tab%d", tabs[i].label, tabs[i].count, i);
        bool active = s.activeTab == i;
        if (active ? ui::AccentButton(label) : ui::SubtleButton(label)) s.activeTab = i;
    }
    ui::Divider();

    // Results ----------------------------------------------------------------
    ImGui::BeginChild("##searchResults", ImVec2(0, 0), ImGuiChildFlags_None);
    const bool all = s.activeTab == 0;
    float sectionH = all ? 260.0f * dpi : ImGui::GetContentRegionAvail().y - 50.0f * dpi;

    if (all || s.activeTab == 1) {
        sectionHeader(icons::Package, "Products", s.productHits.size(), app.inventoryLoading);
        if (s.productHits.empty()) {
            ui::SecondaryText(app.inventory.empty() ? "Inventory not loaded." : "No matches.");
        } else {
            DataTable::DrawOptions opts;
            opts.size = ImVec2(0, sectionH);
            opts.rowHeight = 36.0f * dpi;
            opts.onRowClicked = [&](int row) { app.openProduct(app.inventory[s.productHits[row]].id); };
            opts.cellRenderer = [&](int, const std::string& key, const std::string& value) {
                if (key == "price") {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "$%.2f", std::atof(value.c_str()));
                    ImGui::TextUnformatted(buf);
                    return true;
                }
                return false;
            };
            s.productTable.draw((int)s.productHits.size(),
                                [&](int row) -> const FlatRecord& { return app.inventory[s.productHits[row]].fields; }, opts);
        }
        ui::VerticalSpace(12 * dpi);
    }
    if (all || s.activeTab == 2) { drawRemoteSection(app, s.invoices, sectionH); ui::VerticalSpace(12 * dpi); }
    if (all || s.activeTab == 3) { drawRemoteSection(app, s.tickets, sectionH); ui::VerticalSpace(12 * dpi); }
    if (all || s.activeTab == 4) { drawRemoteSection(app, s.customers, sectionH); }
    ImGui::EndChild();

    pages::drawRecordViewer(app);
}

} // namespace pages
