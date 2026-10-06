#include "App.h"
#include "ListPage.h"
#include "Pages.h"

namespace pages {

void tickets(App& app) {
    static ListPageState state("ticketsTable");
    static bool init = false;
    if (!init) {
        init = true;
        state.preferredColumns = {
            {"summary.order_id", "Ticket #", 120.0f, true},
            {"summary.customer.fullName", "Customer", 200.0f, true},
            {"summary.customer.mobile", "Mobile", 140.0f, true},
            {"summary.total", "Total", 100.0f, true},
            {"summary.created_date", "Created", 160.0f, true},
            {"summary.created_by.fullname", "Created by", 160.0f, false},
            {"summary.repair_collected", "Collected", 100.0f, false},
            {"summary.source", "Source", 120.0f, false},
            {"summary.modified_on", "Modified", 160.0f, false},
            {"summary.id", "ID", 100.0f, false},
        };
        state.table.setColumns(state.preferredColumns);
    }
    state.draw(app, "Tickets", "Search tickets by keyword...",
               [](const RepairDeskApi& api, const std::string& kw, int page, int ps) { return api.listTickets(kw, page, ps); },
               icons::Repair);
}

} // namespace pages
