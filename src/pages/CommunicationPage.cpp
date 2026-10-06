#include <cstdio>
#include <ctime>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "App.h"
#include "Async.h"
#include "ListPage.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

namespace {

struct ComposeState {
    std::string ticketId;
    std::string mobile;
    std::string message;
    bool sending = false;
};

ComposeState& C() {
    static ComposeState c;
    return c;
}

std::string dateDaysAgo(int days) {
    std::time_t t = std::time(nullptr) - (std::time_t)days * 86400;
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void sendSms(App& app) {
    ComposeState& c = C();
    long long ticket = std::atoll(c.ticketId.c_str());
    if (ticket <= 0 || c.mobile.empty() || c.message.empty() || c.sending) return;
    c.sending = true;
    RepairDeskApi client = app.api();
    std::string mobile = c.mobile, message = c.message;
    Async::run([client, ticket, mobile, message] { return client.sendTicketSms(ticket, mobile, message); },
               [&app](ApiResult<json> r) {
                   ComposeState& st = C();
                   st.sending = false;
                   if (r.ok) {
                       app.notify(r.message.empty() ? "SMS sent." : r.message, ui::InfoKind::Success);
                       st.message.clear();
                   } else {
                       app.notify("SMS failed: " + r.message, ui::InfoKind::Error);
                   }
               });
}

} // namespace

namespace pages {

void communication(App& app) {
    static ListPageState inbox("smsInboxTable");
    static bool init = false;
    static std::string fromDate, toDate;
    if (!init) {
        init = true;
        fromDate = dateDaysAgo(30);
        toDate = dateDaysAgo(0);
        inbox.preferredColumns = {
            {"date_time", "Date", 160.0f, true},
            {"direction", "Direction", 100.0f, true},
            {"from", "From", 140.0f, true},
            {"to", "To", 140.0f, true},
            {"message", "Message", 420.0f, true},
            {"status", "Status", 110.0f, true},
            {"module", "Module", 100.0f, false},
            {"module_id", "Module ID", 100.0f, false},
            {"error", "Error", 200.0f, false},
            {"id", "ID", 90.0f, false},
        };
        inbox.table.setColumns(inbox.preferredColumns);
    }

    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;
    ComposeState& c = C();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float composeW = 380.0f * dpi;
    float gap = 16.0f * dpi;

    ImGui::BeginChild("##inboxPane", ImVec2(avail.x - composeW - gap, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        auto fetch = [](const RepairDeskApi& api, const std::string& phone, int page, int ps) {
            return api.listSmsInbox(fromDate, toDate, phone, page, ps);
        };
        inbox.draw(app, "Communication", "Filter by phone number...", fetch, icons::Message,
                   [&](int, const std::string& key, const std::string& value) {
                       if (key == "direction") {
                           bool in = value.find("in") != std::string::npos;
                           ImGui::TextColored(in ? P.success : P.accent, "%s", value.c_str());
                           return true;
                       }
                       return false;
                   });
    }
    ImGui::EndChild();

    ImGui::SameLine(0, gap);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, P.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * dpi);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * dpi, 16 * dpi));
    ImGui::BeginChild("##compose", ImVec2(composeW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ui::Subtitle("Inbox range");
    ui::Label("From (YYYY-MM-DD)");
    ui::TextField("##from", fromDate, nullptr, -1);
    ui::Label("To (YYYY-MM-DD)");
    ui::TextField("##to", toDate, nullptr, -1);
    if (ui::Button((std::string(icons::Refresh) + "  Reload inbox").c_str(), ImVec2(-1, 0))) {
        inbox.page = 1;
        inbox.load(app, [](const RepairDeskApi& api, const std::string& phone, int page, int ps) {
            return api.listSmsInbox(fromDate, toDate, phone, page, ps);
        });
    }
    ui::VerticalSpace(12 * dpi);
    ui::Divider();
    ui::VerticalSpace(8 * dpi);

    ui::Subtitle((std::string(icons::Send) + "  Send SMS").c_str());
    ui::SecondaryText("Messages are sent through a ticket, as required by RepairDesk.");
    ui::Label("Ticket ID");
    ui::TextField("##ticket", c.ticketId, "e.g. 12345", -1, ImGuiInputTextFlags_CharsDecimal);
    ui::Label("Mobile number");
    ui::TextField("##mobile", c.mobile, "+1234567890", -1);
    ui::Label("Message");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, P.control);
    ImGui::InputTextMultiline("##msg", &c.message, ImVec2(-1, 120 * dpi));
    ImGui::PopStyleColor();
    char counter[48];
    std::snprintf(counter, sizeof(counter), "%zu characters", c.message.size());
    ui::SecondaryText(counter);
    ui::VerticalSpace(4 * dpi);

    bool valid = std::atoll(c.ticketId.c_str()) > 0 && !c.mobile.empty() && !c.message.empty();
    ImGui::BeginDisabled(!valid || c.sending);
    if (c.sending) { ui::Spinner("##smsSpin"); ImGui::SameLine(); }
    if (ui::AccentButton((std::string(icons::Send) + "  Send").c_str(), ImVec2(-1, 0))) sendSms(app);
    ImGui::EndDisabled();

    ImGui::EndChild();
}

} // namespace pages
