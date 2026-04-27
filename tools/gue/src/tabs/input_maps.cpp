#include "input_maps.h"

#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <algorithm>

namespace gue {

// ---------------------------------------------------------------------------
// Table creation
// ---------------------------------------------------------------------------

void InputMapsTab::EnsureTables(sqlite3* db) {
    const char* sqls[] = {
        "CREATE TABLE IF NOT EXISTS media_input_presets ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name        TEXT    NOT NULL UNIQUE,"
        "  description TEXT    NOT NULL DEFAULT '',"
        "  is_default  INTEGER NOT NULL DEFAULT 0,"
        "  created_at  INTEGER NOT NULL DEFAULT 0"
        ")",
        "CREATE TABLE IF NOT EXISTS media_input_maps ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  preset_id    INTEGER NOT NULL DEFAULT 1,"
        "  context      TEXT    NOT NULL DEFAULT 'gameplay',"
        "  key          TEXT    NOT NULL DEFAULT '',"
        "  modifier     TEXT    NOT NULL DEFAULT '',"
        "  trigger_type TEXT    NOT NULL DEFAULT 'press',"
        "  action       TEXT    NOT NULL DEFAULT '',"
        "  axis_value   REAL    NOT NULL DEFAULT 1.0,"
        "  enabled      INTEGER NOT NULL DEFAULT 1,"
        "  remappable   INTEGER NOT NULL DEFAULT 1,"
        "  UNIQUE (preset_id, context, key, modifier, trigger_type)"
        ")",
    };
    for (auto* sql : sqls)
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);

    // Seed the default preset if it doesn't exist yet.
    sqlite3_exec(db,
        "INSERT OR IGNORE INTO media_input_presets"
        " (id, name, description, is_default, created_at)"
        " VALUES (1, 'Default', 'Default keyboard layout', 1, 0)",
        nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Data access
// ---------------------------------------------------------------------------

static std::string colStr(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

void InputMapsTab::FetchAll(sqlite3* db) {
    presets_.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, description, is_default"
        " FROM media_input_presets ORDER BY id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            InputPreset p;
            p.id          = sqlite3_column_int(stmt, 0);
            p.name        = colStr(stmt, 1);
            p.description = colStr(stmt, 2);
            p.is_default  = sqlite3_column_int(stmt, 3) != 0;
            presets_.push_back(std::move(p));
        }
        sqlite3_finalize(stmt);
    }
    // Reload bindings for the currently selected preset.
    if (sel_preset_ >= 0 && sel_preset_ < (int)presets_.size())
        FetchBindings(db, presets_[sel_preset_].id);
    else
        bindings_.clear();
    needs_fetch_ = false;
}

void InputMapsTab::FetchBindings(sqlite3* db, int preset_id) {
    bindings_.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, preset_id, context, key, modifier, trigger_type,"
        "       action, axis_value, enabled, remappable"
        " FROM media_input_maps WHERE preset_id = ? ORDER BY context, id",
        -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, preset_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        InputBinding b;
        b.id           = sqlite3_column_int(stmt, 0);
        b.preset_id    = sqlite3_column_int(stmt, 1);
        b.context      = colStr(stmt, 2);
        b.key          = colStr(stmt, 3);
        b.modifier     = colStr(stmt, 4);
        b.trigger_type = colStr(stmt, 5);
        b.action       = colStr(stmt, 6);
        b.axis_value   = (float)sqlite3_column_double(stmt, 7);
        b.enabled      = sqlite3_column_int(stmt, 8) != 0;
        b.remappable   = sqlite3_column_int(stmt, 9) != 0;
        bindings_.push_back(std::move(b));
    }
    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

void InputMapsTab::SavePreset(sqlite3* db, InputPreset& p) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (p.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_input_presets (name, description, is_default, created_at)"
            " VALUES (?, ?, ?, 0)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_input_presets SET name=?, description=?, is_default=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save preset error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, p.name.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, p.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, p.is_default ? 1 : 0);
    if (p.id != 0) sqlite3_bind_int(stmt, 4, p.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save preset error: %s", sqlite3_errmsg(db));
    } else {
        if (p.id == 0) p.id = (int)sqlite3_last_insert_rowid(db);
        // If this preset is now default, clear the flag on all others.
        if (p.is_default) {
            char sql[128];
            std::snprintf(sql, sizeof(sql),
                          "UPDATE media_input_presets SET is_default=0 WHERE id <> %d", p.id);
            sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        }
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Saved preset '%s' (id=%d).", p.name.c_str(), p.id);
        needs_fetch_ = true;
    }
    sqlite3_finalize(stmt);
}

void InputMapsTab::DeletePreset(sqlite3* db, int id) {
    // Cascade-delete all bindings for this preset first.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_input_maps WHERE preset_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM media_input_presets WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sel_preset_   = -1;
    sel_binding_  = -1;
    needs_fetch_  = true;
    std::snprintf(status_msg_, sizeof(status_msg_), "Deleted preset id=%d.", id);
}

void InputMapsTab::SaveBinding(sqlite3* db, InputBinding& b) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (b.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO media_input_maps"
            " (preset_id, context, key, modifier, trigger_type, action, axis_value, enabled, remappable)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_input_maps"
            " SET preset_id=?, context=?, key=?, modifier=?, trigger_type=?,"
            "     action=?, axis_value=?, enabled=?, remappable=?"
            " WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save binding error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int   (stmt, 1, b.preset_id);
    sqlite3_bind_text  (stmt, 2, b.context.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 3, b.key.c_str(),           -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 4, b.modifier.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 5, b.trigger_type.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 6, b.action.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 7, b.axis_value);
    sqlite3_bind_int   (stmt, 8, b.enabled    ? 1 : 0);
    sqlite3_bind_int   (stmt, 9, b.remappable ? 1 : 0);
    if (b.id != 0) sqlite3_bind_int(stmt, 10, b.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save binding error: %s", sqlite3_errmsg(db));
    } else {
        if (b.id == 0) b.id = (int)sqlite3_last_insert_rowid(db);
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Saved binding '%s' -> '%s'.", b.key.c_str(), b.action.c_str());
        // Refresh bindings list
        if (sel_preset_ >= 0 && sel_preset_ < (int)presets_.size())
            FetchBindings(db, presets_[sel_preset_].id);
    }
    sqlite3_finalize(stmt);
}

void InputMapsTab::DeleteBinding(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_input_maps WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sel_binding_ = -1;
    if (sel_preset_ >= 0 && sel_preset_ < (int)presets_.size())
        FetchBindings(db, presets_[sel_preset_].id);
}

// ---------------------------------------------------------------------------
// Action suggestions
// ---------------------------------------------------------------------------

void InputMapsTab::BuildActionSuggestions(sqlite3* db) {
    action_suggestions_.clear();

    // Universal UI/gameplay actions — always available.
    static const char* kUniversal[] = {
        "OpenInventory", "OpenMap", "OpenCharacter", "OpenSpellBook",
        "Screenshot", "Interact", "Chat", "ToggleUI",
        "MoveForward", "MoveBack", "MoveLeft", "MoveRight",
        "Jump", "Sprint", "Crouch", "Block", "Attack", "Cast",
    };
    for (const char* s : kUniversal)
        action_suggestions_.push_back(s);

    // Actions from actor defs.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT DISTINCT action FROM media_actor_anims ORDER BY action",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t && *t) {
                std::string s(t);
                // Don't duplicate universal entries.
                bool dup = false;
                for (const auto& a : action_suggestions_)
                    if (a == s) { dup = true; break; }
                if (!dup) action_suggestions_.push_back(s);
            }
        }
        sqlite3_finalize(stmt);
    }
    suggestions_dirty_ = false;
}

// ---------------------------------------------------------------------------
// Key capture widget
// ---------------------------------------------------------------------------

bool InputMapsTab::DrawKeyCapture(const char* btn_label,
                                  std::string& out_key,
                                  std::string& out_modifier) {
    // Map of ImGuiKey → canonical name string.
    struct KM { ImGuiKey key; const char* name; };
    static const KM kKeyMap[] = {
        {ImGuiKey_A, "A"}, {ImGuiKey_B, "B"}, {ImGuiKey_C, "C"}, {ImGuiKey_D, "D"},
        {ImGuiKey_E, "E"}, {ImGuiKey_F, "F"}, {ImGuiKey_G, "G"}, {ImGuiKey_H, "H"},
        {ImGuiKey_I, "I"}, {ImGuiKey_J, "J"}, {ImGuiKey_K, "K"}, {ImGuiKey_L, "L"},
        {ImGuiKey_M, "M"}, {ImGuiKey_N, "N"}, {ImGuiKey_O, "O"}, {ImGuiKey_P, "P"},
        {ImGuiKey_Q, "Q"}, {ImGuiKey_R, "R"}, {ImGuiKey_S, "S"}, {ImGuiKey_T, "T"},
        {ImGuiKey_U, "U"}, {ImGuiKey_V, "V"}, {ImGuiKey_W, "W"}, {ImGuiKey_X, "X"},
        {ImGuiKey_Y, "Y"}, {ImGuiKey_Z, "Z"},
        {ImGuiKey_0, "0"}, {ImGuiKey_1, "1"}, {ImGuiKey_2, "2"}, {ImGuiKey_3, "3"},
        {ImGuiKey_4, "4"}, {ImGuiKey_5, "5"}, {ImGuiKey_6, "6"}, {ImGuiKey_7, "7"},
        {ImGuiKey_8, "8"}, {ImGuiKey_9, "9"},
        {ImGuiKey_F1,  "F1"},  {ImGuiKey_F2,  "F2"},  {ImGuiKey_F3,  "F3"},
        {ImGuiKey_F4,  "F4"},  {ImGuiKey_F5,  "F5"},  {ImGuiKey_F6,  "F6"},
        {ImGuiKey_F7,  "F7"},  {ImGuiKey_F8,  "F8"},  {ImGuiKey_F9,  "F9"},
        {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
        {ImGuiKey_Space,     "Space"},     {ImGuiKey_Enter,     "Enter"},
        {ImGuiKey_Tab,       "Tab"},       {ImGuiKey_Backspace, "Backspace"},
        {ImGuiKey_Delete,    "Delete"},    {ImGuiKey_UpArrow,   "Up"},
        {ImGuiKey_DownArrow, "Down"},      {ImGuiKey_LeftArrow, "Left"},
        {ImGuiKey_RightArrow,"Right"},
    };

    if (capturing_key_) {
        ImGui::TextColored({1.f, 1.f, 0.f, 1.f}, "Press any key... (Esc to cancel)");

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            capturing_key_ = false;
            return false;
        }

        for (const auto& km : kKeyMap) {
            if (ImGui::IsKeyPressed(km.key)) {
                out_key = km.name;
                // Detect active modifiers — build alphabetically to avoid duplicates.
                const ImGuiIO& io = ImGui::GetIO();
                bool shift = io.KeyShift;
                bool ctrl  = io.KeyCtrl;
                bool alt   = io.KeyAlt;
                std::string mod;
                if (ctrl  && shift && alt) mod = "Shift+Ctrl+Alt";
                else if (ctrl  && shift)   mod = "Shift+Ctrl";
                else if (ctrl  && alt)     mod = "Ctrl+Alt";
                else if (shift && alt)     mod = "Shift+Alt";
                else if (ctrl)             mod = "Ctrl";
                else if (shift)            mod = "Shift";
                else if (alt)              mod = "Alt";
                out_modifier = mod;
                capturing_key_ = false;
                return true;
            }
        }
        return false;
    }

    if (ImGui::Button(btn_label)) {
        capturing_key_ = true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main Draw
// ---------------------------------------------------------------------------

void InputMapsTab::Draw(sqlite3* db) {
    if (needs_fetch_) {
        EnsureTables(db);
        FetchAll(db);
    }

    // Build action suggestions lazily.
    if (suggestions_dirty_) BuildActionSuggestions(db);

    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    // ── Two-panel layout ─────────────────────────────────────────────────────

    // Left panel: preset list
    ImGui::BeginChild("##ip_presets", {200, 0}, true);

    if (ImGui::Button("+ New Preset")) {
        new_preset_      = true;
        dirty_preset_    = false;
        edit_preset_     = {};
        edit_preset_idx_ = -1;
    }

    ImGui::Separator();
    for (int i = 0; i < (int)presets_.size(); ++i) {
        const auto& p = presets_[i];
        char label[128];
        std::snprintf(label, sizeof(label), "%s%s##ip%d",
                      p.name.c_str(),
                      p.is_default ? " [default]" : "",
                      i);
        bool sel = (sel_preset_ == i);
        if (ImGui::Selectable(label, sel)) {
            if (sel_preset_ != i) {
                sel_preset_      = i;
                edit_preset_     = p;
                edit_preset_idx_ = i;
                dirty_preset_    = false;
                new_preset_      = false;
                sel_binding_     = -1;
                dirty_binding_   = false;
                suggestions_dirty_ = true;
                FetchBindings(db, p.id);
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right panel
    ImGui::BeginChild("##ip_right", {0, 0}, false);

    // --- Preset editor ---
    if (new_preset_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Preset");
        ImGui::Separator();
        char nbuf[128]; std::strncpy(nbuf, edit_preset_.name.c_str(), 127); nbuf[127] = 0;
        if (ImGui::InputText("Name##np", nbuf, 128)) edit_preset_.name = nbuf;
        char dbuf[256]; std::strncpy(dbuf, edit_preset_.description.c_str(), 255); dbuf[255] = 0;
        if (ImGui::InputText("Description##np", dbuf, 256)) edit_preset_.description = dbuf;
        ImGui::Checkbox("Set as default##np", &edit_preset_.is_default);
        ImGui::Spacing();
        if (ImGui::Button("Create##np")) {
            SavePreset(db, edit_preset_);
            new_preset_ = false;
            FetchAll(db);
            // Select the newly created preset.
            for (int i = 0; i < (int)presets_.size(); ++i) {
                if (presets_[i].id == edit_preset_.id) {
                    sel_preset_ = i;
                    FetchBindings(db, edit_preset_.id);
                    break;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##np")) new_preset_ = false;

    } else if (sel_preset_ >= 0 && sel_preset_ < (int)presets_.size()) {
        // Preset header + edit strip
        InputPreset& p = edit_preset_;
        ImGui::Text("Preset: [id=%d]", p.id);
        ImGui::SameLine(0, 8);
        char nbuf[128]; std::strncpy(nbuf, p.name.c_str(), 127); nbuf[127] = 0;
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputText("##pn", nbuf, 128)) { p.name = nbuf; dirty_preset_ = true; }
        ImGui::SameLine();
        char dbuf[256]; std::strncpy(dbuf, p.description.c_str(), 255); dbuf[255] = 0;
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText("##pd", dbuf, 256)) { p.description = dbuf; dirty_preset_ = true; }
        ImGui::SameLine();
        if (ImGui::Checkbox("Default##p", &p.is_default)) dirty_preset_ = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!dirty_preset_);
        if (ImGui::Button("Save##p")) {
            SavePreset(db, p);
            presets_[sel_preset_] = p;
            dirty_preset_ = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete Preset##p")) DeletePreset(db, p.id);
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Context filter
        {
            static const char* kContexts[] = {
                "gameplay", "menu", "vehicle", "swimming", "chat"
            };
            // Build a current list that includes any context already in bindings
            // but not in the static list.
            ImGui::TextUnformatted("Context filter:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("##ctx_filter", filter_context_.c_str())) {
                for (const char* c : kContexts) {
                    bool sel = filter_context_ == c;
                    if (ImGui::Selectable(c, sel)) filter_context_ = c;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                // Any extra contexts in the data
                for (const auto& b : bindings_) {
                    bool found = false;
                    for (const char* c : kContexts) if (b.context == c) { found = true; break; }
                    if (!found) {
                        bool s = filter_context_ == b.context;
                        if (ImGui::Selectable(b.context.c_str(), s))
                            filter_context_ = b.context;
                        if (s) ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Add Row")) {
                new_binding_       = true;
                dirty_binding_     = false;
                sel_binding_       = -1;
                edit_binding_      = {};
                edit_binding_.preset_id    = p.id;
                edit_binding_.context      = filter_context_;
                edit_binding_.trigger_type = "press";
                edit_binding_.axis_value   = 1.f;
                edit_binding_.enabled      = true;
                edit_binding_.remappable   = true;
            }
        }
        ImGui::Spacing();

        // Detect conflict (context, key, modifier, trigger_type) duplicates.
        auto isConflict = [&](int idx) -> bool {
            const auto& b = bindings_[idx];
            if (b.key.empty()) return false;
            for (int j = 0; j < (int)bindings_.size(); ++j) {
                if (j == idx) continue;
                const auto& o = bindings_[j];
                if (o.context == b.context && o.key == b.key &&
                    o.modifier == b.modifier && o.trigger_type == b.trigger_type)
                    return true;
            }
            return false;
        };

        // Bindings table
        static const char* kTriggers[]  = {"press", "release", "hold", "double", "axis"};
        static const char* kModifiers[] = {"", "Shift", "Ctrl", "Alt",
                                           "Shift+Ctrl", "Shift+Alt", "Ctrl+Alt", "Shift+Ctrl+Alt"};

        if (ImGui::BeginTable("##bindings_tbl", 10,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_ScrollY, ImVec2(0, 320))) {

            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("Context",  ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Key",      ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Modifier", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Trigger",  ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Axis",     ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("On",       ImGuiTableColumnFlags_WidthFixed, 30);
            ImGui::TableSetupColumn("Remap",    ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("##save",   ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("##del",    ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableHeadersRow();

            // Collect visible indices
            for (int idx = 0; idx < (int)bindings_.size(); ++idx) {
                InputBinding& b = bindings_[idx];
                if (b.context != filter_context_) continue;

                bool conflict = isConflict(idx);
                ImGui::PushID(idx + 30000);
                ImGui::TableNextRow();

                if (conflict)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           ImGui::GetColorU32({0.6f, 0.1f, 0.1f, 0.5f}));

                // Context
                ImGui::TableNextColumn();
                {
                    char cbuf[32]; std::strncpy(cbuf, b.context.c_str(), 31); cbuf[31] = 0;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##ctx", cbuf, 32)) b.context = cbuf;
                }

                // Key — capture button
                ImGui::TableNextColumn();
                {
                    bool captured = false;
                    if (capturing_key_ && capturing_row_ == idx) {
                        captured = DrawKeyCapture("##cap", b.key, b.modifier);
                    } else {
                        char klabel[64];
                        std::snprintf(klabel, sizeof(klabel), "%s##keybtn%d",
                                      b.key.empty() ? "(none)" : b.key.c_str(), idx);
                        if (ImGui::Button(klabel)) {
                            capturing_key_ = true;
                            capturing_row_ = idx;
                        }
                    }
                    (void)captured;
                }

                // Modifier
                ImGui::TableNextColumn();
                {
                    int mIdx = 0;
                    for (int m = 0; m < 8; ++m)
                        if (b.modifier == kModifiers[m]) { mIdx = m; break; }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##mod", &mIdx, kModifiers, 8))
                        b.modifier = kModifiers[mIdx];
                }

                // Trigger type
                ImGui::TableNextColumn();
                {
                    int tIdx = 0;
                    for (int t = 0; t < 5; ++t)
                        if (b.trigger_type == kTriggers[t]) { tIdx = t; break; }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##trg", &tIdx, kTriggers, 5))
                        b.trigger_type = kTriggers[tIdx];
                }

                // Action — combo with suggestions
                ImGui::TableNextColumn();
                {
                    ImGui::SetNextItemWidth(-1);
                    char abuf[64]; std::strncpy(abuf, b.action.c_str(), 63); abuf[63] = 0;
                    if (ImGui::BeginCombo("##act", abuf, ImGuiComboFlags_NoArrowButton)) {
                        // Freeform text first
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::InputText("##actinp", abuf, 64)) b.action = abuf;
                        ImGui::Separator();
                        for (const auto& s : action_suggestions_) {
                            bool sel = (b.action == s);
                            if (ImGui::Selectable(s.c_str(), sel)) b.action = s;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }

                // Axis value
                ImGui::TableNextColumn();
                {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputFloat("##ax", &b.axis_value, 0.f, 0.f, "%.1f");
                }

                // Enabled
                ImGui::TableNextColumn();
                ImGui::Checkbox("##en", &b.enabled);

                // Remappable
                ImGui::TableNextColumn();
                ImGui::Checkbox("##rm", &b.remappable);

                // Save
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Save")) SaveBinding(db, b);

                // Delete
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Button, {0.55f, 0.1f, 0.1f, 1.f});
                if (ImGui::SmallButton("Del")) DeleteBinding(db, b.id);
                ImGui::PopStyleColor();

                if (conflict && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Conflict: another row shares the same"
                                      " (context, key, modifier, trigger).");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // New binding form
        if (new_binding_) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Binding");
            ImGui::Spacing();

            InputBinding& nb = edit_binding_;

            // Key capture for new binding
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Key:");
            ImGui::SameLine();
            if (capturing_key_ && capturing_row_ == -1) {
                DrawKeyCapture("##capnew", nb.key, nb.modifier);
            } else {
                char klabel[64];
                std::snprintf(klabel, sizeof(klabel), "%s##newkeybtn",
                              nb.key.empty() ? "Click to capture..." : nb.key.c_str());
                if (ImGui::Button(klabel)) {
                    capturing_key_ = true;
                    capturing_row_ = -1;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Modifier:");
            ImGui::SameLine();
            {
                int mIdx = 0;
                for (int m = 0; m < 8; ++m)
                    if (nb.modifier == kModifiers[m]) { mIdx = m; break; }
                ImGui::SetNextItemWidth(120);
                if (ImGui::Combo("##nmod", &mIdx, kModifiers, 8))
                    nb.modifier = kModifiers[mIdx];
            }

            ImGui::TextUnformatted("Trigger:");
            ImGui::SameLine();
            {
                int tIdx = 0;
                for (int t = 0; t < 5; ++t)
                    if (nb.trigger_type == kTriggers[t]) { tIdx = t; break; }
                ImGui::SetNextItemWidth(100);
                if (ImGui::Combo("##ntrg", &tIdx, kTriggers, 5))
                    nb.trigger_type = kTriggers[tIdx];
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Action:");
            ImGui::SameLine();
            {
                ImGui::SetNextItemWidth(160);
                char abuf[64]; std::strncpy(abuf, nb.action.c_str(), 63); abuf[63] = 0;
                if (ImGui::BeginCombo("##nact", abuf, ImGuiComboFlags_NoArrowButton)) {
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##nactinp", abuf, 64)) nb.action = abuf;
                    ImGui::Separator();
                    for (const auto& s : action_suggestions_) {
                        bool sel = (nb.action == s);
                        if (ImGui::Selectable(s.c_str(), sel)) nb.action = s;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::InputFloat("Axis##nb", &nb.axis_value, 0.f, 0.f, "%.1f");

            ImGui::Checkbox("Enabled##nb",    &nb.enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Remappable##nb", &nb.remappable);

            ImGui::Spacing();
            if (ImGui::Button("Add##nb")) {
                if (nb.key.empty()) {
                    std::snprintf(status_msg_, sizeof(status_msg_),
                                  "Press 'Click to capture' to assign a key first.");
                } else if (nb.action.empty()) {
                    std::snprintf(status_msg_, sizeof(status_msg_),
                                  "Choose an action before adding.");
                } else {
                    SaveBinding(db, nb);
                    new_binding_ = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##nb")) new_binding_ = false;
        }

    } else {
        ImGui::TextDisabled("Select a preset from the left, or create a new one.");
    }

    ImGui::EndChild(); // ##ip_right
}

} // namespace gue
