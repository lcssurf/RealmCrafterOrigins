#pragma once

#include <string>
#include <vector>

#include "sqlite3.h"

namespace gue {

struct EquipmentSlotConfigRow {
    int  slot_id = 0;
    bool gives_kit = false;
    int  hotbar_slots_granted = 0;
    bool enabled = true;
};

class EquipmentSlotsTab {
public:
    void Draw(sqlite3* db);

private:
    void Fetch(sqlite3* db);
    bool UpsertSlot(sqlite3* db, const EquipmentSlotConfigRow& row);
    void SetStatus(const char* fmt, ...);

    // 10 fixed slots (0..9) are always present in memory.
    // Missing rows in DB use defaults: gives_kit=false, max=0, enabled=true.
    EquipmentSlotConfigRow rows_[10];

    bool need_fetch_ = true;
    char status_msg_[256] = {};
};

} // namespace gue
