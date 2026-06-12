#pragma once

#include <imgui.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gue {
namespace ui {

inline bool SearchableComboId(
    const char* label, int& current_id,
    const std::vector<std::pair<int, std::string>>& items,
    const char* empty_label = "(none)") {
    std::string current_label = empty_label;
    for (const auto& item : items) {
        if (item.first == current_id) {
            current_label = item.second;
            break;
        }
    }

    static std::unordered_map<ImGuiID, ImGuiTextFilter> filters;
    ImGuiID id = ImGui::GetID(label);
    auto& filter = filters[id];
    bool changed = false;

    if (!ImGui::BeginCombo(label, current_label.c_str())) {
        return false;
    }

    ImGui::SetNextItemWidth(-1.0f);
    filter.Draw("##search", 200);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::Separator();

    const int none_id = 0;
    if (ImGui::Selectable(empty_label, current_id == none_id) && current_id != none_id) {
        current_id = none_id;
        changed = true;
    }

    for (const auto& item : items) {
        if (!filter.PassFilter(item.second.c_str())) {
            continue;
        }

        ImGui::PushID(item.first);
        bool sel = (item.first == current_id);
        if (ImGui::Selectable(item.second.c_str(), sel)) {
            if (current_id != item.first) {
                current_id = item.first;
                changed = true;
            }
        }
        ImGui::PopID();
    }

    ImGui::EndCombo();
    return changed;
}

inline bool SearchableComboString(
    const char* label,
    std::string& current,
    const std::vector<std::string>& items,
    const char* empty_label = "(none)") {
    const std::string& current_label = current.empty() ? empty_label : current;

    static std::unordered_map<ImGuiID, ImGuiTextFilter> filters;
    ImGuiID id = ImGui::GetID(label);
    auto& filter = filters[id];
    bool changed = false;

    if (!ImGui::BeginCombo(label, current_label.c_str())) {
        return false;
    }

    ImGui::SetNextItemWidth(-1.0f);
    filter.Draw("##search", 200);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::Separator();

    if (ImGui::Selectable(empty_label, current.empty()) && !current.empty()) {
        current.clear();
        changed = true;
    }

    for (size_t i = 0; i < items.size(); ++i) {
        const std::string& item = items[i];
        if (!filter.PassFilter(item.c_str())) {
            continue;
        }

        ImGui::PushID((int)i);
        bool sel = (item == current);
        if (ImGui::Selectable(item.c_str(), sel)) {
            if (current != item) {
                current = item;
                changed = true;
            }
        }
        ImGui::PopID();
    }

    ImGui::EndCombo();
    return changed;
}

}  // namespace ui
}  // namespace gue
