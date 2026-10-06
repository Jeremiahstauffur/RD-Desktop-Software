#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include <imgui.h>
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

struct StockTakeState {
    std::string input;
    std::vector<ScanEntry> entries;
    bool focusInput = true;
    double errorUntil = 0.0;
    std::string errorText;
    bool applying = false;
};

StockTakeState& S() {
    static StockTakeState s;
    return s;
}

void handleScan(App& app) {
    StockTakeState& s = S();
    std::string code = s.input;
    s.input.clear();
    s.focusInput = true;
    if (code.find_first_not_of(" \t\r\n") == std::string::npos) return;

    int idx = app.findInventoryByCode(code);
    if (idx < 0) {
        MessageBeep(MB_ICONHAND);
        s.errorText = "No item found";
        s.errorUntil = ImGui::GetTime() + 4.0;
        return;
    }
    s.errorUntil = 0.0;

    const InventoryItem& item = app.inventory[idx];
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

void applyToRepairDesk(App& app) {
    StockTakeState& s = S();
    if (s.entries.empty() || s.applying) return;
    s.applying = true;
    std::vector<ScanEntry> entries = s.entries;
    RepairDeskApi client = app.api();

    struct Outcome { int ok = 0; int failed = 0; std::string firstError; };
    Async::run(
        [client, entries] {
            Outcome out;
            for (const ScanEntry& e : entries) {
                json fields;
                fields["name"] = e.name;
                fields["in_stock"] = e.scanned;
                ApiResult<json> r = client.updateInventoryItem(e.id, fields);
                if (r.ok) out.ok++;
                else {
                    out.failed++;
                    if (out.firstError.empty()) out.firstError = r.message;
                }
            }
            return out;
        },
        [&app](Outcome out) {
            StockTakeState& st = S();
            st.applying = false;
            char buf[256];
            if (out.failed == 0) {
                std::snprintf(buf, sizeof(buf), "Stock take submitted: %d item(s) updated.", out.ok);
                app.notify(buf, ui::InfoKind::Success);
                st.entries.clear();
            } else {
                std::snprintf(buf, sizeof(buf), "Stock take: %d updated, %d failed. %s", out.ok, out.failed,
                              out.firstError.c_str());
                app.notify(buf, ui::InfoKind::Error);
            }
            app.refreshInventory();
        });
}

} // namespace

namespace pages {

void stockTake(App& app) {
    StockTakeState& s = S();
    const Palette& P = Theme::get().palette();
    const Fonts& F = Theme::get().fonts();
    const float dpi = app.dpiScale;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surface);
    ImGui::PushStyleColor(ImGuiCol_Border, P.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::BeginChild("##stockTake", ImVec2(380 * dpi, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // Title row ------------------------------------------------------------
    ui::Subtitle("Stock take");
    ImGui::SameLine();
    ui::AlignRight(ImGui::GetFrameHeight());
    if (ui::IconButton("closeStock", icons::Close, "Close stock take")) app.stockTakeOpen = false;
    ui::SecondaryText("Scan a barcode or type a SKU, UPC or name, then press Enter.");
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
    if (ImGui::IsItemActive()) {
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x + 4, mx.y - 1), ImVec2(mx.x - 4, mx.y - 1), Theme::u32(P.accent), 2.0f);
    }
    if (entered) handleScan(app);

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
        ImGui::TextColored(P.accent, "%d", e.scanned);
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
    if (removeIndex >= 0) s.entries.erase(s.entries.begin() + removeIndex);
    ImGui::EndChild();

    // Footer ---------------------------------------------------------------
    ui::VerticalSpace(4 * dpi);
    ImGui::BeginDisabled(s.entries.empty() || s.applying);
    if (ui::Button("Clear")) s.entries.clear();
    ImGui::SameLine();
    std::string applyLabel = std::string(icons::Save) + "  Submit";
    float applyW = ImGui::CalcTextSize(applyLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
    ui::AlignRight(applyW + (s.applying ? 28 * dpi : 0));
    if (s.applying) {
        ui::Spinner("##applySpin");
        ImGui::SameLine();
    }
    if (ui::AccentButton(applyLabel.c_str())) applyToRepairDesk(app);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !s.entries.empty()) {
        ui::Tooltip("Submits the stock take: sets each item's in-stock quantity in RepairDesk to the scanned quantity");
    }

    ImGui::EndChild();
}

} // namespace pages
