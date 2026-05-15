#include "quest_log.h"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "net/protocol.h"

namespace rco::ui {

namespace {
constexpr uint8_t kQuestLogModeSnapshot = 0;
constexpr uint8_t kQuestLogModeDelta = 1;

bool ReadQuestEntry(rco::net::Reader& reader, QuestEntry* out_entry) {
    if (!out_entry) return false;
    QuestEntry entry;
    entry.quest_id = reader.ReadU32();
    entry.state = reader.ReadU8();
    entry.code = reader.ReadString();
    entry.title = reader.ReadString();
    entry.description = reader.ReadString();
    const uint8_t objective_count = reader.ReadU8();

    entry.objectives.reserve(objective_count);
    for (uint8_t i = 0; i < objective_count && reader.OK(); ++i) {
        QuestObjectiveEntry objective;
        objective.objective_id = reader.ReadU32();
        objective.objective_type = reader.ReadU8();
        objective.description = reader.ReadString();
        objective.current_count = reader.ReadU16();
        objective.target_count = reader.ReadU16();
        entry.objectives.push_back(std::move(objective));
    }
    if (!reader.OK()) return false;
    *out_entry = std::move(entry);
    return true;
}

bool ReadAvailableQuestEntry(rco::net::Reader& reader, QuestAvailableEntry* out_entry) {
    if (!out_entry) return false;
    QuestAvailableEntry entry;
    entry.quest_id = reader.ReadU32();
    entry.code = reader.ReadString();
    entry.title = reader.ReadString();
    entry.description = reader.ReadString();
    entry.min_level = reader.ReadU16();
    entry.repeatable = reader.ReadBool();
    if (!reader.OK()) return false;
    *out_entry = std::move(entry);
    return true;
}
}

void QuestLog::Clear() {
    quests_.clear();
    available_quests_.clear();
    selected_index_ = 0;
    selected_available_index_ = 0;
}

bool QuestLog::ApplyPacket(rco::net::Reader& reader) {
    const uint8_t mode = reader.ReadU8();
    if (!reader.OK()) return false;
    switch (mode) {
        case kQuestLogModeSnapshot:
            return ApplySnapshotPacket(reader);
        case kQuestLogModeDelta:
            return ApplyDeltaPacket(reader);
        default:
            return false;
    }
}

bool QuestLog::ApplySnapshotPacket(rco::net::Reader& reader) {
    const uint16_t quest_count = reader.ReadU16();
    if (!reader.OK()) return false;

    std::vector<QuestEntry> parsed;
    parsed.reserve(quest_count);

    for (uint16_t i = 0; i < quest_count; ++i) {
        QuestEntry q;
        if (!ReadQuestEntry(reader, &q)) return false;
        parsed.push_back(std::move(q));
    }

    std::vector<QuestAvailableEntry> parsed_available;
    if (!reader.Done()) {
        const uint16_t available_count = reader.ReadU16();
        if (!reader.OK()) return false;
        parsed_available.reserve(available_count);
        for (uint16_t i = 0; i < available_count; ++i) {
            QuestAvailableEntry q;
            if (!ReadAvailableQuestEntry(reader, &q)) return false;
            parsed_available.push_back(std::move(q));
        }
    }

    quests_ = std::move(parsed);
    available_quests_ = std::move(parsed_available);
    std::sort(quests_.begin(), quests_.end(),
        [](const QuestEntry& a, const QuestEntry& b) { return a.quest_id < b.quest_id; });
    std::sort(available_quests_.begin(), available_quests_.end(),
        [](const QuestAvailableEntry& a, const QuestAvailableEntry& b) { return a.quest_id < b.quest_id; });
    ClampSelection();
    return reader.OK();
}

bool QuestLog::ApplyDeltaPacket(rco::net::Reader& reader) {
    const uint16_t quest_upsert_count = reader.ReadU16();
    if (!reader.OK()) return false;
    for (uint16_t i = 0; i < quest_upsert_count; ++i) {
        QuestEntry entry;
        if (!ReadQuestEntry(reader, &entry)) return false;
        UpsertQuestEntry(std::move(entry));
    }

    const uint16_t quest_remove_count = reader.ReadU16();
    if (!reader.OK()) return false;
    for (uint16_t i = 0; i < quest_remove_count; ++i) {
        const uint32_t quest_id = reader.ReadU32();
        if (!reader.OK()) return false;
        RemoveQuestEntry(quest_id);
    }

    const uint16_t available_upsert_count = reader.ReadU16();
    if (!reader.OK()) return false;
    for (uint16_t i = 0; i < available_upsert_count; ++i) {
        QuestAvailableEntry entry;
        if (!ReadAvailableQuestEntry(reader, &entry)) return false;
        UpsertAvailableQuest(std::move(entry));
    }

    const uint16_t available_remove_count = reader.ReadU16();
    if (!reader.OK()) return false;
    for (uint16_t i = 0; i < available_remove_count; ++i) {
        const uint32_t quest_id = reader.ReadU32();
        if (!reader.OK()) return false;
        RemoveAvailableQuest(quest_id);
    }

    std::sort(quests_.begin(), quests_.end(),
        [](const QuestEntry& a, const QuestEntry& b) { return a.quest_id < b.quest_id; });
    std::sort(available_quests_.begin(), available_quests_.end(),
        [](const QuestAvailableEntry& a, const QuestAvailableEntry& b) { return a.quest_id < b.quest_id; });
    ClampSelection();
    return reader.OK();
}

void QuestLog::UpsertQuestEntry(QuestEntry&& entry) {
    for (auto& existing : quests_) {
        if (existing.quest_id == entry.quest_id) {
            existing = std::move(entry);
            return;
        }
    }
    quests_.push_back(std::move(entry));
}

void QuestLog::RemoveQuestEntry(uint32_t quest_id) {
    quests_.erase(
        std::remove_if(quests_.begin(), quests_.end(),
            [quest_id](const QuestEntry& entry) { return entry.quest_id == quest_id; }),
        quests_.end());
}

void QuestLog::UpsertAvailableQuest(QuestAvailableEntry&& entry) {
    for (auto& existing : available_quests_) {
        if (existing.quest_id == entry.quest_id) {
            existing = std::move(entry);
            return;
        }
    }
    available_quests_.push_back(std::move(entry));
}

void QuestLog::RemoveAvailableQuest(uint32_t quest_id) {
    available_quests_.erase(
        std::remove_if(available_quests_.begin(), available_quests_.end(),
            [quest_id](const QuestAvailableEntry& entry) { return entry.quest_id == quest_id; }),
        available_quests_.end());
}

void QuestLog::ClampSelection() {
    if (quests_.empty()) {
        selected_index_ = 0;
    } else {
        if (selected_index_ < 0) selected_index_ = 0;
        if (selected_index_ >= static_cast<int>(quests_.size()))
            selected_index_ = static_cast<int>(quests_.size()) - 1;
    }
    if (available_quests_.empty()) {
        selected_available_index_ = 0;
    } else {
        if (selected_available_index_ < 0) selected_available_index_ = 0;
        if (selected_available_index_ >= static_cast<int>(available_quests_.size()))
            selected_available_index_ = static_cast<int>(available_quests_.size()) - 1;
    }
}

const char* QuestLog::StateLabel(uint8_t state) {
    switch (state) {
        case 1: return "Active";
        case 2: return "Completed";
        case 3: return "Turned In";
        case 4: return "Failed";
        case 5: return "Abandoned";
        default: return "Unknown";
    }
}

const char* QuestLog::ObjectiveTypeLabel(uint8_t objective_type) {
    switch (objective_type) {
        case 1: return "Kill";
        case 2: return "Collect";
        case 3: return "Talk";
        case 4: return "Explore";
        case 5: return "Interact";
        default: return "Objective";
    }
}

bool QuestLog::IsQuestActiveState(uint8_t state) {
    return state == 1;
}

bool QuestLog::IsQuestCompletableState(uint8_t state) {
    return state == 2;
}

void QuestLog::Render(int screen_w, int screen_h) {
    if (tracker_visible) {
        ImGui::SetNextWindowPos(
            {static_cast<float>(screen_w) - 360.f, 50.f},
            ImGuiCond_Once);
        ImGui::SetNextWindowSize({340.f, 0.f}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.66f);
        if (ImGui::Begin("Quest Tracker##quest_tracker", nullptr,
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoCollapse)) {

            int shown = 0;
            for (const auto& q : quests_) {
                if (!IsQuestActiveState(q.state) && !IsQuestCompletableState(q.state)) continue;
                if (shown >= 4) break;

                ImGui::TextColored({0.95f, 0.86f, 0.45f, 1.f}, "%s", q.title.c_str());
                if (q.objectives.empty()) {
                    ImGui::TextDisabled("No objectives");
                } else {
                    for (const auto& obj : q.objectives) {
                        const int target = std::max<int>(1, static_cast<int>(obj.target_count));
                        const int current = std::min<int>(target, static_cast<int>(obj.current_count));
                        const float fill = static_cast<float>(current) / static_cast<float>(target);
                        char label[256];
                        std::snprintf(label, sizeof(label), "%s (%d/%d)", obj.description.c_str(), current, target);
                        ImGui::ProgressBar(fill, {-1.f, 0.f}, label);
                    }
                }
                ImGui::Spacing();
                ++shown;
            }

            if (shown == 0) {
                ImGui::TextDisabled("No active quests");
            }
        }
        ImGui::End();
    }

    if (!journal_visible) return;

    ImGui::SetNextWindowPos({40.f, 40.f}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({980.f, 600.f}, ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.90f);

    if (!ImGui::Begin("Quest Journal##quest_journal", &journal_visible,
        ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    ImGui::Columns(2, "quest_journal_cols", true);
    ImGui::SetColumnWidth(0, 320.f);

    ImGui::BeginChild("##quest_list", {0.f, 0.f}, false);
    ImGui::TextDisabled("Active / Completed");
    ImGui::Separator();
    if (quests_.empty()) {
        ImGui::TextDisabled("No quest entries yet.");
    } else {
        for (int i = 0; i < static_cast<int>(quests_.size()); ++i) {
            const auto& q = quests_[i];
            char row[256];
            std::snprintf(row, sizeof(row), "[%s] %s", StateLabel(q.state), q.title.c_str());
            if (ImGui::Selectable(row, selected_index_ == i)) {
                selected_index_ = i;
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Available");
    ImGui::Separator();
    if (available_quests_.empty()) {
        ImGui::TextDisabled("No quests available right now.");
    } else {
        for (int i = 0; i < static_cast<int>(available_quests_.size()); ++i) {
            const auto& q = available_quests_[i];
            char row[256];
            std::snprintf(row, sizeof(row), "%s  (Lv %d)", q.title.c_str(), static_cast<int>(q.min_level));
            if (ImGui::Selectable(row, selected_available_index_ == i)) {
                selected_available_index_ = i;
            }
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    ImGui::BeginChild("##quest_detail", {0.f, 0.f}, false);
    if (!quests_.empty() && selected_index_ >= 0 &&
        selected_index_ < static_cast<int>(quests_.size())) {
        const auto& q = quests_[selected_index_];
        ImGui::TextColored({0.95f, 0.86f, 0.45f, 1.f}, "%s", q.title.c_str());
        ImGui::TextDisabled("%s  |  %s", q.code.c_str(), StateLabel(q.state));
        ImGui::Separator();
        ImGui::TextWrapped("%s", q.description.c_str());
        ImGui::Spacing();

        if (q.objectives.empty()) {
            ImGui::TextDisabled("No objectives");
        } else {
            for (const auto& obj : q.objectives) {
                const int target = std::max<int>(1, static_cast<int>(obj.target_count));
                const int current = std::min<int>(target, static_cast<int>(obj.current_count));
                const float fill = static_cast<float>(current) / static_cast<float>(target);
                ImGui::Text("%s - %s", ObjectiveTypeLabel(obj.objective_type), obj.description.c_str());
                char prog[64];
                std::snprintf(prog, sizeof(prog), "%d / %d", current, target);
                ImGui::ProgressBar(fill, {-1.f, 0.f}, prog);
                ImGui::Spacing();
            }
        }

        ImGui::Separator();
        if (IsQuestActiveState(q.state)) {
            if (ImGui::Button("Abandon Quest", {160.f, 30.f}) && on_action) {
                on_action(rco::net::kQuestActionAbandon, q.quest_id);
            }
        } else if (IsQuestCompletableState(q.state)) {
            if (ImGui::Button("Turn In Quest", {160.f, 30.f}) && on_action) {
                on_action(rco::net::kQuestActionTurnIn, q.quest_id);
            }
        } else {
            ImGui::TextDisabled("No actions available for this state.");
        }
    } else {
        ImGui::TextDisabled("Select a quest to inspect details.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Available Quest Selection");
    if (!available_quests_.empty() &&
        selected_available_index_ >= 0 &&
        selected_available_index_ < static_cast<int>(available_quests_.size())) {
        const auto& aq = available_quests_[selected_available_index_];
        ImGui::TextColored({0.80f, 0.90f, 1.00f, 1.0f}, "%s", aq.title.c_str());
        ImGui::TextDisabled("%s  |  Min Level %d%s",
            aq.code.c_str(),
            static_cast<int>(aq.min_level),
            aq.repeatable ? "  |  Repeatable" : "");
        ImGui::TextWrapped("%s", aq.description.c_str());
        if (ImGui::Button("Accept Selected Quest", {220.f, 30.f}) && on_action) {
            on_action(rco::net::kQuestActionAccept, aq.quest_id);
        }
    } else {
        ImGui::TextDisabled("Select an available quest from the left list.");
    }

    ImGui::EndChild();
    ImGui::Columns(1);
    ImGui::End();
}

} // namespace rco::ui
