#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

namespace gue {

// StatusEffectTemplateRow mirrors one row in status_effect_templates.
// Fase 1 (server migrateV57) had only CC-relevant fields. Fase 2
// (migrateV58) activated stat_mods_json/max_stacks for kind="buff"/
// "debuff" rows. Fase 3 (migrateV59) activates tick_interval_ms/
// tick_damage_min/tick_damage_max/is_heal for DoT (kind="debuff",
// is_heal=false) and HoT (kind="buff", is_heal=true) — cc_type stays
// ignored for buff/debuff rows (kept at its default so a row flipped from
// cc->buff/debuff doesn't carry a stale meaningless value). See
// docs/TECH_DEBT.md, Buffs/Debuffs/CC investigation, for the full design.
struct StatusEffectTemplateRow {
    int         id = 0;
    std::string name;
    std::string kind = "cc"; // "cc" | "buff" | "debuff"
    std::string cc_type = "stun"; // "stun" | "root" | "silence" | "slow" — only meaningful when kind=="cc"
    int         duration_ms = 2000;
    std::string stack_rule = "refresh"; // "refresh" | "stack_up_to_max" | "ignore_if_active"
    std::string icon_path;
    bool        enabled = true;
    // stat_mods_json: JSON array of {"stat","flat","pct"} objects (world.StatMod
    // on the server side) — only meaningful when kind=="buff"/"debuff".
    // Free-text edited here on purpose (see StatusEffectsTab's doc comment
    // below for why a JSON textbox instead of a sub-list widget).
    std::string stat_mods_json = "[]";
    // max_stacks: only meaningful when stack_rule=="stack_up_to_max".
    int         max_stacks = 1;
    // tick_interval_ms<=0 (default) means no tick — a plain stat buff/
    // debuff, or CC. >0 makes this a DoT ("debuff" + is_heal=false) or HoT
    // ("buff" + is_heal=true); tick_damage_min/max are rolled per tick.
    int         tick_interval_ms = 0;
    int         tick_damage_min = 0;
    int         tick_damage_max = 0;
    bool        is_heal = false;
};

// StatusEffectsTab — CRUD editor for status_effect_templates. Kept as the
// same inline-editable-table pattern Fase 1 chose (Save/Del per row, "+
// Add" button, same shape as media.cpp's anim-binding table) rather than
// CombatAbilitiesTab's richer new/editing-copy + dirty-flag pattern —
// stat_mods_json is edited as a raw JSON text field (no sub-list widget)
// specifically to avoid that jump: the row shape stays flat/single-line,
// so the simpler pattern still fits. If a future phase needs per-mod
// validation/autocomplete, that's the trigger to graduate to the
// CombatAbilitiesTab-style editor, not before.
class StatusEffectsTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);
    void FetchAll(sqlite3* db);

    bool SaveRow(sqlite3* db, StatusEffectTemplateRow& row);
    bool DeleteRow(sqlite3* db, int row_id);

    void SetStatus(const char* fmt, ...);

    bool tables_ensured_ = false;
    bool need_fetch_ = true;

    std::vector<StatusEffectTemplateRow> rows_;
    char status_msg_[256] = {};
};

} // namespace gue
