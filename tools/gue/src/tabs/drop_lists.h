#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>
#include <utility>

namespace gue {

struct DropEntryRow {
    int   id = 0;
    int   item_id = 0;
    float chance = 0.5f;
    int   min_qty = 1;
    int   max_qty = 1;
};

struct DropListRow {
    int              id = 0;
    std::string      name;
    bool             enabled = true;
    std::vector<DropEntryRow> entries;
};

class DropListsTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);
    void FetchAll(sqlite3* db);
    void LoadItems(sqlite3* db);
    bool SaveDropList(sqlite3* db, DropListRow& row);
    bool DeleteDropList(sqlite3* db, int id);
    bool DrawFields(DropListRow& row);

    std::vector<DropListRow>                 lists_;
    std::vector<std::pair<int, std::string>> items_;
    int  selected_ = -1;
    bool need_fetch_ = true;
    bool dirty_ = false;
    bool show_new_ = false;
    DropListRow new_row_;
    DropListRow editing_row_;
    char status_msg_[256] = {};
};

} // namespace gue
