#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
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

struct CartLine {
    long long id = 0;
    std::string name;
    std::string sku;
    double price = 0.0;
    int qty = 1;
};

struct PosState {
    std::string scan;
    std::string search;
    std::string customerId;
    std::vector<CartLine> cart;
    bool focusScan = true;
    double errorUntil = 0.0;
    bool submitting = false;
    std::string lastInvoice;
};

PosState& S() {
    static PosState s;
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

void addToCart(const InventoryItem& item) {
    PosState& s = S();
    for (CartLine& l : s.cart) {
        if (l.id == item.id) { l.qty++; return; }
    }
    CartLine l;
    l.id = item.id;
    l.name = item.name;
    l.sku = item.sku;
    l.price = item.price;
    l.qty = 1;
    s.cart.push_back(l);
}

void submitSale(App& app) {
    PosState& s = S();
    if (s.cart.empty() || s.submitting) return;
    s.submitting = true;

    json body;
    json summary;
    if (!s.customerId.empty()) summary["customer_id"] = s.customerId;
    long long now = (long long)std::time(nullptr);
    summary["created_date"] = std::to_string(now);
    summary["due_on"] = std::to_string(now);
    body["summary"] = summary;
    json items = json::array();
    for (const CartLine& l : s.cart) {
        json it;
        it["product_id"] = std::to_string(l.id);
        it["sku"] = l.sku;
        it["name"] = l.name;
        it["quantity"] = std::to_string(l.qty);
        char price[32];
        std::snprintf(price, sizeof(price), "%.2f", l.price);
        it["price"] = price;
        it["line_discount"] = "0";
        items.push_back(it);
    }
    body["items"] = items;

    RepairDeskApi client = app.api();
    Async::run([client, body] { return client.createInvoice(body); },
               [&app](ApiResult<json> r) {
                   PosState& st = S();
                   st.submitting = false;
                   if (r.ok) {
                       std::string orderId;
                       if (r.value.is_object() && r.value.contains("data") && r.value["data"].is_object()) {
                           const json& d = r.value["data"];
                           if (d.contains("order_id")) orderId = jsonToText(d["order_id"]);
                       }
                       st.lastInvoice = orderId.empty() ? "Invoice created" : "Invoice #" + orderId + " created";
                       app.notify(st.lastInvoice, ui::InfoKind::Success);
                       st.cart.clear();
                       st.customerId.clear();
                       app.refreshInventory();
                   } else {
                       app.notify("Invoice failed: " + r.message, ui::InfoKind::Error);
                   }
               });
}

} // namespace

namespace pages {

void pos(App& app) {
    PosState& s = S();
    const Palette& P = Theme::get().palette();
    const Fonts& F = Theme::get().fonts();
    const float dpi = app.dpiScale;

    ui::PageTitle("Point of sale");
    ui::VerticalSpace(4 * dpi);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float cartW = 400.0f * dpi;
    float gap = 16.0f * dpi;

    // Product picker --------------------------------------------------------
    ImGui::BeginChild("##posProducts", ImVec2(avail.x - cartW - gap, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (s.focusScan) { ImGui::SetKeyboardFocusHere(); s.focusScan = false; }
        ImGui::SetNextItemWidth(320 * dpi);
        bool entered = ImGui::InputTextWithHint("##posScan", "Scan barcode / SKU and press Enter", &s.scan,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered) {
            int idx = app.findInventoryByCode(s.scan);
            if (idx >= 0) {
                addToCart(app.inventory[idx]);
                s.errorUntil = 0;
            } else {
                MessageBeep(MB_ICONHAND);
                s.errorUntil = ImGui::GetTime() + 3.0;
            }
            s.scan.clear();
            s.focusScan = true;
        }
        ImGui::SameLine();
        ui::SearchField("posSearch", s.search, "Filter products...", 260 * dpi);
        ImGui::SameLine();
        if (ui::IconButton("posRefresh", icons::Refresh, "Refresh inventory")) app.refreshInventory();
        if (ImGui::GetTime() < s.errorUntil) {
            ImGui::TextColored(P.danger, "%s  No item found", icons::Error);
        } else {
            ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        }

        if (app.inventory.empty()) {
            if (!app.connected()) {
                ui::EmptyState(icons::Key, "Connect to RepairDesk", "Open Settings to enter an API key or sign in with OAuth2.");
            } else if (app.inventoryLoading) {
                ui::Spinner("##posLoad");
            } else {
                ui::EmptyState(icons::Package, "No products", "Refresh inventory to load products.");
            }
        } else {
            std::string needle = lower(s.search);
            ImGui::BeginChild("##productGrid", ImVec2(0, 0), ImGuiChildFlags_None);
            float cardW = 200.0f * dpi;
            float cardH = 96.0f * dpi;
            int perRow = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cardW + ImGui::GetStyle().ItemSpacing.x)));
            int shown = 0;
            for (size_t i = 0; i < app.inventory.size(); ++i) {
                const InventoryItem& it = app.inventory[i];
                if (!needle.empty() && lower(it.name).find(needle) == std::string::npos &&
                    lower(it.sku).find(needle) == std::string::npos && lower(it.upc).find(needle) == std::string::npos) {
                    continue;
                }
                if (shown % perRow != 0) ImGui::SameLine();
                ImGui::PushID((int)i);
                ImVec2 pos = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
                bool hovered = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(pos, ImVec2(pos.x + cardW, pos.y + cardH),
                                  Theme::u32(hovered ? P.controlHover : P.surface), 8.0f * dpi);
                dl->AddRect(pos, ImVec2(pos.x + cardW, pos.y + cardH),
                            Theme::u32(hovered ? P.accent : P.border), 8.0f * dpi);
                ImVec4 clip(pos.x, pos.y, pos.x + cardW, pos.y + cardH);
                dl->AddText(F.bodyBold, F.bodyBold->FontSize, ImVec2(pos.x + 12 * dpi, pos.y + 10 * dpi),
                            Theme::u32(P.text), it.name.c_str(), nullptr, cardW - 24 * dpi, &clip);
                dl->AddText(ImVec2(pos.x + 12 * dpi, pos.y + cardH - 44 * dpi), Theme::u32(P.textSecondary), it.sku.c_str());
                char price[32];
                std::snprintf(price, sizeof(price), "$%.2f", it.price);
                dl->AddText(F.subtitle, F.subtitle->FontSize, ImVec2(pos.x + 12 * dpi, pos.y + cardH - 26 * dpi),
                            Theme::u32(P.accent), price);
                char stock[32];
                std::snprintf(stock, sizeof(stock), "%d in stock", it.inStock);
                ImVec2 st = ImGui::CalcTextSize(stock);
                dl->AddText(ImVec2(pos.x + cardW - st.x - 12 * dpi, pos.y + cardH - 24 * dpi),
                            Theme::u32(it.inStock > 0 ? P.textSecondary : P.danger), stock);
                if (clicked) addToCart(it);
                ImGui::PopID();
                shown++;
                if (shown >= 400) break;
            }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();

    // Cart ------------------------------------------------------------------
    ImGui::SameLine(0, gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::BeginChild("##cart", ImVec2(cartW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ui::Subtitle((std::string(icons::Cart) + "  Cart").c_str());
    ui::Label("Customer ID (optional)");
    ui::TextField("##customer", s.customerId, "RepairDesk customer id", -1);
    ui::Divider();

    float footerH = ImGui::GetFrameHeight() * 2 + ImGui::GetTextLineHeight() * 2 + 32 * dpi;
    ImGui::BeginChild("##cartLines", ImVec2(0, ImGui::GetContentRegionAvail().y - footerH));
    if (s.cart.empty()) {
        ui::VerticalSpace(16 * dpi);
        ui::EmptyState(icons::Cart, "Cart is empty", "Scan or click a product to add it.");
    }
    int remove = -1;
    for (size_t i = 0; i < s.cart.size(); ++i) {
        CartLine& l = s.cart[i];
        ImGui::PushID((int)i);
        ImGui::PushFont(F.bodyBold);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
        ImGui::TextUnformatted(l.name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SameLine();
        ui::AlignRight(ImGui::GetFrameHeight());
        if (ui::IconButton("rm", icons::Close, "Remove")) remove = (int)i;

        if (ui::IconButton("minus", icons::Remove, "Decrease")) l.qty = std::max(1, l.qty - 1);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", l.qty);
        ImGui::SameLine();
        if (ui::IconButton("plus", icons::Add, "Increase", true)) l.qty++;
        ImGui::SameLine();
        char line[48];
        std::snprintf(line, sizeof(line), "$%.2f", l.price * l.qty);
        ui::AlignRight(ImGui::CalcTextSize(line).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(line);
        ui::Divider();
        ImGui::PopID();
    }
    if (remove >= 0) s.cart.erase(s.cart.begin() + remove);
    ImGui::EndChild();

    double total = 0;
    int count = 0;
    for (const CartLine& l : s.cart) { total += l.price * l.qty; count += l.qty; }
    char items[48], totalText[48];
    std::snprintf(items, sizeof(items), "%d item(s)", count);
    std::snprintf(totalText, sizeof(totalText), "$%.2f", total);
    ui::SecondaryText(items);
    ImGui::SameLine();
    ImGui::PushFont(F.title);
    ui::AlignRight(ImGui::CalcTextSize(totalText).x);
    ImGui::TextColored(P.accent, "%s", totalText);
    ImGui::PopFont();
    ui::SecondaryText("Tax is calculated by RepairDesk when the invoice is created.");

    ImGui::BeginDisabled(s.cart.empty() || s.submitting);
    if (ui::Button("Clear", ImVec2(100 * dpi, 0))) s.cart.clear();
    ImGui::SameLine();
    if (s.submitting) { ui::Spinner("##posSpin"); ImGui::SameLine(); }
    if (ui::AccentButton((std::string(icons::Document) + "  Create invoice").c_str(), ImVec2(-1, 0))) submitSale(app);
    ImGui::EndDisabled();

    ImGui::EndChild();
}

} // namespace pages
