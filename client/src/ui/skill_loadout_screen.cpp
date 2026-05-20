#include "skill_loadout_screen.h"
#include "util.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "../gameplay/kit_pool.h"
#include "../gameplay/skill_state.h"

namespace rco::ui {

void SkillLoadoutScreen::Render(int screen_w, int screen_h) {
    if (!open_) return;

    auto* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled({0.f, 0.f},
                      {static_cast<float>(screen_w), static_cast<float>(screen_h)},
                      IM_COL32(0, 0, 0, 155));

    const float window_w = std::min(1020.f, static_cast<float>(screen_w) - 80.f);
    const float window_h = std::min(700.f, static_cast<float>(screen_h) - 80.f);
    ImGui::SetNextWindowPos({(screen_w - window_w) * 0.5f, (screen_h - window_h) * 0.5f},
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize({window_w, window_h}, ImGuiCond_Always);

    if (!ImGui::Begin("Skill Loadout", &open_,
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Configure your active kit loadout. Click a skill on the left to assign "
                       "it to the first empty hotbar slot. Click a filled slot to clear it.");
    ImGui::Separator();

    const auto& state = rco::gameplay::PlayerSkillState();
    const auto& pool = rco::gameplay::ActiveKitPool();

    const uint32_t active_kit_id = state.kit_id() != 0 ? state.kit_id() : pool.kit_id();
    if (active_kit_id == 0) {
        ImGui::TextDisabled("Equip an item with an active kit to configure loadout.");
        ImGui::End();
        return;
    }

    const std::string active_key =
        !state.kit_key().empty() ? state.kit_key() : pool.kit_key();
    const std::string active_display =
        !state.kit_display_name().empty() ? state.kit_display_name() : pool.kit_display_name();

    int slot_count = 4;
    for (const auto& ab : state.abilities()) {
        slot_count = std::max(slot_count, static_cast<int>(ab.slot_index) + 1);
    }
    slot_count = std::clamp(slot_count, 1, 16);

    std::vector<const rco::gameplay::SkillStateAbility*> by_slot(
        static_cast<std::size_t>(slot_count), nullptr);
    std::unordered_map<uint32_t, uint8_t> level_by_ability_id;
    std::unordered_map<uint32_t, std::string> description_by_ability_id;
    std::unordered_map<uint32_t, const rco::gameplay::SkillStateAbility*> ability_by_id;
    for (const auto& ab : state.abilities()) {
        const int idx = static_cast<int>(ab.slot_index);
        if (idx >= 0 && idx < slot_count) {
            by_slot[static_cast<std::size_t>(idx)] = &ab;
        }
        if (ab.ability_id != 0) {
            ability_by_id[ab.ability_id] = &ab;
            level_by_ability_id[ab.ability_id] = ab.mastery_level;
            if (!ab.description.empty()) {
                description_by_ability_id[ab.ability_id] = ab.description;
            }
        }
    }

    ImGui::Text("Kit: %s  [%s]  (id=%u)",
                active_display.empty() ? "(unnamed)" : active_display.c_str(),
                active_key.empty() ? "unknown" : active_key.c_str(),
                active_kit_id);
    if (status_msg_[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_msg_);
    }
    ImGui::Separator();

    if (ImGui::BeginTable("##skill_loadout_table", 2,
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pool", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableSetupColumn("Hotbar", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##kit_pool", {0.f, 0.f}, false);
        ImGui::TextDisabled("Available Skills");
        ImGui::Separator();
        if (pool.kit_id() != active_kit_id) {
            ImGui::TextDisabled("Waiting for active kit pool sync...");
        } else if (pool.abilities().empty()) {
            ImGui::TextDisabled("No abilities available in this kit.");
        } else {
            for (const auto& ab : pool.abilities()) {
                uint8_t mastery_level = 1;
                auto it = level_by_ability_id.find(ab.ability_id);
                if (it != level_by_ability_id.end() && it->second > 0) {
                    mastery_level = it->second;
                }
                char label[256];
                std::snprintf(label, sizeof(label), "%s (Lv %u)##pool_%u",
                              ab.ability_name.c_str(),
                              static_cast<unsigned>(mastery_level),
                              ab.ability_id);
                if (ImGui::Button(label, {-FLT_MIN, 0.f})) {
                    bool already_assigned = false;
                    for (const auto* slot_ab : by_slot) {
                        if (slot_ab != nullptr && slot_ab->ability_id == ab.ability_id) {
                            already_assigned = true;
                            break;
                        }
                    }
                    if (already_assigned) {
                        std::snprintf(status_msg_, sizeof(status_msg_),
                                      "Ability '%s' is already assigned.",
                                      ab.ability_name.c_str());
                    } else {
                        int empty_slot = -1;
                        for (int i = 0; i < slot_count; ++i) {
                            if (by_slot[static_cast<std::size_t>(i)] == nullptr) {
                                empty_slot = i;
                                break;
                            }
                        }
                        if (empty_slot < 0) {
                            std::snprintf(status_msg_, sizeof(status_msg_),
                                          "No empty slot. Clear one first.");
                        } else if (on_set_slot) {
                            on_set_slot(active_kit_id, static_cast<uint8_t>(empty_slot), ab.ability_id);
                            std::snprintf(status_msg_, sizeof(status_msg_),
                                          "Assign request sent: slot %d <- %s.",
                                          empty_slot + 1, ab.ability_name.c_str());
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    const auto desc_it = description_by_ability_id.find(ab.ability_id);
                    const bool has_desc = (desc_it != description_by_ability_id.end() &&
                                           !desc_it->second.empty());
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", ab.ability_name.c_str());
                    if (has_desc) {
                        ImGui::Separator();
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
                        ImGui::TextUnformatted(desc_it->second.c_str());
                        ImGui::PopTextWrapPos();
                    } else {
                        ImGui::Separator();
                        ImGui::TextDisabled("No description available.");
                    }
                    ImGui::Separator();
                    ImGui::Text("Cooldown: %u ms", ab.cooldown_ms);
                    const auto ability_it = ability_by_id.find(ab.ability_id);
                    if (ability_it != ability_by_id.end()) {
                        const auto* full_ab = ability_it->second;
                        if (full_ab->mastery_level > 0 && full_ab->mastery_max_level > 0) {
                            if (full_ab->mastery_level >= full_ab->mastery_max_level) {
                                ImGui::TextColored(ImVec4(1.f, 0.84f, 0.f, 1.f), "MAX LEVEL");
                            } else {
                                const float pct = ProgressBetweenThresholds(
                                    full_ab->mastery_xp,
                                    full_ab->mastery_xp_current_level_thr,
                                    full_ab->mastery_xp_for_next);
                                const std::string xp_value = AbbreviateNumber(full_ab->mastery_xp);
                                const std::string xp_next = AbbreviateNumber(full_ab->mastery_xp_for_next);
                                ImGui::Text("%.1f%%", pct * 100.0f);
                                ImGui::Text("%s / %s", xp_value.c_str(), xp_next.c_str());
                            }
                        }
                    }
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##kit_hotbar_slots", {0.f, 0.f}, false);
        ImGui::TextDisabled("Hotbar Slots");
        ImGui::Separator();

        for (int i = 0; i < slot_count; ++i) {
            const auto* slot_ab = by_slot[static_cast<std::size_t>(i)];
            if (slot_ab != nullptr) {
                uint8_t mastery_level = slot_ab->mastery_level > 0 ? slot_ab->mastery_level : 1;
                char label[256];
                std::snprintf(label, sizeof(label), "[%d] %s (Lv %u)##slot_%d",
                              i + 1,
                              slot_ab->ability_name.c_str(),
                              static_cast<unsigned>(mastery_level),
                              i);
                if (ImGui::Button(label, {-FLT_MIN, 0.f}) && on_clear_slot) {
                    on_clear_slot(active_kit_id, static_cast<uint8_t>(i));
                    std::snprintf(status_msg_, sizeof(status_msg_),
                                  "Clear request sent: slot %d.", i + 1);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", slot_ab->ability_name.c_str());
                    if (!slot_ab->description.empty()) {
                        ImGui::Separator();
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
                        ImGui::TextUnformatted(slot_ab->description.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::Separator();
                    ImGui::Text("Cooldown: %u ms", slot_ab->cooldown_ms);
                    if (slot_ab->mastery_level > 0 && slot_ab->mastery_max_level > 0) {
                        if (slot_ab->mastery_level >= slot_ab->mastery_max_level) {
                            ImGui::TextColored(ImVec4(1.f, 0.84f, 0.f, 1.f), "MAX LEVEL");
                        } else {
                            const float pct = ProgressBetweenThresholds(
                                slot_ab->mastery_xp,
                                slot_ab->mastery_xp_current_level_thr,
                                slot_ab->mastery_xp_for_next);
                            const std::string xp_value = AbbreviateNumber(slot_ab->mastery_xp);
                            const std::string xp_next = AbbreviateNumber(slot_ab->mastery_xp_for_next);
                            ImGui::Text("%.1f%%", pct * 100.0f);
                            ImGui::Text("%s / %s", xp_value.c_str(), xp_next.c_str());
                        }
                    }
                    ImGui::EndTooltip();
                }
            } else {
                ImGui::BeginDisabled();
                char label[64];
                std::snprintf(label, sizeof(label), "[%d] (empty)", i + 1);
                ImGui::Button(label, {-FLT_MIN, 0.f});
                ImGui::EndDisabled();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Clear Entire Kit Loadout", {-FLT_MIN, 0.f}) && on_clear_kit) {
            on_clear_kit(active_kit_id);
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Clear-kit request sent.");
        }

        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace rco::ui
