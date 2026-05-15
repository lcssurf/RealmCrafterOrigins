#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

namespace gue {

struct QuestDefinition {
    int         id = 0;
    std::string code;
    std::string title;
    std::string description;
    int         min_level = 1;
    bool        repeatable = false;
    bool        auto_accept = false;
    int         prerequisite_quest_id = 0;
    bool        is_active = true;
};

struct QuestObjectiveDefinition {
    int         id = 0;
    int         quest_id = 0;
    int         objective_order = 1;
    int         objective_type = 1; // 1=kill 2=collect 3=talk 4=explore 5=interact
    std::string description;
    std::string target_npc_name;
    int         target_item_id = 0;
    std::string target_area_name;
    int         target_count = 1;
};

struct QuestRewardDefinition {
    int id = 0;
    int quest_id = 0;
    int xp_reward = 0;
    int gold_reward = 0;
    int item_id = 0;
    int item_qty = 0;
};

struct QuestItemOption {
    int         id = 0;
    std::string name;
};

class QuestsTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);

    void FetchQuests(sqlite3* db);
    void FetchObjectives(sqlite3* db, int quest_id);
    void FetchRewards(sqlite3* db, int quest_id);
    void FetchItems(sqlite3* db);

    bool SaveQuest(sqlite3* db, QuestDefinition& quest, bool is_new);
    bool DeleteQuest(sqlite3* db, int quest_id);
    bool DuplicateQuest(sqlite3* db, int quest_id);

    bool SaveObjective(sqlite3* db, QuestObjectiveDefinition& objective);
    bool DeleteObjective(sqlite3* db, int objective_id);

    bool SaveReward(sqlite3* db, QuestRewardDefinition& reward);
    bool DeleteReward(sqlite3* db, int reward_id);

    bool ValidateQuest(sqlite3* db, const QuestDefinition& quest, bool is_new, std::string* out_error) const;
    bool ValidateObjective(const QuestObjectiveDefinition& objective, std::string* out_error) const;
    bool ValidateReward(const QuestRewardDefinition& reward, std::string* out_error) const;

    int  BuildNextQuestCopySuffix(sqlite3* db, const std::string& code_base) const;
    void SetStatus(const char* fmt, ...);

    bool tables_ensured_ = false;
    bool need_fetch_quests_ = true;
    bool need_fetch_children_ = false;
    bool need_fetch_items_ = true;
    int  select_quest_after_fetch_ = 0;

    std::vector<QuestDefinition> quests_;
    std::vector<QuestObjectiveDefinition> objectives_;
    std::vector<QuestRewardDefinition> rewards_;
    std::vector<QuestItemOption> items_;

    int selected_quest_ = -1;
    int selected_objective_ = -1;
    int selected_reward_ = -1;

    bool show_new_quest_ = false;
    bool show_new_objective_ = false;
    bool show_new_reward_ = false;

    bool dirty_quest_ = false;
    bool dirty_objective_ = false;
    bool dirty_reward_ = false;

    QuestDefinition new_quest_;
    QuestDefinition editing_quest_;
    QuestObjectiveDefinition new_objective_;
    QuestObjectiveDefinition editing_objective_;
    QuestRewardDefinition new_reward_;
    QuestRewardDefinition editing_reward_;

    char status_msg_[256] = {};
};

} // namespace gue
