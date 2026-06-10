#include "drop_lists.h"

#include <imgui.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gue {

namespace {

std::string TrimCopy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

static bool ComboId(const char* label, int& currentId,
                   const std::vector<std::pair<int, std::string>>& items,
                   const char* emptyLabel = "(none)") {
    int curIdx = 0; // 0 = empty
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].first == currentId) { curIdx = (int)(i + 1); break; }
    }

    bool changed = false;
    const std::string& curLabel =
        curIdx == 0 ? std::string(emptyLabel) : items[curIdx - 1].second;

    if (ImGui::BeginCombo(label, curLabel.c_str())) {
        if (ImGui::Selectable(emptyLabel, curIdx == 0)) {
            currentId = 0;
            changed = true;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            bool sel = (curIdx == (int)(i + 1));
            if (ImGui::Selectable(items[i].second.c_str(), sel)) {
                currentId = items[i].first;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static std::string colText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

} // namespace

void DropListsTab::EnsureTables(sqlite3* db) {
    const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS loot_tables ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  enabled INTEGER NOT NULL DEFAULT 1"
        ")",

        "CREATE TABLE IF NOT EXISTS loot_entries ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  loot_table_id INTEGER NOT NULL,"
        "  item_id INTEGER NOT NULL,"
        "  chance REAL NOT NULL DEFAULT 0.0,"
        "  min_qty INTEGER NOT NULL DEFAULT 1,"
        "  max_qty INTEGER NOT NULL DEFAULT 1,"
        "  FOREIGN KEY(loot_table_id) REFERENCES loot_tables(id),"
        "  FOREIGN KEY(item_id) REFERENCES item_templates(id)"
        ")",
    };

    for (const char* s : ddl) sqlite3_exec(db, s, nullptr, nullptr, nullptr);
}

void DropListsTab::LoadItems(sqlite3* db) {
    items_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name FROM item_templates ORDER BY id",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        std::string name = colText(stmt, 1);
        items_.push_back({id, std::move(name)});
    }
    sqlite3_finalize(stmt);
}

void DropListsTab::FetchAll(sqlite3* db) {
    lists_.clear();
    selected_ = -1;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, enabled FROM loot_tables ORDER BY id",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Fetch error: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DropListRow row;
        row.id      = sqlite3_column_int(stmt, 0);
        row.name    = colText(stmt, 1);
        row.enabled = sqlite3_column_int(stmt, 2) != 0;
        lists_.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    sqlite3_stmt* eStmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, loot_table_id, item_id, chance, min_qty, max_qty"
        " FROM loot_entries WHERE loot_table_id=? ORDER BY id",
        -1, &eStmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Fetch error (entries): %s", sqlite3_errmsg(db));
        return;
    }

    for (auto& row : lists_) {
        sqlite3_bind_int(eStmt, 1, row.id);
        while (sqlite3_step(eStmt) == SQLITE_ROW) {
            DropEntryRow e;
            e.id       = sqlite3_column_int(eStmt, 0);
            e.item_id  = sqlite3_column_int(eStmt, 2);
            e.chance   = static_cast<float>(sqlite3_column_double(eStmt, 3));
            e.min_qty  = sqlite3_column_int(eStmt, 4);
            e.max_qty  = sqlite3_column_int(eStmt, 5);
            row.entries.push_back(e);
        }
        sqlite3_reset(eStmt);
        sqlite3_clear_bindings(eStmt);
    }
    sqlite3_finalize(eStmt);

    need_fetch_ = false;
    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Loaded %d drop list(s).", (int)lists_.size());
}

bool DropListsTab::DrawFields(DropListRow& row) {
    bool changed = false;

    char name_buf[128] = {};
    std::strncpy(name_buf, row.name.c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        row.name = name_buf;
        changed = true;
    }
    if (ImGui::Checkbox("Enabled", &row.enabled)) changed = true;

    ImGui::SeparatorText("Drops");
    for (size_t i = 0; i < row.entries.size();) {
        auto& entry = row.entries[i];
        ImGui::PushID((int)i);

        bool removed = false;
        if (ComboId("Item", entry.item_id, items_, "(select item)")) {
            changed = true;
        }

        if (ImGui::SliderFloat("Chance", &entry.chance, 0.f, 1.f, "%.2f")) {
            changed = true;
        }

        if (ImGui::InputInt("Min", &entry.min_qty)) {
            if (entry.min_qty < 1) entry.min_qty = 1;
            if (entry.max_qty < entry.min_qty) entry.max_qty = entry.min_qty;
            changed = true;
        }
        if (ImGui::InputInt("Max", &entry.max_qty)) {
            if (entry.max_qty < 1) entry.max_qty = 1;
            if (entry.max_qty < entry.min_qty) entry.max_qty = entry.min_qty;
            changed = true;
        }

        if (ImGui::Button("Remove")) {
            row.entries.erase(row.entries.begin() + (int)i);
            changed = true;
            removed = true;
        }
        ImGui::PopID();
        if (removed) {
            continue;
        }
        ++i;
    }

    if (ImGui::Button("+ Add Drop")) {
        row.entries.push_back({});
        changed = true;
    }

    return changed;
}

bool DropListsTab::SaveDropList(sqlite3* db, DropListRow& row) {
    row.name = TrimCopy(row.name);
    if (row.name.empty()) {
        std::snprintf(status_msg_, sizeof(status_msg_), "Name cannot be empty.");
        return false;
    }

    if (sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: cannot start transaction: %s", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = SQLITE_ERROR;
    if (row.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO loot_tables (name, enabled) VALUES (?, ?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE loot_tables SET name=?, enabled=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    sqlite3_bind_text(stmt, 1, row.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, row.enabled ? 1 : 0);
    if (row.id != 0) sqlite3_bind_int(stmt, 3, row.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_finalize(stmt);

    if (row.id == 0) {
        row.id = static_cast<int>(sqlite3_last_insert_rowid(db));
    }

    if (sqlite3_prepare_v2(db, "DELETE FROM loot_entries WHERE loot_table_id=?",
                          -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int(stmt, 1, row.id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(db,
        "INSERT INTO loot_entries (loot_table_id, item_id, chance, min_qty, max_qty)"
        " VALUES (?, ?, ?, ?, ?)",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    for (auto& e : row.entries) {
        if (e.item_id <= 0) continue;
        if (e.min_qty < 1) e.min_qty = 1;
        if (e.max_qty < e.min_qty) e.max_qty = e.min_qty;
        if (e.chance < 0.f) e.chance = 0.f;
        if (e.chance > 1.f) e.chance = 1.f;

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, row.id);
        sqlite3_bind_int(stmt, 2, e.item_id);
        sqlite3_bind_double(stmt, 3, e.chance);
        sqlite3_bind_int(stmt, 4, e.min_qty);
        sqlite3_bind_int(stmt, 5, e.max_qty);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Save error: %s", sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: cannot commit: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }

    need_fetch_ = true;
    dirty_ = false;
    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Saved drop list '%s' (id=%d).", row.name.c_str(), row.id);
    return true;
}

bool DropListsTab::DeleteDropList(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "UPDATE loot_tables SET enabled=0 WHERE id=?",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Delete error: %s", sqlite3_errmsg(db));
        return false;
    }
    need_fetch_ = true;
    selected_ = -1;
    std::snprintf(status_msg_, sizeof(status_msg_), "Disabled drop list %d.", id);
    return true;
}

void DropListsTab::Draw(sqlite3* db) {
    if (!db) return;
    if (need_fetch_) {
        EnsureTables(db);
        FetchAll(db);
        LoadItems(db);
        need_fetch_ = false;
    }

    if (ImGui::Button("Refresh")) {
        need_fetch_ = true;
        FetchAll(db);
        LoadItems(db);
        need_fetch_ = false;
    }

    ImGui::SameLine();
    if (ImGui::Button("New Drop List")) {
        show_new_ = true;
        selected_ = -1;
        dirty_ = false;
        new_row_ = {};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    float listWidth = 360.f;
    ImGui::BeginChild("##drop_list_list", {listWidth, 0}, true);
    for (int i = 0; i < (int)lists_.size(); ++i) {
        auto& row = lists_[i];
        char label[256];
        std::snprintf(label, sizeof(label), "%d: %s%s",
                      row.id, row.name.c_str(), row.enabled ? "" : " [disabled]");
        if (ImGui::Selectable(label, selected_ == i)) {
            selected_ = i;
            editing_row_ = row;
            dirty_ = false;
            show_new_ = false;
        }
    }
    if (lists_.empty()) {
        ImGui::TextDisabled("No drop lists.");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##drop_list_editor", {0, 0}, true);

    if (show_new_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Drop List");
        ImGui::Separator();
        if (DrawFields(new_row_)) dirty_ = true;

        ImGui::Spacing();
        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Create")) {
            if (SaveDropList(db, new_row_)) {
                show_new_ = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) show_new_ = false;
    } else if (selected_ >= 0 && selected_ < (int)lists_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editing_row_.id, editing_row_.name.c_str());
        ImGui::Separator();
        if (DrawFields(editing_row_)) dirty_ = true;

        ImGui::Spacing();
        ImGui::BeginDisabled(!dirty_);
        if (ImGui::Button("Save")) {
            SaveDropList(db, editing_row_);
            dirty_ = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            editing_row_ = lists_[selected_];
            dirty_ = false;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Disable")) DeleteDropList(db, editing_row_.id);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Select a drop list, or click \"New Drop List\".");
    }

    ImGui::EndChild();
}

} // namespace gue
