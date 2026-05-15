#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/codec.h"

namespace rco::ui {

struct QuestObjectiveEntry {
    uint32_t    objective_id   = 0;
    uint8_t     objective_type = 0;
    std::string description;
    uint16_t    current_count  = 0;
    uint16_t    target_count   = 0;
};

struct QuestEntry {
    uint32_t                      quest_id = 0;
    uint8_t                       state    = 0;
    std::string                   code;
    std::string                   title;
    std::string                   description;
    std::vector<QuestObjectiveEntry> objectives;
};

struct QuestAvailableEntry {
    uint32_t    quest_id    = 0;
    std::string code;
    std::string title;
    std::string description;
    uint16_t    min_level   = 1;
    bool        repeatable  = false;
};

class QuestLog {
public:
    // Actions emitted by UI and sent to server using PQuestAction.
    std::function<void(uint8_t action, uint32_t quest_id)> on_action;

    // Window visibility for the full journal (toggle with J in main).
    bool journal_visible = false;

    // Compact tracker visibility (always-on HUD helper).
    bool tracker_visible = true;

    void Clear();

    // Decodes one PQuestLog packet payload (snapshot or delta mode).
    // Returns false when payload is malformed or unsupported.
    bool ApplyPacket(rco::net::Reader& reader);

    void Render(int screen_w, int screen_h);

private:
    std::vector<QuestEntry> quests_;
    std::vector<QuestAvailableEntry> available_quests_;
    int selected_index_ = 0;
    int selected_available_index_ = 0;

    bool ApplySnapshotPacket(rco::net::Reader& reader);
    bool ApplyDeltaPacket(rco::net::Reader& reader);
    void UpsertQuestEntry(QuestEntry&& entry);
    void RemoveQuestEntry(uint32_t quest_id);
    void UpsertAvailableQuest(QuestAvailableEntry&& entry);
    void RemoveAvailableQuest(uint32_t quest_id);
    void ClampSelection();

    static const char* StateLabel(uint8_t state);
    static const char* ObjectiveTypeLabel(uint8_t objective_type);
    static bool IsQuestActiveState(uint8_t state);
    static bool IsQuestCompletableState(uint8_t state);
};

} // namespace rco::ui
