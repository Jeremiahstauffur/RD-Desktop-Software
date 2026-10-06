#include "DataTable.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include <cfloat>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "Theme.h"
#include "Widgets.h"

DataTable::DataTable(std::string id) : id_(std::move(id)) {}

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
                                44.0f);
    }
    for (int i = 0; i < dataColumns; ++i) {
        ImGuiTableColumnFlags cf = ImGuiTableColumnFlags_WidthFixed;
        if (!columns_[i].defaultVisible) cf |= ImGuiTableColumnFlags_DefaultHide;
        ImGui::TableSetupColumn(columns_[i].label.c_str(), cf, columns_[i].width, (ImGuiID)(i + 1));
    }
    ImGui::TableSetupColumn("##gear", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoReorder |
                                          ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoResize |
                                          ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHeaderLabel,
                            40.0f);
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
    ImGuiListClipper clipper;
    clipper.Begin(rowCount, options.rowHeight);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const FlatRecord& rec = rowAt(row);
            ImGui::TableNextRow(0, options.rowHeight);
            ImGui::PushID(row);

            bool isSelected = selectable && (*options.selection)[row];
            if (isSelected) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Theme::u32(Theme::withAlpha(P.accent, 0.14f)));
            }

            // Invisible full-row selectable for hover highlight / row click.
            bool rowClicked = false;
            bool cellTextClicked = false;
            int anchorCol = selectable ? selectionCol : firstDataCol;
            if (ImGui::TableSetColumnIndex(anchorCol)) {
                ImVec2 cursor = ImGui::GetCursorPos();
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                if (ImGui::Selectable("##row", false,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0, options.rowHeight))) {
                    rowClicked = true;
                }
                ImGui::PopStyleColor();
                ImGui::SetCursorPos(cursor);
                if (selectable) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetFrameHeight()) * 0.5f);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2.0f);
                    bool v = (*options.selection)[row];
                    if (ui::Checkbox("##sel", &v)) (*options.selection)[row] = v;
                }
            }

            for (int i = 0; i < dataColumns; ++i) {
                if (!ImGui::TableSetColumnIndex(firstDataCol + i)) continue;
                const std::string& key = columns_[i].key;
                auto it = rec.find(key);
                static const std::string empty;
                const std::string& value = it != rec.end() ? it->second : empty;

                const bool editable = options.isEditable && options.onCellEdited && options.isEditable(row, key);
                const bool editingThis = editable && editRow_ == row && editKey_ == key;

                if (editingThis) {
                    // Inline editor
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetFrameHeight()) * 0.5f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (editFocus_) {
                        ImGui::SetKeyboardFocusHere();
                        editFocus_ = false;
                    }
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, P.control);
                    ImGui::PushStyleColor(ImGuiCol_Border, P.accent);
                    bool commit = ImGui::InputText("##edit", &editBuffer_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    ImGui::PopStyleColor(2);
                    bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
                    bool lostFocus = !ImGui::IsItemActive() && !ImGui::IsItemFocused() && !editFocus_ &&
                                     ImGui::IsItemDeactivated();
                    if (commit || (lostFocus && !cancelled)) {
                        if (editBuffer_ != value) options.onCellEdited(row, key, editBuffer_);
                        cancelEdit();
                    } else if (cancelled) {
                        cancelEdit();
                    }
                    continue;
                }

                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (options.rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
                ImVec2 textStart = ImGui::GetCursorScreenPos();
                if (!(options.cellRenderer && options.cellRenderer(row, key, value))) {
                    ImGui::TextUnformatted(value.empty() && editable ? "\xE2\x80\x94" : value.c_str());
                }
                if (editable) {
                    // Clickable text area: at least a reasonable minimum so empty cells can be clicked.
                    ImVec2 textEnd = ImGui::GetItemRectMax();
                    float minW = 60.0f;
                    ImVec2 hitMax(ImMax(textEnd.x, textStart.x + minW), textEnd.y);
                    bool hovered = ImGui::IsMouseHoveringRect(textStart, hitMax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
                    if (hovered) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
                        ImGui::GetWindowDrawList()->AddLine(ImVec2(textStart.x, hitMax.y + 1), ImVec2(hitMax.x, hitMax.y + 1),
                                                            Theme::u32(P.accentLine), 1.0f);
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            editRow_ = row;
                            editKey_ = key;
                            editBuffer_ = value;
                            editFocus_ = true;
                            cellTextClicked = true;
                        }
                    }
                } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) &&
                           ImGui::CalcTextSize(value.c_str()).x > ImGui::GetColumnWidth()) {
                    ui::Tooltip(value.c_str());
                }
            }
            if (rowClicked && !cellTextClicked && options.onRowClicked) options.onRowClicked(row);
            ImGui::PopID();
        }
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

    ImGui::EndTable();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}
