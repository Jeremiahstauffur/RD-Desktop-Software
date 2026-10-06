#include "App.h"
#include "ListPage.h"
#include "Pages.h"

namespace pages {

void customers(App& app) {
    static ListPageState state("customersTable");
    static bool init = false;
    if (!init) {
        init = true;
        state.preferredColumns = {
            {"fullName", "Name", 220.0f, true},
            {"mobile", "Mobile", 150.0f, true},
            {"email", "Email", 220.0f, true},
            {"phone", "Phone", 150.0f, false},
            {"organization", "Organization", 180.0f, false},
            {"address1", "Address", 240.0f, false},
            {"city", "City", 140.0f, false},
            {"state", "State", 100.0f, false},
            {"postcode", "Post code", 100.0f, false},
            {"customer_group", "Group", 140.0f, false},
            {"created_on", "Created", 160.0f, false},
            {"id", "ID", 100.0f, false},
        };
        state.table.setColumns(state.preferredColumns);
    }
    state.draw(app, "Customers", "Search customers by name, phone or email...",
               [](const RepairDeskApi& api, const std::string& kw, int page, int ps) { return api.listCustomers(kw, page, ps); },
               icons::People);
}

} // namespace pages
