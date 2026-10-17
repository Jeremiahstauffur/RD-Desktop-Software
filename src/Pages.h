#pragma once

#include <nlohmann/json.hpp>

class App;
using json = nlohmann::json;

namespace pages {

void inventory(App& app);
void stockTake(App& app);          // right-hand sidebar; drawn by the inventory page
float stockTakeWidth(const App& app);   // total width of the sidebar (+ bulk-edit panel when open)
void pos(App& app);
void tickets(App& app);
void invoices(App& app);
void customers(App& app);
void communication(App& app);
void settings(App& app);
void search(App& app);

// Shows a read-only popup listing every field of a record. Call `openRecordViewer`
// once, then `drawRecordViewer` every frame from the owning page.
void openRecordViewer(const char* title, const json& record);
void drawRecordViewer(App& app);

} // namespace pages
