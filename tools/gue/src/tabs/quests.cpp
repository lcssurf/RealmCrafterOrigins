#include "quests.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gue {

namespace {

static const char* kObjectiveTypeNames[] = {
    "Kill NPC",
    "Collect Item",
    "Talk to NPC",
    "Explore Area",
    "Interact",
};
static constexpr int kObjectiveTypeCount = 5;

std::string TrimCopy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool IsQuestCodeChar(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

void NormalizeQuestCodeInPlace(std::string* code) {
    if (!code) return;
    for (char& ch : *code) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == ' ' || ch == '-') ch = '_';
    }
}

std::string ObjectiveTypeLabel(int objective_type) {
    if (objective_type < 1 || objective_type > kObjectiveTypeCount) {
        return "Unknown";
    }
    return kObjectiveTypeNames[objective_type - 1];
}

int IndexByItemID(const std::vector<QuestItemOption>& items, int item_id) {
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (items[i].id == item_id) return i;
    }
    return -1;
}

} // namespace

void QuestsTab::SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(status_msg_, sizeof(status_msg_), fmt, args);
    va_end(args);
}

void QuestsTab::EnsureTables(sqlite3* db) {
    if (tables_ensured_) return;

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS quest_defs ("
        "  id                    INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  code                  TEXT NOT NULL UNIQUE,"
        "  title                 TEXT NOT NULL DEFAULT '',"
        "  description           TEXT NOT NULL DEFAULT '',"
        "  min_level             INTEGER NOT NULL DEFAULT 1,"
        "  repeatable            INTEGER NOT NULL DEFAULT 0,"
        "  auto_accept           INTEGER NOT NULL DEFAULT 0,"
        "  prerequisite_quest_id INTEGER NOT NULL DEFAULT 0,"
        "  is_active             INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS quest_objective_defs ("
        "  id               INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  quest_id         INTEGER NOT NULL DEFAULT 0,"
        "  objective_order  INTEGER NOT NULL DEFAULT 0,"
        "  objective_type   INTEGER NOT NULL DEFAULT 1,"
        "  description      TEXT NOT NULL DEFAULT '',"
        "  target_npc_name  TEXT NOT NULL DEFAULT '',"
        "  target_item_id   INTEGER NOT NULL DEFAULT 0,"
        "  target_area_name TEXT NOT NULL DEFAULT '',"
        "  target_count     INTEGER NOT NULL DEFAULT 1"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS quest_reward_defs ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  quest_id    INTEGER NOT NULL DEFAULT 0,"
        "  xp_reward   INTEGER NOT NULL DEFAULT 0,"
        "  gold_reward INTEGER NOT NULL DEFAULT 0,"
        "  item_id     INTEGER NOT NULL DEFAULT 0,"
        "  item_qty    INTEGER NOT NULL DEFAULT 0"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_quest_objective_defs_quest"
        " ON quest_objective_defs(quest_id)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_quest_reward_defs_quest"
        " ON quest_reward_defs(quest_id)",
        nullptr, nullptr, nullptr);

    tables_ensured_ = true;
}

void QuestsTab::FetchItems(sqlite3* db) {
    items_.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, name FROM item_templates ORDER BY name", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Item list error: %s", sqlite3_errmsg(db));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QuestItemOption entry;
        entry.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) {
            entry.name = reinterpret_cast<const char*>(text);
        }
        items_.push_back(std::move(entry));
    }
    sqlite3_finalize(stmt);
}

void QuestsTab::FetchQuests(sqlite3* db) {
    EnsureTables(db);

    quests_.clear();
    objectives_.clear();
    rewards_.clear();
    selected_quest_ = -1;
    selected_objective_ = -1;
    selected_reward_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, code, title, description, min_level, repeatable, auto_accept, prerequisite_quest_id, is_active "
        "FROM quest_defs ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Quest fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QuestDefinition quest;
        quest.id = sqlite3_column_int(stmt, 0);
        if (const auto* text = sqlite3_column_text(stmt, 1)) quest.code = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 2)) quest.title = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 3)) quest.description = reinterpret_cast<const char*>(text);
        quest.min_level = sqlite3_column_int(stmt, 4);
        quest.repeatable = sqlite3_column_int(stmt, 5) != 0;
        quest.auto_accept = sqlite3_column_int(stmt, 6) != 0;
        quest.prerequisite_quest_id = sqlite3_column_int(stmt, 7);
        quest.is_active = sqlite3_column_int(stmt, 8) != 0;
        quests_.push_back(std::move(quest));
    }
    sqlite3_finalize(stmt);

    if (!quests_.empty()) {
        if (select_quest_after_fetch_ > 0) {
            for (int i = 0; i < static_cast<int>(quests_.size()); ++i) {
                if (quests_[i].id == select_quest_after_fetch_) {
                    selected_quest_ = i;
                    break;
                }
            }
        }
        if (selected_quest_ < 0) selected_quest_ = 0;
        editing_quest_ = quests_[selected_quest_];
        need_fetch_children_ = true;
    }
    select_quest_after_fetch_ = 0;
    SetStatus("Loaded %d quests.", static_cast<int>(quests_.size()));
}

void QuestsTab::FetchObjectives(sqlite3* db, int quest_id) {
    objectives_.clear();
    selected_objective_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, quest_id, objective_order, objective_type, description, "
        "       target_npc_name, target_item_id, target_area_name, target_count "
        "FROM quest_objective_defs WHERE quest_id=? ORDER BY objective_order, id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Objective fetch error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, quest_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QuestObjectiveDefinition objective;
        objective.id = sqlite3_column_int(stmt, 0);
        objective.quest_id = sqlite3_column_int(stmt, 1);
        objective.objective_order = sqlite3_column_int(stmt, 2);
        objective.objective_type = sqlite3_column_int(stmt, 3);
        if (const auto* text = sqlite3_column_text(stmt, 4)) objective.description = reinterpret_cast<const char*>(text);
        if (const auto* text = sqlite3_column_text(stmt, 5)) objective.target_npc_name = reinterpret_cast<const char*>(text);
        objective.target_item_id = sqlite3_column_int(stmt, 6);
        if (const auto* text = sqlite3_column_text(stmt, 7)) objective.target_area_name = reinterpret_cast<const char*>(text);
        objective.target_count = sqlite3_column_int(stmt, 8);
        objectives_.push_back(std::move(objective));
    }
    sqlite3_finalize(stmt);
}

void QuestsTab::FetchRewards(sqlite3* db, int quest_id) {
    rewards_.clear();
    selected_reward_ = -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, quest_id, xp_reward, gold_reward, item_id, item_qty "
        "FROM quest_reward_defs WHERE quest_id=? ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Reward fetch error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, quest_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QuestRewardDefinition reward;
        reward.id = sqlite3_column_int(stmt, 0);
        reward.quest_id = sqlite3_column_int(stmt, 1);
        reward.xp_reward = sqlite3_column_int(stmt, 2);
        reward.gold_reward = sqlite3_column_int(stmt, 3);
        reward.item_id = sqlite3_column_int(stmt, 4);
        reward.item_qty = sqlite3_column_int(stmt, 5);
        rewards_.push_back(std::move(reward));
    }
    sqlite3_finalize(stmt);
}

bool QuestsTab::ValidateQuest(sqlite3* db, const QuestDefinition& quest, bool is_new, std::string* out_error) const {
    const std::string code = TrimCopy(quest.code);
    if (code.empty()) {
        if (out_error) *out_error = "Quest code is required.";
        return false;
    }
    for (char ch : code) {
        if (!IsQuestCodeChar(ch)) {
            if (out_error) *out_error = "Quest code must use lowercase letters, numbers, or underscore.";
            return false;
        }
    }
    if (TrimCopy(quest.title).empty()) {
        if (out_error) *out_error = "Quest title is required.";
        return false;
    }
    if (quest.min_level < 1) {
        if (out_error) *out_error = "Minimum level must be >= 1.";
        return false;
    }
    if (quest.id > 0 && quest.prerequisite_quest_id == quest.id) {
        if (out_error) *out_error = "Quest cannot require itself as prerequisite.";
        return false;
    }
    if (quest.prerequisite_quest_id > 0) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM quest_defs WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
            if (out_error) *out_error = "Could not validate prerequisite quest.";
            return false;
        }
        sqlite3_bind_int(stmt, 1, quest.prerequisite_quest_id);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count == 0) {
            if (out_error) *out_error = "Prerequisite quest ID does not exist.";
            return false;
        }
    }

    sqlite3_stmt* dup = nullptr;
    if (is_new) {
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM quest_defs WHERE code=?", -1, &dup, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(dup, 1, code.c_str(), -1, SQLITE_TRANSIENT);
            int count = 0;
            if (sqlite3_step(dup) == SQLITE_ROW) count = sqlite3_column_int(dup, 0);
            sqlite3_finalize(dup);
            if (count > 0) {
                if (out_error) *out_error = "Quest code already exists.";
                return false;
            }
        }
    } else {
        if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM quest_defs WHERE code=? AND id<>?", -1, &dup, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(dup, 1, code.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(dup, 2, quest.id);
            int count = 0;
            if (sqlite3_step(dup) == SQLITE_ROW) count = sqlite3_column_int(dup, 0);
            sqlite3_finalize(dup);
            if (count > 0) {
                if (out_error) *out_error = "Quest code already exists.";
                return false;
            }
        }
    }
    return true;
}

bool QuestsTab::ValidateObjective(const QuestObjectiveDefinition& objective, std::string* out_error) const {
    if (objective.quest_id <= 0) {
        if (out_error) *out_error = "Objective is not bound to a quest.";
        return false;
    }
    if (objective.objective_order < 1) {
        if (out_error) *out_error = "Objective order must be >= 1.";
        return false;
    }
    if (objective.objective_type < 1 || objective.objective_type > kObjectiveTypeCount) {
        if (out_error) *out_error = "Objective type is invalid.";
        return false;
    }
    if (TrimCopy(objective.description).empty()) {
        if (out_error) *out_error = "Objective description is required.";
        return false;
    }
    if (objective.target_count < 1) {
        if (out_error) *out_error = "Objective target count must be >= 1.";
        return false;
    }

    switch (objective.objective_type) {
        case 1: // kill
        case 3: // talk
        case 5: // interact
            if (TrimCopy(objective.target_npc_name).empty()) {
                if (out_error) *out_error = "Target NPC name is required for this objective type.";
                return false;
            }
            break;
        case 2: // collect
            if (objective.target_item_id <= 0) {
                if (out_error) *out_error = "Target item ID is required for collect objectives.";
                return false;
            }
            break;
        case 4: // explore
            if (TrimCopy(objective.target_area_name).empty()) {
                if (out_error) *out_error = "Target area name is required for explore objectives.";
                return false;
            }
            break;
        default:
            break;
    }
    return true;
}

bool QuestsTab::ValidateReward(const QuestRewardDefinition& reward, std::string* out_error) const {
    if (reward.quest_id <= 0) {
        if (out_error) *out_error = "Reward is not bound to a quest.";
        return false;
    }
    if (reward.xp_reward < 0 || reward.gold_reward < 0) {
        if (out_error) *out_error = "Reward XP and gold cannot be negative.";
        return false;
    }
    if (reward.item_id < 0 || reward.item_qty < 0) {
        if (out_error) *out_error = "Reward item fields cannot be negative.";
        return false;
    }
    if (reward.item_id == 0 && reward.item_qty > 0) {
        if (out_error) *out_error = "Item quantity requires a valid item selection.";
        return false;
    }
    if (reward.item_id > 0 && reward.item_qty < 1) {
        if (out_error) *out_error = "Item reward must have quantity >= 1.";
        return false;
    }
    if (reward.item_id > 65535) {
        if (out_error) *out_error = "Item reward ID must be <= 65535.";
        return false;
    }
    if (reward.item_qty > 255) {
        if (out_error) *out_error = "Item reward quantity must be <= 255.";
        return false;
    }
    if (reward.xp_reward == 0 && reward.gold_reward == 0 && reward.item_id == 0) {
        if (out_error) *out_error = "Reward must grant XP, gold, or an item.";
        return false;
    }
    return true;
}

bool QuestsTab::SaveQuest(sqlite3* db, QuestDefinition& quest, bool is_new) {
    quest.code = TrimCopy(quest.code);
    quest.title = TrimCopy(quest.title);
    quest.description = TrimCopy(quest.description);
    NormalizeQuestCodeInPlace(&quest.code);
    if (quest.min_level < 1) quest.min_level = 1;
    if (quest.prerequisite_quest_id < 0) quest.prerequisite_quest_id = 0;

    std::string validation_error;
    if (!ValidateQuest(db, quest, is_new, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (is_new) {
        const char* sql =
            "INSERT INTO quest_defs "
            "(code, title, description, min_level, repeatable, auto_accept, prerequisite_quest_id, is_active) "
            "VALUES (?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Quest create error: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_text(stmt, 1, quest.code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, quest.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, quest.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, quest.min_level);
        sqlite3_bind_int(stmt, 5, quest.repeatable ? 1 : 0);
        sqlite3_bind_int(stmt, 6, quest.auto_accept ? 1 : 0);
        sqlite3_bind_int(stmt, 7, quest.prerequisite_quest_id);
        sqlite3_bind_int(stmt, 8, quest.is_active ? 1 : 0);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            quest.id = static_cast<int>(sqlite3_last_insert_rowid(db));
        }
    } else {
        const char* sql =
            "UPDATE quest_defs SET "
            "code=?, title=?, description=?, min_level=?, repeatable=?, auto_accept=?, prerequisite_quest_id=?, is_active=? "
            "WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Quest save error: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_text(stmt, 1, quest.code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, quest.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, quest.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, quest.min_level);
        sqlite3_bind_int(stmt, 5, quest.repeatable ? 1 : 0);
        sqlite3_bind_int(stmt, 6, quest.auto_accept ? 1 : 0);
        sqlite3_bind_int(stmt, 7, quest.prerequisite_quest_id);
        sqlite3_bind_int(stmt, 8, quest.is_active ? 1 : 0);
        sqlite3_bind_int(stmt, 9, quest.id);
        rc = sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Quest save error: %s", sqlite3_errmsg(db));
        return false;
    }

    select_quest_after_fetch_ = quest.id;
    need_fetch_quests_ = true;
    dirty_quest_ = false;
    show_new_quest_ = false;
    SetStatus("Saved quest '%s' (id=%d).", quest.code.c_str(), quest.id);
    return true;
}

bool QuestsTab::DeleteQuest(sqlite3* db, int quest_id) {
    sqlite3_stmt* guard = nullptr;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM character_quests WHERE quest_id=?", -1, &guard, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(guard, 1, quest_id);
        int used_count = 0;
        if (sqlite3_step(guard) == SQLITE_ROW) used_count = sqlite3_column_int(guard, 0);
        sqlite3_finalize(guard);
        if (used_count > 0) {
            SetStatus("Cannot delete quest %d: already used by %d character quest record(s).", quest_id, used_count);
            return false;
        }
    }

    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM quest_defs WHERE prerequisite_quest_id=?", -1, &guard, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(guard, 1, quest_id);
        int dependent = 0;
        if (sqlite3_step(guard) == SQLITE_ROW) dependent = sqlite3_column_int(guard, 0);
        sqlite3_finalize(guard);
        if (dependent > 0) {
            SetStatus("Cannot delete quest %d: used as prerequisite by %d quest(s).", quest_id, dependent);
            return false;
        }
    }

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = nullptr;
    auto delete_by_quest = [&](const char* sql) -> bool {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, quest_id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    };

    bool ok =
        delete_by_quest("DELETE FROM quest_reward_defs WHERE quest_id=?") &&
        delete_by_quest("DELETE FROM quest_objective_defs WHERE quest_id=?") &&
        delete_by_quest("DELETE FROM quest_defs WHERE id=?");

    if (!ok) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Quest delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    need_fetch_quests_ = true;
    selected_quest_ = -1;
    SetStatus("Deleted quest %d.", quest_id);
    return true;
}

int QuestsTab::BuildNextQuestCopySuffix(sqlite3* db, const std::string& code_base) const {
    int suffix = 1;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM quest_defs WHERE code=?", -1, &stmt, nullptr) != SQLITE_OK) {
        return suffix;
    }
    while (suffix < 1000) {
        std::string candidate = code_base + "_copy";
        if (suffix > 1) candidate += std::to_string(suffix);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
        int exists = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = sqlite3_column_int(stmt, 0);
        if (exists == 0) {
            sqlite3_finalize(stmt);
            return suffix;
        }
        ++suffix;
    }
    sqlite3_finalize(stmt);
    return suffix;
}

bool QuestsTab::DuplicateQuest(sqlite3* db, int quest_id) {
    sqlite3_stmt* read_quest = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT code, title, description, min_level, repeatable, auto_accept, prerequisite_quest_id, is_active "
        "FROM quest_defs WHERE id=?",
        -1, &read_quest, nullptr) != SQLITE_OK) {
        SetStatus("Duplicate error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(read_quest, 1, quest_id);
    if (sqlite3_step(read_quest) != SQLITE_ROW) {
        sqlite3_finalize(read_quest);
        SetStatus("Duplicate error: source quest %d not found.", quest_id);
        return false;
    }

    QuestDefinition source;
    source.id = quest_id;
    if (const auto* text = sqlite3_column_text(read_quest, 0)) source.code = reinterpret_cast<const char*>(text);
    if (const auto* text = sqlite3_column_text(read_quest, 1)) source.title = reinterpret_cast<const char*>(text);
    if (const auto* text = sqlite3_column_text(read_quest, 2)) source.description = reinterpret_cast<const char*>(text);
    source.min_level = sqlite3_column_int(read_quest, 3);
    source.repeatable = sqlite3_column_int(read_quest, 4) != 0;
    source.auto_accept = sqlite3_column_int(read_quest, 5) != 0;
    source.prerequisite_quest_id = sqlite3_column_int(read_quest, 6);
    source.is_active = sqlite3_column_int(read_quest, 7) != 0;
    sqlite3_finalize(read_quest);

    int copy_suffix = BuildNextQuestCopySuffix(db, source.code);
    QuestDefinition duplicate = source;
    duplicate.id = 0;
    duplicate.code = source.code + "_copy";
    if (copy_suffix > 1) duplicate.code += std::to_string(copy_suffix);
    duplicate.title = source.title + " (Copy)";
    duplicate.prerequisite_quest_id = 0;
    duplicate.is_active = false;

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* insert_copy = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO quest_defs "
        "(code, title, description, min_level, repeatable, auto_accept, prerequisite_quest_id, is_active) "
        "VALUES (?,?,?,?,?,?,?,?)",
        -1, &insert_copy, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Duplicate insert error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(insert_copy, 1, duplicate.code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_copy, 2, duplicate.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_copy, 3, duplicate.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_copy, 4, duplicate.min_level);
    sqlite3_bind_int(insert_copy, 5, duplicate.repeatable ? 1 : 0);
    sqlite3_bind_int(insert_copy, 6, duplicate.auto_accept ? 1 : 0);
    sqlite3_bind_int(insert_copy, 7, duplicate.prerequisite_quest_id);
    sqlite3_bind_int(insert_copy, 8, duplicate.is_active ? 1 : 0);
    int rc_insert = sqlite3_step(insert_copy);
    sqlite3_finalize(insert_copy);
    if (rc_insert != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Duplicate insert error: %s", sqlite3_errmsg(db));
        return false;
    }
    const int new_quest_id = static_cast<int>(sqlite3_last_insert_rowid(db));

    sqlite3_stmt* copy_obj = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO quest_objective_defs "
        "(quest_id, objective_order, objective_type, description, target_npc_name, target_item_id, target_area_name, target_count) "
        "SELECT ?, objective_order, objective_type, description, target_npc_name, target_item_id, target_area_name, target_count "
        "FROM quest_objective_defs WHERE quest_id=?",
        -1, &copy_obj, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Duplicate objective copy error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(copy_obj, 1, new_quest_id);
    sqlite3_bind_int(copy_obj, 2, quest_id);
    int rc_obj = sqlite3_step(copy_obj);
    sqlite3_finalize(copy_obj);
    if (rc_obj != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Duplicate objective copy error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_stmt* copy_reward = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO quest_reward_defs (quest_id, xp_reward, gold_reward, item_id, item_qty) "
        "SELECT ?, xp_reward, gold_reward, item_id, item_qty "
        "FROM quest_reward_defs WHERE quest_id=?",
        -1, &copy_reward, nullptr) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Duplicate reward copy error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(copy_reward, 1, new_quest_id);
    sqlite3_bind_int(copy_reward, 2, quest_id);
    int rc_reward = sqlite3_step(copy_reward);
    sqlite3_finalize(copy_reward);
    if (rc_reward != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        SetStatus("Duplicate reward copy error: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    select_quest_after_fetch_ = new_quest_id;
    need_fetch_quests_ = true;
    need_fetch_children_ = true;
    SetStatus("Duplicated quest '%s' -> '%s'.", source.code.c_str(), duplicate.code.c_str());
    return true;
}

bool QuestsTab::SaveObjective(sqlite3* db, QuestObjectiveDefinition& objective) {
    objective.description = TrimCopy(objective.description);
    objective.target_npc_name = TrimCopy(objective.target_npc_name);
    objective.target_area_name = TrimCopy(objective.target_area_name);
    if (objective.objective_order < 1) objective.objective_order = 1;
    if (objective.target_count < 1) objective.target_count = 1;
    if (objective.objective_type < 1) objective.objective_type = 1;
    if (objective.objective_type > kObjectiveTypeCount) objective.objective_type = kObjectiveTypeCount;
    if (objective.target_item_id < 0) objective.target_item_id = 0;

    if (objective.objective_type != 1 &&
        objective.objective_type != 3 &&
        objective.objective_type != 5) {
        objective.target_npc_name.clear();
    }
    if (objective.objective_type != 2) {
        objective.target_item_id = 0;
    }
    if (objective.objective_type != 4) {
        objective.target_area_name.clear();
    }

    std::string validation_error;
    if (!ValidateObjective(objective, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (objective.id == 0) {
        const char* sql =
            "INSERT INTO quest_objective_defs "
            "(quest_id, objective_order, objective_type, description, target_npc_name, target_item_id, target_area_name, target_count) "
            "VALUES (?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Objective create error: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(stmt, 1, objective.quest_id);
        sqlite3_bind_int(stmt, 2, objective.objective_order);
        sqlite3_bind_int(stmt, 3, objective.objective_type);
        sqlite3_bind_text(stmt, 4, objective.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, objective.target_npc_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, objective.target_item_id);
        sqlite3_bind_text(stmt, 7, objective.target_area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, objective.target_count);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            objective.id = static_cast<int>(sqlite3_last_insert_rowid(db));
        }
    } else {
        const char* sql =
            "UPDATE quest_objective_defs SET "
            "objective_order=?, objective_type=?, description=?, target_npc_name=?, target_item_id=?, target_area_name=?, target_count=? "
            "WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Objective save error: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(stmt, 1, objective.objective_order);
        sqlite3_bind_int(stmt, 2, objective.objective_type);
        sqlite3_bind_text(stmt, 3, objective.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, objective.target_npc_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, objective.target_item_id);
        sqlite3_bind_text(stmt, 6, objective.target_area_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, objective.target_count);
        sqlite3_bind_int(stmt, 8, objective.id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Objective save error: %s", sqlite3_errmsg(db));
        return false;
    }

    need_fetch_children_ = true;
    dirty_objective_ = false;
    show_new_objective_ = false;
    SetStatus("Saved objective %d.", objective.id);
    return true;
}

bool QuestsTab::DeleteObjective(sqlite3* db, int objective_id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM quest_objective_defs WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Objective delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, objective_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Objective delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    need_fetch_children_ = true;
    selected_objective_ = -1;
    dirty_objective_ = false;
    SetStatus("Deleted objective %d.", objective_id);
    return true;
}

bool QuestsTab::SaveReward(sqlite3* db, QuestRewardDefinition& reward) {
    if (reward.xp_reward < 0) reward.xp_reward = 0;
    if (reward.gold_reward < 0) reward.gold_reward = 0;
    if (reward.item_id < 0) reward.item_id = 0;
    if (reward.item_qty < 0) reward.item_qty = 0;
    if (reward.item_id == 0) reward.item_qty = 0;

    std::string validation_error;
    if (!ValidateReward(reward, &validation_error)) {
        SetStatus("%s", validation_error.c_str());
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (reward.id == 0) {
        const char* sql =
            "INSERT INTO quest_reward_defs (quest_id, xp_reward, gold_reward, item_id, item_qty) "
            "VALUES (?,?,?,?,?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Reward create error: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(stmt, 1, reward.quest_id);
        sqlite3_bind_int(stmt, 2, reward.xp_reward);
        sqlite3_bind_int(stmt, 3, reward.gold_reward);
        sqlite3_bind_int(stmt, 4, reward.item_id);
        sqlite3_bind_int(stmt, 5, reward.item_qty);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            reward.id = static_cast<int>(sqlite3_last_insert_rowid(db));
        }
    } else {
        const char* sql =
            "UPDATE quest_reward_defs SET xp_reward=?, gold_reward=?, item_id=?, item_qty=? WHERE id=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            SetStatus("Reward save error: %s", sqlite3_errmsg(db));
            return false;
        }
        sqlite3_bind_int(stmt, 1, reward.xp_reward);
        sqlite3_bind_int(stmt, 2, reward.gold_reward);
        sqlite3_bind_int(stmt, 3, reward.item_id);
        sqlite3_bind_int(stmt, 4, reward.item_qty);
        sqlite3_bind_int(stmt, 5, reward.id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Reward save error: %s", sqlite3_errmsg(db));
        return false;
    }

    need_fetch_children_ = true;
    dirty_reward_ = false;
    show_new_reward_ = false;
    SetStatus("Saved reward %d.", reward.id);
    return true;
}

bool QuestsTab::DeleteReward(sqlite3* db, int reward_id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM quest_reward_defs WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
        SetStatus("Reward delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, reward_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        SetStatus("Reward delete error: %s", sqlite3_errmsg(db));
        return false;
    }

    need_fetch_children_ = true;
    selected_reward_ = -1;
    dirty_reward_ = false;
    SetStatus("Deleted reward %d.", reward_id);
    return true;
}

void QuestsTab::Draw(sqlite3* db) {
    EnsureTables(db);
    if (need_fetch_items_) {
        FetchItems(db);
        need_fetch_items_ = false;
    }
    if (need_fetch_quests_) {
        FetchQuests(db);
        need_fetch_quests_ = false;
    }
    if (need_fetch_children_ && selected_quest_ >= 0 && selected_quest_ < static_cast<int>(quests_.size())) {
        const int quest_id = quests_[selected_quest_].id;
        FetchObjectives(db, quest_id);
        FetchRewards(db, quest_id);
        need_fetch_children_ = false;
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_items_ = true;
        need_fetch_quests_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("New Quest")) {
        new_quest_ = {};
        new_quest_.min_level = 1;
        new_quest_.is_active = true;
        show_new_quest_ = true;
        show_new_objective_ = false;
        show_new_reward_ = false;
        selected_quest_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    const float list_width = 320.0f;
    ImGui::BeginChild("##quest_list", {list_width, 0.0f}, true);
    for (int i = 0; i < static_cast<int>(quests_.size()); ++i) {
        const auto& quest = quests_[i];
        char label[320];
        std::snprintf(label, sizeof(label),
            "[%s] %s - %s (Lv %d)##quest_%d",
            quest.is_active ? "ON" : "OFF",
            quest.code.c_str(),
            quest.title.c_str(),
            quest.min_level,
            quest.id);
        if (ImGui::Selectable(label, selected_quest_ == i)) {
            selected_quest_ = i;
            editing_quest_ = quest;
            dirty_quest_ = false;
            show_new_quest_ = false;
            selected_objective_ = -1;
            selected_reward_ = -1;
            show_new_objective_ = false;
            show_new_reward_ = false;
            need_fetch_children_ = true;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##quest_editor", {0.0f, 0.0f}, true);

    auto draw_quest_fields = [&](QuestDefinition& quest) -> bool {
        bool changed = false;
        char code_buf[128] = {};
        std::strncpy(code_buf, quest.code.c_str(), sizeof(code_buf) - 1);
        if (ImGui::InputText("Code", code_buf, sizeof(code_buf))) {
            quest.code = code_buf;
            NormalizeQuestCodeInPlace(&quest.code);
            changed = true;
        }
        char title_buf[256] = {};
        std::strncpy(title_buf, quest.title.c_str(), sizeof(title_buf) - 1);
        if (ImGui::InputText("Title", title_buf, sizeof(title_buf))) {
            quest.title = title_buf;
            changed = true;
        }
        ImGui::TextUnformatted("Description");
        char desc_buf[2048] = {};
        std::strncpy(desc_buf, quest.description.c_str(), sizeof(desc_buf) - 1);
        if (ImGui::InputTextMultiline("##quest_description", desc_buf, sizeof(desc_buf), { -1.0f, 90.0f })) {
            quest.description = desc_buf;
            changed = true;
        }

        if (ImGui::InputInt("Minimum Level", &quest.min_level)) {
            if (quest.min_level < 1) quest.min_level = 1;
            changed = true;
        }
        if (ImGui::Checkbox("Repeatable", &quest.repeatable)) changed = true;
        if (ImGui::Checkbox("Auto Accept", &quest.auto_accept)) changed = true;
        if (ImGui::Checkbox("Active", &quest.is_active)) changed = true;

        std::string current_prereq = "None (0)";
        if (quest.prerequisite_quest_id > 0) {
            for (const auto& row : quests_) {
                if (row.id == quest.prerequisite_quest_id) {
                    char prereq[256];
                    std::snprintf(prereq, sizeof(prereq), "[%d] %s - %s", row.id, row.code.c_str(), row.title.c_str());
                    current_prereq = prereq;
                    break;
                }
            }
        }
        if (ImGui::BeginCombo("Prerequisite Quest", current_prereq.c_str())) {
            bool none_selected = (quest.prerequisite_quest_id == 0);
            if (ImGui::Selectable("None (0)", none_selected)) {
                quest.prerequisite_quest_id = 0;
                changed = true;
            }
            if (none_selected) ImGui::SetItemDefaultFocus();

            for (const auto& row : quests_) {
                if (quest.id > 0 && row.id == quest.id) continue;
                char label[256];
                std::snprintf(label, sizeof(label), "[%d] %s - %s", row.id, row.code.c_str(), row.title.c_str());
                bool selected = (quest.prerequisite_quest_id == row.id);
                if (ImGui::Selectable(label, selected)) {
                    quest.prerequisite_quest_id = row.id;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const std::string code_trimmed = TrimCopy(quest.code);
        if (!code_trimmed.empty()) {
            bool code_ok = true;
            for (char ch : code_trimmed) {
                if (!IsQuestCodeChar(ch)) {
                    code_ok = false;
                    break;
                }
            }
            if (!code_ok) {
                ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f},
                    "Code should use lowercase letters, numbers, and underscore only.");
            }
        } else {
            ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "Code is required.");
        }
        if (TrimCopy(quest.title).empty()) {
            ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "Title is required.");
        }

        return changed;
    };

    auto draw_objective_fields = [&](QuestObjectiveDefinition& objective, const char* suffix) -> bool {
        bool changed = false;
        char desc_id[64];
        char order_id[64];
        char type_id[64];
        char count_id[64];
        char npc_id[64];
        char item_combo_id[64];
        char item_id_id[64];
        char area_id[64];
        std::snprintf(desc_id, sizeof(desc_id), "Description##%s", suffix);
        std::snprintf(order_id, sizeof(order_id), "Order##%s", suffix);
        std::snprintf(type_id, sizeof(type_id), "Type##%s", suffix);
        std::snprintf(count_id, sizeof(count_id), "Target Count##%s", suffix);
        std::snprintf(npc_id, sizeof(npc_id), "Target NPC Name##%s", suffix);
        std::snprintf(item_combo_id, sizeof(item_combo_id), "Target Item##%s", suffix);
        std::snprintf(item_id_id, sizeof(item_id_id), "Target Item ID##%s", suffix);
        std::snprintf(area_id, sizeof(area_id), "Target Area##%s", suffix);

        if (ImGui::InputInt(order_id, &objective.objective_order)) {
            if (objective.objective_order < 1) objective.objective_order = 1;
            changed = true;
        }

        int type_index = objective.objective_type - 1;
        if (type_index < 0) type_index = 0;
        if (type_index >= kObjectiveTypeCount) type_index = kObjectiveTypeCount - 1;
        if (ImGui::Combo(type_id, &type_index, kObjectiveTypeNames, kObjectiveTypeCount)) {
            objective.objective_type = type_index + 1;
            changed = true;
        }

        char objective_desc_buf[1024] = {};
        std::strncpy(objective_desc_buf, objective.description.c_str(), sizeof(objective_desc_buf) - 1);
        if (ImGui::InputText(desc_id, objective_desc_buf, sizeof(objective_desc_buf))) {
            objective.description = objective_desc_buf;
            changed = true;
        }
        if (ImGui::InputInt(count_id, &objective.target_count)) {
            if (objective.target_count < 1) objective.target_count = 1;
            changed = true;
        }

        if (objective.objective_type == 1 || objective.objective_type == 3 || objective.objective_type == 5) {
            char npc_buf[256] = {};
            std::strncpy(npc_buf, objective.target_npc_name.c_str(), sizeof(npc_buf) - 1);
            if (ImGui::InputText(npc_id, npc_buf, sizeof(npc_buf))) {
                objective.target_npc_name = npc_buf;
                changed = true;
            }
        }
        if (objective.objective_type == 2) {
            std::string current_item = "None (0)";
            if (objective.target_item_id > 0) {
                const int idx = IndexByItemID(items_, objective.target_item_id);
                if (idx >= 0) {
                    char item_label[256];
                    std::snprintf(item_label, sizeof(item_label), "[%d] %s", items_[idx].id, items_[idx].name.c_str());
                    current_item = item_label;
                } else {
                    char item_label[64];
                    std::snprintf(item_label, sizeof(item_label), "Item ID %d", objective.target_item_id);
                    current_item = item_label;
                }
            }
            if (ImGui::BeginCombo(item_combo_id, current_item.c_str())) {
                bool none_selected = (objective.target_item_id == 0);
                if (ImGui::Selectable("None (0)", none_selected)) {
                    objective.target_item_id = 0;
                    changed = true;
                }
                if (none_selected) ImGui::SetItemDefaultFocus();
                for (const auto& item : items_) {
                    char item_label[256];
                    std::snprintf(item_label, sizeof(item_label), "[%d] %s", item.id, item.name.c_str());
                    bool selected = (objective.target_item_id == item.id);
                    if (ImGui::Selectable(item_label, selected)) {
                        objective.target_item_id = item.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::InputInt(item_id_id, &objective.target_item_id)) {
                if (objective.target_item_id < 0) objective.target_item_id = 0;
                changed = true;
            }
        }
        if (objective.objective_type == 4) {
            char area_buf[256] = {};
            std::strncpy(area_buf, objective.target_area_name.c_str(), sizeof(area_buf) - 1);
            if (ImGui::InputText(area_id, area_buf, sizeof(area_buf))) {
                objective.target_area_name = area_buf;
                changed = true;
            }
        }
        return changed;
    };

    auto draw_reward_fields = [&](QuestRewardDefinition& reward, const char* suffix) -> bool {
        bool changed = false;
        char xp_id[64];
        char gold_id[64];
        char item_combo_id[64];
        char item_id_id[64];
        char qty_id[64];
        std::snprintf(xp_id, sizeof(xp_id), "XP Reward##%s", suffix);
        std::snprintf(gold_id, sizeof(gold_id), "Gold Reward##%s", suffix);
        std::snprintf(item_combo_id, sizeof(item_combo_id), "Item Reward##%s", suffix);
        std::snprintf(item_id_id, sizeof(item_id_id), "Item ID##%s", suffix);
        std::snprintf(qty_id, sizeof(qty_id), "Item Qty##%s", suffix);

        if (ImGui::InputInt(xp_id, &reward.xp_reward)) {
            if (reward.xp_reward < 0) reward.xp_reward = 0;
            changed = true;
        }
        if (ImGui::InputInt(gold_id, &reward.gold_reward)) {
            if (reward.gold_reward < 0) reward.gold_reward = 0;
            changed = true;
        }

        std::string current_item = "None (0)";
        if (reward.item_id > 0) {
            const int idx = IndexByItemID(items_, reward.item_id);
            if (idx >= 0) {
                char item_label[256];
                std::snprintf(item_label, sizeof(item_label), "[%d] %s", items_[idx].id, items_[idx].name.c_str());
                current_item = item_label;
            } else {
                char item_label[64];
                std::snprintf(item_label, sizeof(item_label), "Item ID %d", reward.item_id);
                current_item = item_label;
            }
        }

        if (ImGui::BeginCombo(item_combo_id, current_item.c_str())) {
            bool none_selected = (reward.item_id == 0);
            if (ImGui::Selectable("None (0)", none_selected)) {
                reward.item_id = 0;
                reward.item_qty = 0;
                changed = true;
            }
            if (none_selected) ImGui::SetItemDefaultFocus();
            for (const auto& item : items_) {
                char item_label[256];
                std::snprintf(item_label, sizeof(item_label), "[%d] %s", item.id, item.name.c_str());
                bool selected = (reward.item_id == item.id);
                if (ImGui::Selectable(item_label, selected)) {
                    reward.item_id = item.id;
                    if (reward.item_qty < 1) reward.item_qty = 1;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::InputInt(item_id_id, &reward.item_id)) {
            if (reward.item_id < 0) reward.item_id = 0;
            if (reward.item_id == 0) reward.item_qty = 0;
            changed = true;
        }
        if (reward.item_id > 0) {
            if (ImGui::InputInt(qty_id, &reward.item_qty)) {
                if (reward.item_qty < 1) reward.item_qty = 1;
                changed = true;
            }
        } else {
            reward.item_qty = 0;
            ImGui::TextDisabled("Item quantity disabled when no item is selected.");
        }
        return changed;
    };

    if (show_new_quest_) {
        ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Quest");
        ImGui::Separator();
        draw_quest_fields(new_quest_);
        ImGui::Spacing();
        if (ImGui::Button("Create Quest")) {
            SaveQuest(db, new_quest_, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##new_quest")) {
            show_new_quest_ = false;
        }
    } else if (selected_quest_ >= 0 && selected_quest_ < static_cast<int>(quests_.size())) {
        ImGui::Text("Quest [id=%d] - %s", editing_quest_.id, editing_quest_.code.c_str());
        ImGui::Separator();
        if (draw_quest_fields(editing_quest_)) {
            dirty_quest_ = true;
        }
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirty_quest_);
        if (ImGui::Button("Save Quest")) {
            SaveQuest(db, editing_quest_, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert Quest")) {
            editing_quest_ = quests_[selected_quest_];
            dirty_quest_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate Quest")) {
            DuplicateQuest(db, editing_quest_.id);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
        if (ImGui::Button("Delete Quest")) {
            DeleteQuest(db, editing_quest_.id);
        }
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Objectives");
        ImGui::SameLine();
        if (ImGui::SmallButton("New Objective")) {
            new_objective_ = {};
            new_objective_.quest_id = editing_quest_.id;
            new_objective_.objective_order = static_cast<int>(objectives_.size()) + 1;
            new_objective_.objective_type = 1;
            new_objective_.target_count = 1;
            show_new_objective_ = true;
            selected_objective_ = -1;
        }
        ImGui::Separator();

        float obj_list_height = std::min(150.0f, 28.0f * static_cast<float>(objectives_.size() + 1));
        ImGui::BeginChild("##objective_list", {0.0f, obj_list_height}, true);
        for (int i = 0; i < static_cast<int>(objectives_.size()); ++i) {
            const auto& objective = objectives_[i];
            char label[320];
            std::snprintf(label, sizeof(label),
                "#%d [%s] %s##obj_%d",
                objective.objective_order,
                ObjectiveTypeLabel(objective.objective_type).c_str(),
                objective.description.c_str(),
                objective.id);
            if (ImGui::Selectable(label, selected_objective_ == i)) {
                selected_objective_ = i;
                editing_objective_ = objective;
                dirty_objective_ = false;
                show_new_objective_ = false;
            }
        }
        if (objectives_.empty()) {
            ImGui::TextDisabled("No objectives for this quest.");
        }
        ImGui::EndChild();

        if (show_new_objective_) {
            ImGui::Spacing();
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Objective");
            ImGui::Separator();
            draw_objective_fields(new_objective_, "new_objective");
            ImGui::Spacing();
            if (ImGui::Button("Create Objective")) {
                SaveObjective(db, new_objective_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##new_objective")) {
                show_new_objective_ = false;
            }
        } else if (selected_objective_ >= 0 && selected_objective_ < static_cast<int>(objectives_.size())) {
            ImGui::Spacing();
            ImGui::Text("Objective [id=%d]", editing_objective_.id);
            ImGui::Separator();
            if (draw_objective_fields(editing_objective_, "edit_objective")) {
                dirty_objective_ = true;
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_objective_);
            if (ImGui::Button("Save Objective")) {
                SaveObjective(db, editing_objective_);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Objective")) {
                editing_objective_ = objectives_[selected_objective_];
                dirty_objective_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
            if (ImGui::Button("Delete Objective")) {
                DeleteObjective(db, editing_objective_.id);
            }
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Rewards");
        ImGui::SameLine();
        if (ImGui::SmallButton("New Reward")) {
            new_reward_ = {};
            new_reward_.quest_id = editing_quest_.id;
            show_new_reward_ = true;
            selected_reward_ = -1;
        }
        ImGui::Separator();

        float reward_list_height = std::min(120.0f, 28.0f * static_cast<float>(rewards_.size() + 1));
        ImGui::BeginChild("##reward_list", {0.0f, reward_list_height}, true);
        for (int i = 0; i < static_cast<int>(rewards_.size()); ++i) {
            const auto& reward = rewards_[i];
            std::string item_text = "None";
            if (reward.item_id > 0) {
                const int idx = IndexByItemID(items_, reward.item_id);
                if (idx >= 0) item_text = items_[idx].name;
                else item_text = "Item ID " + std::to_string(reward.item_id);
            }
            char label[320];
            std::snprintf(label, sizeof(label),
                "XP %d | Gold %d | Item %s x%d##reward_%d",
                reward.xp_reward,
                reward.gold_reward,
                item_text.c_str(),
                reward.item_qty,
                reward.id);
            if (ImGui::Selectable(label, selected_reward_ == i)) {
                selected_reward_ = i;
                editing_reward_ = reward;
                dirty_reward_ = false;
                show_new_reward_ = false;
            }
        }
        if (rewards_.empty()) {
            ImGui::TextDisabled("No rewards for this quest.");
        }
        ImGui::EndChild();

        if (show_new_reward_) {
            ImGui::Spacing();
            ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "New Reward");
            ImGui::Separator();
            draw_reward_fields(new_reward_, "new_reward");
            ImGui::Spacing();
            if (ImGui::Button("Create Reward")) {
                SaveReward(db, new_reward_);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##new_reward")) {
                show_new_reward_ = false;
            }
        } else if (selected_reward_ >= 0 && selected_reward_ < static_cast<int>(rewards_.size())) {
            ImGui::Spacing();
            ImGui::Text("Reward [id=%d]", editing_reward_.id);
            ImGui::Separator();
            if (draw_reward_fields(editing_reward_, "edit_reward")) {
                dirty_reward_ = true;
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_reward_);
            if (ImGui::Button("Save Reward")) {
                SaveReward(db, editing_reward_);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert Reward")) {
                editing_reward_ = rewards_[selected_reward_];
                dirty_reward_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.0f});
            if (ImGui::Button("Delete Reward")) {
                DeleteReward(db, editing_reward_.id);
            }
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("Select a quest, or click \"New Quest\".");
    }

    ImGui::EndChild();
}

} // namespace gue
