#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "App.h"
#include "Async.h"
#include "DataTable.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

// Shared state + rendering for the read-only list pages (tickets, invoices,
// SMS inbox). Each page owns one instance and supplies a fetch function.
struct ListPageState {
    explicit ListPageState(const char* tableId) : table(tableId) {}

    DataTable table;
    std::vector<json> rows;
    std::vector<FlatRecord> records;
    std::vector<int> view;
    std::vector<ColumnSpec> preferredColumns;
    std::string search;
    std::string lastSearch;
    bool loading = false;
    bool loadedOnce = false;
    bool viewDirty = true;
    std::string error;
    std::string status;
    int page = 1;
    int pageSize = 100;

    using Fetch = std::function<ApiResult<std::vector<json>>(const RepairDeskApi&, const std::string& keyword, int page, int pageSize)>;

    void load(App& app, const Fetch& fetch) {
        if (loading) return;
        if (!app.connected()) {
            error = "Not connected. Open Settings and enter an API key or connect with OAuth2.";
            return;
        }
        loading = true;
        error.clear();
        RepairDeskApi client = app.api();
        std::string keyword = search;
        int p = page, ps = pageSize;
        Async::run(
            [client, fetch, keyword, p, ps] { return fetch(client, keyword, p, ps); },
            [this, &app](ApiResult<std::vector<json>> result) {
                loading = false;
                loadedOnce = true;
                if (result.ok) {
                    rows = std::move(result.value);
                    records.clear();
                    records.reserve(rows.size());
                    for (const json& j : rows) records.push_back(flattenJson(j, 3));
                    table.setColumns(preferredColumns);
                    table.discoverColumns(records);
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "%zu record(s) on page %d", rows.size(), page);
                    status = buf;
                    viewDirty = true;
                } else {
                    error = result.message;
                    app.notify(result.message, ui::InfoKind::Error);
                }
            });
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    void rebuildView() {
        view.clear();
        for (size_t i = 0; i < records.size(); ++i) view.push_back((int)i);
        if (!table.sortKey.empty()) {
            const std::string key = table.sortKey;
            const bool asc = table.sortAscending;
            auto valueOf = [&](int idx) -> const std::string& {
                static const std::string empty;
                auto it = records[idx].find(key);
                return it == records[idx].end() ? empty : it->second;
            };
            bool numeric = !view.empty();
            for (int idx : view) {
                const std::string& v = valueOf(idx);
                if (v.empty()) continue;
                char* end = nullptr;
                std::strtod(v.c_str(), &end);
                if (end == v.c_str() || *end != '\0') { numeric = false; break; }
            }
            std::stable_sort(view.begin(), view.end(), [&](int a, int b) {
                const std::string& va = valueOf(a);
                const std::string& vb = valueOf(b);
                if (numeric) {
                    double da = va.empty() ? 0 : std::atof(va.c_str());
                    double db = vb.empty() ? 0 : std::atof(vb.c_str());
                    return asc ? da < db : db < da;
                }
                return asc ? lower(va) < lower(vb) : lower(vb) < lower(va);
            });
        }
        viewDirty = false;
    }

    // Draws title, toolbar (search + refresh + paging) and the table.
    void draw(App& app, const char* title, const char* searchHint, const Fetch& fetch, const char* emptyIcon,
              const DataTable::CellRenderer& cellRenderer = {}) {
        const float dpi = app.dpiScale;
        if (!loadedOnce && !loading && app.connected()) load(app, fetch);
        if (table.sortDirty) { table.sortDirty = false; viewDirty = true; }
        if (viewDirty) rebuildView();

        ui::PageTitle(title);
        ImGui::SameLine();
        if (loading) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8 * dpi);
            ui::Spinner("##spin");
        }

        if (ui::SearchField("search", search, searchHint, 320 * dpi)) {
            page = 1;
            load(app, fetch);
        }
        ImGui::SameLine();
        if (ui::Button((std::string(icons::Refresh) + "  Refresh").c_str())) load(app, fetch);

        // Paging on the right
        ImGui::SameLine();
        char pageText[32];
        std::snprintf(pageText, sizeof(pageText), "Page %d", page);
        float pagerW = ImGui::GetFrameHeight() * 2 + ImGui::CalcTextSize(pageText).x + ImGui::GetStyle().ItemSpacing.x * 2 + 8;
        ui::AlignRight(pagerW);
        ImGui::BeginDisabled(page <= 1 || loading);
        if (ui::IconButton("prev", icons::ChevronLeft, "Previous page")) { page--; load(app, fetch); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ui::SecondaryText(pageText);
        ImGui::SameLine();
        ImGui::BeginDisabled(loading || (int)rows.size() < pageSize);
        if (ui::IconButton("next", icons::ChevronRight, "Next page")) { page++; load(app, fetch); }
        ImGui::EndDisabled();

        ui::VerticalSpace(4 * dpi);
        if (!error.empty()) {
            ui::InfoBar(ui::InfoKind::Error, error);
            ui::VerticalSpace(4 * dpi);
        }

        float statusH = ImGui::GetTextLineHeightWithSpacing() + 8 * dpi;
        ImVec2 tableSize(0, ImGui::GetContentRegionAvail().y - statusH);
        if (records.empty()) {
            ImGui::BeginChild("##empty", tableSize);
            if (!app.connected()) {
                ui::EmptyState(icons::Key, "Connect to RepairDesk", "Open Settings to enter an API key or sign in with OAuth2.");
            } else if (loadedOnce && !loading) {
                ui::EmptyState(emptyIcon, "Nothing to show", "No records were returned for this page.");
            }
            ImGui::EndChild();
        } else {
            DataTable::DrawOptions opts;
            opts.size = tableSize;
            opts.rowHeight = 38.0f * dpi;
            opts.cellRenderer = cellRenderer;
            opts.onRowClicked = [&](int row) { pages::openRecordViewer(title, rows[view[row]]); };
            table.draw((int)view.size(), [&](int row) -> const FlatRecord& { return records[view[row]]; }, opts);
        }
        ui::VerticalSpace(2 * dpi);
        ui::SecondaryText(status.c_str());
        pages::drawRecordViewer(app);
    }
};
