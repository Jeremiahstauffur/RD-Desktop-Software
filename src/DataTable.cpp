#include "DataTable.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <numeric>
#include <unordered_set>

#include <cfloat>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "Theme.h"
#include "Widgets.h"

namespace {

constexpr float kSelectionColWidth = 44.0f;
constexpr float kGearColWidth = 40.0f;

bool sameLayout(const TableLayout& a, const TableLayout& b) {
    if (a.columns.size() != b.columns.size() || a.sortKey != b.sortKey || a.sortAscending != b.sortAscending) return false;
    if (std::abs(a.refScale - b.refScale) > 0.01f) return false;
    for (size_t i = 0; i < a.columns.size(); ++i) {
        const TableLayout::Column& x = a.columns[i];
        const TableLayout::Column& y = b.columns[i];
        if (x.key != y.key || x.visible != y.visible || std::abs(x.width - y.width) > 0.5f) return false;
    }
    return true;
}

const std::string& recordId(const FlatRecord& rec) {
    static const std::string empty;
    auto it = rec.find("id");
    return it == rec.end() ? empty : it->second;
}

} // namespace

Settings* DataTable::settings_ = nullptr;

DataTable::DataTable(std::string id) : id_(std::move(id)) {}

void DataTable::bindSettings(Settings* settings) {
    settings_ = settings;
}

std::string DataTable::prettifyKey(const std::string& key) {
    std::string out;
    bool capitalizeNext = true;
    for (size_t i = 0; i < key.size(); ++i) {
        char c = key[i];
        if (c == '_' || c == '-') {
            out.push_back(' ');
            continue;
        }
        if (c == '.') {
            out += " > ";
            capitalizeNext = true;
            continue;
        }
        if (std::isupper((unsigned char)c) && i > 0 && std::islower((unsigned char)key[i - 1])) {
            out.push_back(' ');
            out.push_back((char)std::tolower((unsigned char)c));
            continue;
        }
        if (capitalizeNext) {
            out.push_back((char)std::toupper((unsigned char)c));
            capitalizeNext = false;
        } else {
            out.push_back(c);
        }
    }
    // Common abbreviations
    auto replaceWord = [&](const char* from, const char* to) {
        size_t pos = 0;
        std::string f(from);
        while ((pos = out.find(f, pos)) != std::string::npos) {
            bool startOk = pos == 0 || out[pos - 1] == ' ';
            bool endOk = pos + f.size() == out.size() || out[pos + f.size()] == ' ';
            if (startOk && endOk) {
                out.replace(pos, f.size(), to);
                pos += std::string(to).size();
            } else {
                pos += f.size();
            }
        }
    };
    replaceWord("Sku", "SKU");
    replaceWord("Upc", "UPC");
    replaceWord("Id", "ID");
    replaceWord("id", "ID");
    replaceWord("Url", "URL");
    replaceWord("Gst", "GST");
    replaceWord("Sms", "SMS");
    return out;
}

void DataTable::setColumns(const std::vector<ColumnSpec>& preferred) {
    preferred_ = preferred;
    columns_ = preferred;
    for (auto& c : columns_) {
        if (c.label.empty()) c.label = prettifyKey(c.key);
    }
}

void DataTable::discoverColumns(const std::vector<FlatRecord>& records) {
    std::unordered_set<std::string> known;
    for (const auto& c : columns_) known.insert(c.key);

    // Keep insertion order stable across refreshes: append new keys sorted.
    std::vector<std::string> extra;
    for (const auto& rec : records) {
        for (const auto& [k, v] : rec) {
            if (known.insert(k).second) extra.push_back(k);
        }
    }
    std::sort(extra.begin(), extra.end());
    for (const auto& k : extra) {
        ColumnSpec c;
        c.key = k;
        c.label = prettifyKey(k);
        c.width = 140.0f;
        c.defaultVisible = false;
        columns_.push_back(c);
    }
}

// ---------------------------------------------------------------------------
// Layout persistence
// ---------------------------------------------------------------------------

// Writes the saved (key based) layout into ImGui's table settings storage for this
// table and asks the table to reload it. ImGui itself only stores columns by index,
// which breaks as soon as the API or a newer version adds or removes a column.
void DataTable::applySavedLayout(int firstDataCol, int totalCols, bool selectable) {
    const TableLayout* src = nullptr;
    if (settings_) {
        auto it = settings_->tableLayouts.find(id_);
        if (it != settings_->tableLayouts.end()) src = &it->second;
    } else if (!lastLayout_.columns.empty()) {
        src = &lastLayout_;
    }
    if (!src || src->columns.empty()) return;
    if (src != &lastLayout_) lastLayout_ = *src;   // applying the saved layout is not a change

    const int dataColumns = (int)columns_.size();
    const ImGuiID id = ImGui::GetID(id_.c_str());

    ImGuiTableSettings* settings = ImGui::TableSettingsFindByID(id);
    if (settings && settings->ColumnsCountMax < totalCols) {
        settings->ID = 0;   // too small for this column count: ditch it, like ImGui does
        settings = nullptr;
    }
    if (!settings) settings = ImGui::TableSettingsCreate(id, totalCols);
    settings->ColumnsCount = (ImGuiTableColumnIdx)totalCols;
    settings->SaveFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                          ImGuiTableFlags_Sortable;
    settings->RefScale = src->refScale;
    settings->WantApply = true;
    ImGuiTableColumnSettings* cs = settings->GetColumnSettings();
    for (int n = 0; n < settings->ColumnsCountMax; ++n) cs[n] = ImGuiTableColumnSettings();

    // Saved position of each column key (unknown keys keep their natural order at the end).
    std::vector<int> rank(dataColumns, INT_MAX);
    std::vector<const TableLayout::Column*> saved(dataColumns, nullptr);
    for (size_t pos = 0; pos < src->columns.size(); ++pos) {
        for (int i = 0; i < dataColumns; ++i) {
            if (columns_[i].key == src->columns[pos].key) {
                rank[i] = (int)pos;
                saved[i] = &src->columns[pos];
                break;
            }
        }
    }
    std::vector<int> order(dataColumns);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return rank[a] < rank[b]; });

    int display = firstDataCol;
    for (int i : order) {
        ImGuiTableColumnSettings& c = cs[firstDataCol + i];
        c.Index = (ImGuiTableColumnIdx)(firstDataCol + i);
        c.DisplayOrder = (ImGuiTableColumnIdx)(display++);
        c.WidthOrWeight = saved[i] && saved[i]->width > 0.0f ? saved[i]->width : columns_[i].width;
        c.IsEnabled = (saved[i] ? saved[i]->visible : columns_[i].defaultVisible) ? 1 : 0;
        c.IsStretch = 0;
        if (!src->sortKey.empty() && columns_[i].key == src->sortKey) {
            c.SortOrder = 0;
            c.SortDirection = src->sortAscending ? ImGuiSortDirection_Ascending : ImGuiSortDirection_Descending;
        }
    }
    if (selectable) {
        ImGuiTableColumnSettings& c = cs[0];
        c.Index = 0;
        c.DisplayOrder = 0;
        c.WidthOrWeight = kSelectionColWidth;
        c.IsEnabled = 1;
    }
    {
        ImGuiTableColumnSettings& c = cs[totalCols - 1];
        c.Index = (ImGuiTableColumnIdx)(totalCols - 1);
        c.DisplayOrder = (ImGuiTableColumnIdx)(totalCols - 1);
        c.WidthOrWeight = kGearColWidth;
        c.IsEnabled = 1;
    }

    // An already created table must re-read its settings; a new one loads them anyway.
    if (ImGuiTable* table = ImGui::TableFindByID(id)) {
        table->IsSettingsRequestLoad = true;
        table->SettingsOffset = -1;
    }
}

// Reads order/width/visibility/sort of the live table (must be called while the
// table is current) and stores it in the bound settings when something changed.
void DataTable::captureLayout(int firstDataCol) {
    ImGuiTable* table = ImGui::GetCurrentTable();
    if (!table) return;
    const int dataColumns = (int)columns_.size();

    TableLayout layout;
    layout.refScale = table->RefScale;
    std::vector<std::pair<int, int>> byOrder;   // (display order, data column index)
    byOrder.reserve(dataColumns);
    for (int i = 0; i < dataColumns; ++i) {
        const ImGuiTableColumn& c = table->Columns[firstDataCol + i];
        byOrder.emplace_back((int)c.DisplayOrder, i);
        if (c.SortOrder == 0) {
            layout.sortKey = columns_[i].key;
            layout.sortAscending = c.SortDirection != ImGuiSortDirection_Descending;
        }
    }
    std::sort(byOrder.begin(), byOrder.end());
    for (const auto& [order, i] : byOrder) {
        const ImGuiTableColumn& c = table->Columns[firstDataCol + i];
        TableLayout::Column col;
        col.key = columns_[i].key;
        col.width = c.WidthRequest > 0.0f ? c.WidthRequest : columns_[i].width;
        col.visible = c.IsUserEnabledNextFrame;
        layout.columns.push_back(std::move(col));
    }

    if (sameLayout(layout, lastLayout_)) return;
    lastLayout_ = layout;
    if (settings_) {
        settings_->tableLayouts[id_] = std::move(layout);
        settings_->layoutDirty = true;
    }
}

// ---------------------------------------------------------------------------
// Inline editing
// ---------------------------------------------------------------------------

void DataTable::cancelEdit() {
    editRow_ = -1;
    editKey_.clear();
    editBuffer_.clear();
    editOriginal_.clear();
    editRowId_.clear();
    editFocus_ = false;
    pending_.clear();
}

const std::string* DataTable::pendingValue(const std::string& key) const {
    for (const auto& [k, v] : pending_) {
        if (k == key) return &v;
    }
    return nullptr;
}

// Moves the text of the cell being edited into the staged changes of the row.
void DataTable::stageCurrentCell(const std::string& originalValue) {
    if (editKey_.empty()) return;
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->first == editKey_) {
            pending_.erase(it);
            break;
        }
    }
    if (editBuffer_ != originalValue) pending_.emplace_back(editKey_, editBuffer_);
    editKey_.clear();
}

void DataTable::beginEdit(int row, const std::string& key, const std::string& value, const std::string& rowId) {
    if (editRow_ != row) pending_.clear();
    editRow_ = row;
    editRowId_ = rowId;
    editKey_ = key;
    editBuffer_ = value;
    editOriginal_ = value;
    editFocus_ = true;
}

// Sends the staged changes of the edited row to the owner and ends editing. The row
// is looked up by its "id" field in case the data was re-sorted meanwhile.
void DataTable::commitEdits(int rowCount, const std::function<const FlatRecord&(int)>& rowAt, const DrawOptions& options) {
    int row = editRow_;
    RowEdits edits = std::move(pending_);
    const std::string rowId = editRowId_;
    cancelEdit();
    if (row < 0 || edits.empty() || !options.onRowEdited) return;

    if (!rowId.empty() && (row >= rowCount || recordId(rowAt(row)) != rowId)) {
        row = -1;
        for (int r = 0; r < rowCount; ++r) {
            if (recordId(rowAt(r)) == rowId) { row = r; break; }
        }
    }
    if (row < 0 || row >= rowCount) return;
    options.onRowEdited(row, edits);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void DataTable::draw(int rowCount, const std::function<const FlatRecord&(int)>& rowAt, const DrawOptions& options) {
    const Palette& P = Theme::get().palette();
    const Fonts& F = Theme::get().fonts();

    const bool selectable = options.selectable && options.selection;
    if (selectable && (int)options.selection->size() != rowCount) options.selection->assign(rowCount, false);

    const int dataColumns = (int)columns_.size();
    const int selectionCol = selectable ? 0 : -1;
    const int firstDataCol = selectable ? 1 : 0;
    const int gearCol = firstDataCol + dataColumns;
    const int totalCols = gearCol + 1;
    if (totalCols > IMGUI_TABLE_MAX_COLUMNS - 1) {
        ImGui::TextColored(P.danger, "Too many columns to display (%d).", totalCols);
        return;
    }

    const bool canEdit = options.isEditable && options.onRowEdited;
    if (editRow_ >= rowCount) cancelEdit();
    const bool wasEditing = editRow_ >= 0;

    // (Re)apply the persisted layout when the table is first drawn or its column set changed.
    {
        std::vector<std::string> keys;
        keys.reserve(columns_.size());
        for (const ColumnSpec& c : columns_) keys.push_back(c.key);
        if (layoutPending_ || keys != appliedKeys_) {
            applySavedLayout(firstDataCol, totalCols, selectable);
            appliedKeys_ = std::move(keys);
            layoutPending_ = false;
        }
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate |
                            ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_NoBordersInBodyUntilResize | ImGuiTableFlags_HighlightHoveredColumn;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(12.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::withAlpha(P.text, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::withAlpha(P.text, 0.08f));

    if (!ImGui::BeginTable(id_.c_str(), totalCols, flags, options.size)) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        return;
    }

    if (selectable) {
        ImGui::TableSetupColumn("##select", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoReorder |
                                                ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoResize |
                                                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHeaderLabel,
                                kSelectionColWidth);
    }
    for (int i = 0; i < dataColumns; ++i) {
        ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed;
        if (!columns_[i].defaultVisible) cf |= ImGuiTableColumnFlags_DefaultHide;
        ImGui::TableSetupColumn(columns_[i].label.c_str(), cf, columns_[i].width, (ImGuiID)(i + 1));
    }
    ImGui::TableSetupColumn("##gear", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoReorder |
                                          ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoResize |
                                          ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHeaderLabel,
                            kGearColWidth);
    ImGui::TableSetupScrollFreeze(selectable ? 1 : 0, 1);

    // Sorting -------------------------------------------------------------
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty) {
            if (specs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                int idx = (int)s.ColumnUserID - 1;
                if (idx >= 0 && idx < dataColumns) {
                    sortKey = columns_[idx].key;
                    sortAscending = s.SortDirection != ImGuiSortDirection_Descending;
                }
            } else {
                sortKey.clear();
            }
            sortDirty = true;
            specs->SpecsDirty = false;
        }
    }

    // Header row ----------------------------------------------------------
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, options.rowHeight);
    if (selectable) {
        ImGui::TableSetColumnIndex(selectionCol);
        int selectedCount = (int)std::count(options.selection->begin(), options.selection->end(), true);
        int state = selectedCount == 0 ? 0 : (selectedCount == rowCount ? 1 : 2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2.0f);
        if (ui::TriStateCheckbox("##selectAll", state)) {
            bool newValue = state != 1;
            std::fill(options.selection->begin(), options.selection->end(), newValue);
        }
    }
    ImGui::PushFont(F.bodyBold);
    ImGui::PushStyleColor(ImGuiCol_Text, P.accent);
    for (int i = 0; i < dataColumns; ++i) {
        if (!ImGui::TableSetColumnIndex(firstDataCol + i)) continue;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetTextLineHeight()) * 0.5f - 1.0f);
        ImGui::PushID(i);
        ImGui::TableHeader(columns_[i].label.c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();

    bool openColumnsPopup = false;
    if (ImGui::TableSetColumnIndex(gearCol)) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetFrameHeight()) * 0.5f);
        if (ui::IconButton("gear", icons::Settings, "Choose columns", true)) openColumnsPopup = true;
    }

    // Snapshot enabled state while the table is current (needed by the popup).
    std::vector<bool> enabled(dataColumns);
    for (int i = 0; i < dataColumns; ++i) {
        enabled[i] = (ImGui::TableGetColumnFlags(firstDataCol + i) & ImGuiTableColumnFlags_IsEnabled) != 0;
    }

    // Body ----------------------------------------------------------------
    ImGuiTable* table = ImGui::GetCurrentTable();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Clicks are detected geometrically (the row selectable, the cells and an active
    // text field all compete for the hover otherwise). Allowed while a cell editor is
    // active so that one click can leave one cell and enter another.
    const bool tableHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const int hoveredCol = tableHovered ? table->HoveredColumnBody : -1;
    const bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouseDoubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool shiftDown = ImGui::GetIO().KeyShift;

    bool editRowDrawn = false;
    int clickRow = -1, clickDataCol = -1;    // single click on an editable cell (applied after the loop)
    int doubleClickRow = -1;
    int tabDir = 0;                          // +1 / -1 when Tab / Shift+Tab was pressed in the editor
    bool editEnter = false, editEscape = false, editDeactivated = false;
    static const std::string empty;

    ImGuiListClipper clipper;
    clipper.Begin(rowCount, options.rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const FlatRecord& rec = rowAt(row);
            ImGui::TableNextRow(0, options.rowHeight);
            ImGui::PushID(row);

            const bool editingRow = editRow_ == row;
            if (editingRow) editRowDrawn = true;

            bool isSelected = selectable && (*options.selection)[row];
            if (isSelected) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Theme::u32(Theme::withAlpha(P.accent, 0.14f)));
            }

            // Row hit test: the row's vertical span clipped to the (unfrozen) body area.
            ImRect rowRect(table->WorkRect.Min.x, table->RowPosY1, table->WorkRect.Max.x, table->RowPosY2);
            rowRect.ClipWith(table->BgClipRect);
            const bool rowHovered = tableHovered && hoveredCol != selectionCol && hoveredCol != gearCol &&
                                    hoveredCol != -1 && rowRect.Contains(mouse);
            const bool rowDoubleClicked = rowHovered && mouseDoubleClicked;
            const bool rowClicked = rowHovered && mouseClicked && !mouseDoubleClicked;
            if (rowDoubleClicked) doubleClickRow = row;

            // Invisible full-row selectable for the hover highlight.
            int anchorCol = selectable ? selectionCol : firstDataCol;
            if (ImGui::TableSetColumnIndex(anchorCol)) {
                ImVec2 cursor = ImGui::GetCursorPos();
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, options.rowHeight));
                ImGui::PopStyleColor();
                ImGui::SetCursorPos(cursor);
                if (selectable) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetFrameHeight()) * 0.5f);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2.0f);
                    bool v = (*options.selection)[row];
                    if (ui::Checkbox("##sel", &v)) (*options.selection)[row] = v;
                }
            }

            bool clickedEditableCell = false;
            for (int i = 0; i < dataColumns; ++i) {
                if (!ImGui::TableSetColumnIndex(firstDataCol + i)) continue;
                const std::string& key = columns_[i].key;
                auto it = rec.find(key);
                const std::string& recordValue = it != rec.end() ? it->second : empty;
                const std::string* staged = editingRow ? pendingValue(key) : nullptr;
                const std::string& value = staged ? *staged : recordValue;

                const bool editable = canEdit && options.isEditable(row, key);
                const bool cellHovered = rowHovered && hoveredCol == firstDataCol + i;
                if (rowClicked && cellHovered && editable) {
                    clickRow = row;
                    clickDataCol = i;
                    clickedEditableCell = true;
                }

                if (editingRow && editKey_ == key) {
                    // Inline editor
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetFrameHeight()) * 0.5f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const bool firstFrame = editFocus_;
                    if (editFocus_) {
                        ImGui::SetKeyboardFocusHere();
                        editFocus_ = false;
                    }
                    ImGui::PushID(i);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, P.control);
                    ImGui::PushStyleColor(ImGuiCol_Border, P.accent);
                    bool entered = ImGui::InputText("##edit", &editBuffer_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    ImGui::PopStyleColor(2);
                    if (firstFrame && !ImGui::IsItemVisible()) ImGui::SetScrollHereX(0.5f);
                    const bool active = ImGui::IsItemActive();
                    if (active) {
                        // Own the Tab key so ImGui's keyboard navigation does not move the focus itself.
                        ImGui::SetKeyOwner(ImGuiKey_Tab, ImGui::GetItemID());
                        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) tabDir = shiftDown ? -1 : 1;
                    }
                    ImGui::PopID();
                    editEnter = entered;
                    editEscape = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                    editDeactivated = ImGui::IsItemDeactivated() && !entered && !editEscape && !firstFrame;
                    continue;
                }

                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
                ImVec2 textStart = ImGui::GetCursorScreenPos();
                if (staged) ImGui::PushStyleColor(ImGuiCol_Text, P.accent);   // changed, not saved yet
                if (!(options.cellRenderer && options.cellRenderer(row, key, value))) {
                    ImGui::TextUnformatted(value.empty() && editable ? "\xE2\x80\x94" : value.c_str());
                }
                if (staged) ImGui::PopStyleColor();
                if (editable && cellHovered) {
                    ImVec2 textEnd = ImGui::GetItemRectMax();
                    ImVec2 hitMax(ImMax(textEnd.x, textStart.x + 60.0f), textEnd.y);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(textStart.x, hitMax.y + 1), ImVec2(hitMax.x, hitMax.y + 1),
                                                        Theme::u32(P.accentLine), 1.0f);
                } else if (!editable && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) &&
                           ImGui::CalcTextSize(value.c_str()).x > ImGui::GetColumnWidth()) {
                    ui::Tooltip(value.c_str());
                }
            }

            if (rowClicked && !clickedEditableCell && !wasEditing && options.onRowClicked) options.onRowClicked(row);
            ImGui::PopID();
        }
    }

    // Editing transitions (after the loop so row indices and callbacks stay consistent)
    if (doubleClickRow >= 0) {
        // The first click of the pair may have started an edit: drop it and open the row.
        cancelEdit();
        clickRow = -1;
        if (options.onRowDoubleClicked) options.onRowDoubleClicked(doubleClickRow);
    } else if (editRow_ >= 0) {
        if (!editRowDrawn) {
            // Scrolled out of view: keep what was typed and save it.
            stageCurrentCell(editOriginal_);
            commitEdits(rowCount, rowAt, options);
        } else if (editEscape) {
            cancelEdit();
        } else if (editEnter) {
            stageCurrentCell(editOriginal_);
            commitEdits(rowCount, rowAt, options);
        } else if (tabDir != 0) {
            // Next / previous visible editable column of this row in display order.
            std::vector<int> editableCols;
            for (int d = 0; d < table->ColumnsCount; ++d) {
                int n = table->DisplayOrderToIndex[d];
                if (n < firstDataCol || n >= gearCol) continue;
                if (!(ImGui::TableGetColumnFlags(n) & ImGuiTableColumnFlags_IsEnabled)) continue;
                int i = n - firstDataCol;
                if (options.isEditable(editRow_, columns_[i].key)) editableCols.push_back(i);
            }
            int pos = -1;
            for (size_t k = 0; k < editableCols.size(); ++k) {
                if (columns_[editableCols[k]].key == editKey_) { pos = (int)k; break; }
            }
            const int next = pos + tabDir;
            stageCurrentCell(editOriginal_);
            if (pos < 0 || next < 0 || next >= (int)editableCols.size()) {
                commitEdits(rowCount, rowAt, options);   // end of the row: save
            } else {
                const int row = editRow_;
                const std::string& key = columns_[editableCols[next]].key;
                const FlatRecord& rec = rowAt(row);
                auto it = rec.find(key);
                const std::string* staged = pendingValue(key);
                beginEdit(row, key, staged ? *staged : (it != rec.end() ? it->second : empty), recordId(rec));
            }
        } else if (clickRow == editRow_ && clickDataCol >= 0 && columns_[clickDataCol].key != editKey_) {
            // Another cell of the same row: keep the changes staged and continue there.
            stageCurrentCell(editOriginal_);
            const int row = editRow_;
            const std::string& key = columns_[clickDataCol].key;
            const FlatRecord& rec = rowAt(row);
            auto it = rec.find(key);
            const std::string* staged = pendingValue(key);
            beginEdit(row, key, staged ? *staged : (it != rec.end() ? it->second : empty), recordId(rec));
            clickRow = -1;
        } else if (editDeactivated) {
            // Clicked away (another row, elsewhere in the table or outside): save the row.
            stageCurrentCell(editOriginal_);
            commitEdits(rowCount, rowAt, options);
        }
    }
    if (clickRow >= 0 && clickDataCol >= 0 && editRow_ < 0) {
        const FlatRecord& rec = rowAt(clickRow);
        const std::string& key = columns_[clickDataCol].key;
        auto it = rec.find(key);
        beginEdit(clickRow, key, it != rec.end() ? it->second : empty, recordId(rec));
    }

    // Columns popup -------------------------------------------------------
    if (openColumnsPopup) ImGui::OpenPopup("##columnsPopup");
    pendingToggles_.assign(dataColumns, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    if (ImGui::BeginPopup("##columnsPopup")) {
        ui::BoldText("Columns");
        ui::SecondaryText("Choose the information to show");
        ImGui::Spacing();
        ImGui::BeginChild("##columnList", ImVec2(260, std::min(420.0f, 30.0f * dataColumns + 8.0f)), ImGuiChildFlags_None);
        for (int i = 0; i < dataColumns; ++i) {
            bool v = enabled[i];
            ImGui::PushID(i);
            if (ui::Checkbox(columns_[i].label.c_str(), &v)) pendingToggles_[i] = v ? 1 : 2;
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Spacing();
        if (ui::SubtleButton("Show all")) {
            for (int i = 0; i < dataColumns; ++i) pendingToggles_[i] = 1;
        }
        ImGui::SameLine();
        if (ui::SubtleButton("Reset")) {
            for (int i = 0; i < dataColumns; ++i) pendingToggles_[i] = columns_[i].defaultVisible ? 1 : 2;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    for (int i = 0; i < dataColumns; ++i) {
        if (pendingToggles_[i] == 1) ImGui::TableSetColumnEnabled(firstDataCol + i, true);
        else if (pendingToggles_[i] == 2) ImGui::TableSetColumnEnabled(firstDataCol + i, false);
    }

    captureLayout(firstDataCol);

    ImGui::EndTable();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}
