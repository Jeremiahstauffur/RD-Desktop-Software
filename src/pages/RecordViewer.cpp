#include <string>

#include <imgui.h>

#include "App.h"
#include "DataTable.h"
#include "Pages.h"
#include "Theme.h"
#include "Widgets.h"

namespace {

struct ViewerState {
    bool open = false;
    bool requestOpen = false;
    std::string title;
    json record;
    FlatRecord fields;
    std::string filter;
};

ViewerState& V() {
    static ViewerState v;
    return v;
}

} // namespace

namespace pages {

void openRecordViewer(const char* title, const json& record) {
    ViewerState& v = V();
    v.title = title;
    v.record = record;
    v.fields = flattenJson(record, 4);
    v.filter.clear();
    v.requestOpen = true;
    v.open = true;
}

void drawRecordViewer(App& app) {
    ViewerState& v = V();
    const Palette& P = Theme::get().palette();
    const float dpi = app.dpiScale;

    if (v.requestOpen) {
        ImGui::OpenPopup("##recordViewer");
        v.requestOpen = false;
    }
    if (!v.open) return;

    ImGui::SetNextWindowSize(ImVec2(640 * dpi, 560 * dpi), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, P.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * dpi, 24 * dpi));
    if (ImGui::BeginPopupModal("##recordViewer", &v.open, ImGuiWindowFlags_NoTitleBar)) {
        ui::Subtitle(v.title.c_str());
        ImGui::SameLine();
        ui::AlignRight(ImGui::GetFrameHeight());
        if (ui::IconButton("closeViewer", icons::Close, "Close")) {
            v.open = false;
            ImGui::CloseCurrentPopup();
        }
        ui::SearchField("viewerFilter", v.filter, "Filter fields...", -1);
        ui::VerticalSpace(4 * dpi);

        std::string needle = toLower(v.filter);
        ImGui::BeginChild("##fields", ImVec2(0, 0), ImGuiChildFlags_None);
        if (ImGui::BeginTable("##recordTable", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            for (const auto& [key, value] : v.fields) {
                std::string label = DataTable::prettifyKey(key);
                if (!needle.empty() && toLower(label).find(needle) == std::string::npos &&
                    toLower(value).find(needle) == std::string::npos) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(P.accent, "%s", label.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::PushTextWrapPos();
                ImGui::TextUnformatted(value.empty() ? "\xE2\x80\x94" : value.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace pages
