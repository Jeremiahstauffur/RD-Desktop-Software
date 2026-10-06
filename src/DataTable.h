#pragma once

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "RepairDeskApi.h"

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
// * Optional selection checkbox column with a header "select all" checkbox.
// * Optional click-to-edit cells: clicking a cell's text turns it into a text
//   field (Enter commits, Escape cancels); clicking the row elsewhere fires
//   `onRowClicked`.
class DataTable {
public:
    explicit DataTable(std::string id);

    // Declares the preferred columns (shown first, in this order). Columns for
    // any additional keys found in `records` are appended (hidden by default)
    // so that every piece of information the API returns can be toggled on.
    void setColumns(const std::vector<ColumnSpec>& preferred);
    void discoverColumns(const std::vector<FlatRecord>& records);
    const std::vector<ColumnSpec>& columns() const { return columns_; }

    // Returns true when the "cell" was handled by the callback; otherwise the
    // table renders the raw text value.
    using CellRenderer = std::function<bool(int row, const std::string& key, const std::string& value)>;

    struct DrawOptions {
        bool selectable = false;                 // show checkbox column
        std::vector<bool>* selection = nullptr;  // sized to row count when selectable
        CellRenderer cellRenderer;
        std::function<void(int row)> onRowClicked;
        // Inline editing. When `isEditable` returns true for a cell, clicking its
        // text starts editing and `onCellEdited` receives the committed value.
        std::function<bool(int row, const std::string& key)> isEditable;
        std::function<void(int row, const std::string& key, const std::string& newValue)> onCellEdited;
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
    void cancelEdit() { editRow_ = -1; editKey_.clear(); }

private:
    int editRow_ = -1;
    std::string editKey_;
    std::string editBuffer_;
    bool editFocus_ = false;
    std::string id_;
    std::vector<ColumnSpec> columns_;
    std::vector<ColumnSpec> preferred_;
    std::vector<char> pendingToggles_;   // per column: 0 none, 1 enable, 2 disable
};
