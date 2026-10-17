#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "RepairDeskApi.h"
#include "Settings.h"

struct ColumnSpec {
    std::string key;            // key in the FlatRecord
    std::string label;          // header text (prettified from key when empty)
    float width = 140.0f;       // initial width in pixels
    bool defaultVisible = true;
};

// Generic table over flattened JSON records.
//
// * Row divider lines only (no vertical borders, no striping).
// * Columns can be dragged to reorder and resized by dragging the right edge.
// * A gear button at the far right of the header opens a checkbox list that
//   toggles each column on or off.
// * Column order, widths and visibility are restored from / persisted to the
//   bound `Settings` (by field key, see `bindSettings`).
// * Optional selection checkbox column with a header "select all" checkbox.
// * Optional inline editing: a single click on an editable cell turns it into a
//   text field. Tab / Shift+Tab move to the next / previous editable column of
//   the same row (the edited values are staged), Enter or clicking elsewhere
//   commits every staged change of the row at once through `onRowEdited`,
//   Escape discards them. A double click on a row fires `onRowDoubleClicked`,
//   a single click on a non-editable cell fires `onRowClicked`.
// * Optional per-column value filters: a funnel icon at the right edge of every
//   header opens a checkbox list of the column's values (`filterValues` supplies
//   them); the owner applies `passesFilters` when it builds its row list.
class DataTable {
public:
    explicit DataTable(std::string id);

    // Tables read their saved layout from and write changes into these settings.
    static void bindSettings(Settings* settings);

    // Declares the preferred columns (shown first, in this order). Columns for
    // any additional keys found in `records` are appended (hidden by default)
    // so that every piece of information the API returns can be toggled on.
    void setColumns(const std::vector<ColumnSpec>& preferred);
    void discoverColumns(const std::vector<FlatRecord>& records);
    const std::vector<ColumnSpec>& columns() const { return columns_; }

    // Returns true when the "cell" was handled by the callback; otherwise the
    // table renders the raw text value.
    using CellRenderer = std::function<bool(int row, const std::string& key, const std::string& value)>;
    // Staged edits of one row: (key, new value) in the order they were made.
    using RowEdits = std::vector<std::pair<std::string, std::string>>;
    // Appends every value of column `key` across the *unfiltered* data (duplicates allowed).
    using FilterValues = std::function<void(const std::string& key, std::vector<std::string>& values)>;

    struct DrawOptions {
        bool selectable = false;                 // show checkbox column
        std::vector<bool>* selection = nullptr;  // sized to row count when selectable
        CellRenderer cellRenderer;
        std::function<void(int row)> onRowClicked;        // single click (not on an editable cell)
        std::function<void(int row)> onRowDoubleClicked;  // double click anywhere on the row
        // Inline editing. When `isEditable` returns true for a cell, a single click on it
        // starts editing; `onRowEdited` receives all changed cells of the row on commit.
        std::function<bool(int row, const std::string& key)> isEditable;
        std::function<void(int row, const RowEdits& edits)> onRowEdited;
        FilterValues filterValues;               // enables the header filter icons
        float rowHeight = 38.0f;
        ImVec2 size = ImVec2(0, 0);              // 0 = fill remaining
    };

    // Draws the table. `rowAt` must return the record for a row index.
    void draw(int rowCount, const std::function<const FlatRecord&(int)>& rowAt, const DrawOptions& options);

    // Sorting (the owner applies the sort to its own data when `sortDirty` is set).
    bool sortDirty = false;
    std::string sortKey;
    bool sortAscending = true;

    static std::string prettifyKey(const std::string& key);

    bool isEditing() const { return editRow_ >= 0; }
    // Drops the current cell edit and every staged change of the row.
    void cancelEdit();

    // Column value filters (the owner re-filters its rows when `filterDirty` is set).
    bool filterDirty = false;
    bool hasFilters() const { return !filters_.empty(); }
    int filterCount() const { return (int)filters_.size(); }
    bool passesFilters(const FlatRecord& rec) const;
    void clearFilters();

private:
    // Editing state --------------------------------------------------------
    void stageCurrentCell(const std::string& originalValue);
    void beginEdit(int row, const std::string& key, const std::string& value, const std::string& rowId);
    void commitEdits(int rowCount, const std::function<const FlatRecord&(int)>& rowAt, const DrawOptions& options);
    const std::string* pendingValue(const std::string& key) const;

    int editRow_ = -1;
    std::string editKey_;
    std::string editBuffer_;
    std::string editOriginal_;           // value of the cell when editing started
    std::string editRowId_;              // "id" field of the edited record (to survive re-sorts)
    bool editFocus_ = false;
    RowEdits pending_;                   // staged (tabbed-through) edits of the edited row

    // Layout persistence ---------------------------------------------------
    static Settings* settings_;
    void applySavedLayout(int firstDataCol, int totalCols, bool selectable);
    void captureLayout(int firstDataCol);
    bool layoutPending_ = true;          // saved layout must be (re)applied before the next BeginTable
    std::vector<std::string> appliedKeys_;   // column keys the layout was last applied for
    TableLayout lastLayout_;             // last layout captured from the live table

    // Column filters -------------------------------------------------------
    void drawFilterPopup();
    void setFilterValue(const std::string& key, const std::string& value, bool on);
    std::map<std::string, std::set<std::string>> filters_;   // column key -> values kept (absent = no filter)
    std::string filterKey_;              // column whose filter popup is open
    std::string filterLabel_;
    std::vector<std::string> filterOptions_;   // distinct values offered in the popup
    std::string filterSearch_;
    ImVec2 filterPopupPos_;

    std::string id_;
    std::vector<ColumnSpec> columns_;
    std::vector<ColumnSpec> preferred_;
    std::vector<char> pendingToggles_;   // per column: 0 none, 1 enable, 2 disable
};
