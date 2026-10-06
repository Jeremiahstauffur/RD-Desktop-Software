#include "App.h"
#include "ListPage.h"
#include "Pages.h"

namespace pages {

void invoices(App& app) {
    static ListPageState state("invoicesTable");
    static bool init = false;
    if (!init) {
        init = true;
        state.preferredColumns = {
            {"summary.order_id", "Invoice #", 120.0f, true},
            {"summary.customer.fullName", "Customer", 200.0f, true},
            {"summary.total", "Total", 100.0f, true},
            {"summary.status", "Status", 120.0f, true},
            {"summary.created_date", "Created", 160.0f, true},
            {"summary.due_on", "Due", 160.0f, false},
            {"summary.payment_method", "Payment", 140.0f, false},
            {"summary.id", "ID", 100.0f, false},
        };
        state.table.setColumns(state.preferredColumns);
    }
    state.draw(app, "Invoices", "Search invoices by keyword...",
               [](const RepairDeskApi& api, const std::string& kw, int page, int ps) { return api.listInvoices(kw, page, ps); },
               icons::Document);
}

} // namespace pages
