#include "settings.h"

#include <imgui.h>
#include "../ui_widgets.h"
#include "rco/renderer/pipeline.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>

namespace gue {

namespace {

static std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? std::string(text) : std::string();
}

static std::string BuildModelLabel(int id, const std::string& name, const std::string& filePath) {
    std::string label = std::to_string(id) + ": ";
    if (!name.empty()) {
        label += name;
    } else {
        label += "(unnamed)";
    }
    if (!filePath.empty()) {
        label += " [" + filePath + "]";
    }
    return label;
}

static bool LoadSetting(sqlite3* db, const char* key, std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT value FROM game_settings WHERE key = ? LIMIT 1",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = ColText(stmt, 0);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool SaveSetting(sqlite3* db, const char* key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO game_settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

} // namespace

void SettingsTab::EnsureTables(sqlite3* db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS game_settings ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT ''"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS anim_vocabulary ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT NOT NULL UNIQUE,"
        "  parent_id INTEGER NOT NULL DEFAULT 0"
        ")",
        nullptr, nullptr, nullptr);

    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS socket_vocabulary ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE"
        ")",
        nullptr, nullptr, nullptr);
}

void SettingsTab::LoadModels(sqlite3* db) {
    media_models_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, file_path FROM media_models ORDER BY id",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_), "Load models: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int id = sqlite3_column_int(stmt, 0);
        media_models_.push_back({
            id,
            BuildModelLabel(id, ColText(stmt, 1), ColText(stmt, 2))
        });
    }
    sqlite3_finalize(stmt);

    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Loaded %d model(s).", (int)media_models_.size());
}

void SettingsTab::LoadBloodFXKeys(sqlite3* db) {
    blood_fx_keys_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT fx_key FROM fx_templates ORDER BY fx_key",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(status_msg_, sizeof(status_msg_), "Load blood FX keys: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string fxKey = ColText(stmt, 0);
        if (!fxKey.empty()) {
            blood_fx_keys_.push_back(fxKey);
        }
    }
    sqlite3_finalize(stmt);

    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Loaded %d blood FX key(s).", (int)blood_fx_keys_.size());
}

void SettingsTab::LoadSettings(sqlite3* db) {
    default_drop_model_id_  = 0;
    default_drop_model_scale_ = 1.f;
    black_cutout_threshold_ = 0.005f;
    blood_fx_key_.clear();
    blood_fx_mode_ = "basic";

    std::string value;
    if (!LoadSetting(db, "default_drop_model_id", value)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Load settings: %s", sqlite3_errmsg(db));
    } else if (!value.empty()) {
        try {
            int parsed = std::stoi(value);
            if (parsed > 0) {
                default_drop_model_id_ = parsed;
            }
        } catch (...) {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Invalid '%s' value in game_settings (using empty).",
                          "default_drop_model_id");
        }
    } else if (status_msg_[0] == '\0') {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Loaded default drop model: <empty>.");
    }

    if (!LoadSetting(db, "default_drop_model_scale", value)) {
        if (status_msg_[0] == '\0') {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Load settings: %s", sqlite3_errmsg(db));
        }
    } else if (!value.empty()) {
        try {
            float parsed = std::stof(value);
            if (parsed < 0.01f) parsed = 0.01f;
            default_drop_model_scale_ = parsed;
        } catch (...) {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Invalid '%s' value in game_settings (using default).",
                          "default_drop_model_scale");
        }
    }

    if (!LoadSetting(db, "blood_fx_key", value)) {
        if (status_msg_[0] == '\0') {
            blood_fx_key_.clear();
        }
    } else {
        blood_fx_key_ = value;
    }

    if (!LoadSetting(db, "blood_mode", value)) {
        if (status_msg_[0] == '\0') {
            blood_fx_mode_ = "basic";
        }
    } else {
        if (value == "all") {
            blood_fx_mode_ = "all";
        } else {
            blood_fx_mode_ = "basic";
        }
    }

    if (LoadSetting(db, "black_cutout_threshold", value) && !value.empty()) {
        try {
            float parsed = std::stof(value);
            if (parsed >= 0.f && parsed <= 0.1f) black_cutout_threshold_ = parsed;
        } catch (...) {}
    }
    if (pipeline_) pipeline_->SetBlackCutoutThreshold(black_cutout_threshold_);

    if (status_msg_[0] == '\0') {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Loaded settings.");
    }

    dirty_ = false;
}

bool SettingsTab::SaveSettings(sqlite3* db) {
    const std::string model_value = (default_drop_model_id_ == 0)
        ? std::string()
        : std::to_string(default_drop_model_id_);
    if (!SaveSetting(db, "default_drop_model_id", model_value)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }

    if (default_drop_model_scale_ < 0.01f) {
        default_drop_model_scale_ = 0.01f;
    }
    const std::string scale_value = std::to_string(default_drop_model_scale_);
    if (!SaveSetting(db, "default_drop_model_scale", scale_value)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    if (!SaveSetting(db, "blood_fx_key", blood_fx_key_)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }
    if (!SaveSetting(db, "blood_mode", blood_fx_mode_)) {
        std::snprintf(status_msg_, sizeof(status_msg_),
                      "Save error: %s", sqlite3_errmsg(db));
        return false;
    }

    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6f", black_cutout_threshold_);
        if (!SaveSetting(db, "black_cutout_threshold", buf)) {
            std::snprintf(status_msg_, sizeof(status_msg_),
                          "Save error: %s", sqlite3_errmsg(db));
            return false;
        }
    }

    dirty_ = false;
    std::snprintf(status_msg_, sizeof(status_msg_),
                  "Saved drop model id=%d, scale=%.2f, blood fx key=%s, mode=%s, cutout threshold=%.4f.",
                  default_drop_model_id_, default_drop_model_scale_,
                  blood_fx_key_.c_str(), blood_fx_mode_.c_str(), black_cutout_threshold_);
    return true;
}

void SettingsTab::Draw(sqlite3* db) {
    if (!db) return;
    if (need_fetch_) {
        EnsureTables(db);
        LoadModels(db);
        LoadBloodFXKeys(db);
        LoadSettings(db);
        need_fetch_ = false;
    }

    if (ImGui::BeginTabBar("##settings_subtabs")) {
        if (ImGui::BeginTabItem("General")) {
            DrawGeneralSettings(db);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Animation Vocabulary")) {
            EnsureTables(db);
            DrawAnimVocabulary(db);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sockets")) {
            EnsureTables(db);
            DrawSocketVocab(db);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void SettingsTab::DrawGeneralSettings(sqlite3* db) {
    ImGui::Text("Global settings");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        need_fetch_ = true;
        EnsureTables(db);
        LoadModels(db);
        LoadBloodFXKeys(db);
        LoadSettings(db);
        need_fetch_ = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_msg_);
    ImGui::Separator();

    int current_id = default_drop_model_id_;
    std::string current_fx_key = blood_fx_key_;
    float current_scale = default_drop_model_scale_;

    ImGui::Text("Drop item mesh");
    if (media_models_.empty()) {
        ImGui::TextDisabled("No models found in media_models.");
    } else {
        if (ui::SearchableComboId("Default Drop Model", current_id, media_models_, "(none)")) {
            default_drop_model_id_ = current_id;
            dirty_ = true;
        }
    }
    if (ImGui::InputFloat("Drop Model Scale", &current_scale, 0.05f, 0.5f, "%.2f")) {
        default_drop_model_scale_ = current_scale;
        if (default_drop_model_scale_ < 0.01f) {
            default_drop_model_scale_ = 0.01f;
        }
        dirty_ = true;
    }
    ImGui::Spacing();
    ImGui::Text("Blood FX on hit");
    if (blood_fx_keys_.empty()) {
        ImGui::TextDisabled("No fx_keys found in fx_templates.");
    } else if (ui::SearchableComboString("Blood FX key", current_fx_key, blood_fx_keys_, "(none)")) {
        blood_fx_key_ = current_fx_key;
        dirty_ = true;
    }
    if (ImGui::RadioButton("On hit (basic only)", blood_fx_mode_ == "basic")) {
        if (blood_fx_mode_ != "basic") {
            blood_fx_mode_ = "basic";
            dirty_ = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("All hits", blood_fx_mode_ == "all")) {
        if (blood_fx_mode_ != "all") {
            blood_fx_mode_ = "all";
            dirty_ = true;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Rendering");
    ImGui::SetNextItemWidth(220);
    if (ImGui::SliderFloat("Black cutout threshold", &black_cutout_threshold_, 0.f, 0.1f, "%.4f")) {
        if (pipeline_) pipeline_->SetBlackCutoutThreshold(black_cutout_threshold_);
        dirty_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Global luminance threshold for materials with 'Black Cutout' enabled.\n"
            "Pixels where lum < threshold are discarded (hair, foliage, fences).\n"
            "0.005 = safe default (only cuts pure black + minor compression noise).");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Value is stored as TEXT in game_settings: "
        "default_drop_model_id='' means no mesh; "
        "default_drop_model_scale is a float multiplier.");

    ImGui::Spacing();
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::Button("Save")) {
        SaveSettings(db);
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Animation Vocabulary (Phase A.1)
// ---------------------------------------------------------------------------

void SettingsTab::LoadAnimVocabulary(sqlite3* db) {
    anim_vocab_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, parent_id FROM anim_vocabulary ORDER BY name",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Load anim_vocabulary: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AnimVocabNode node;
        node.id = sqlite3_column_int(stmt, 0);
        node.name = ColText(stmt, 1);
        node.parent_id = sqlite3_column_int(stmt, 2);
        anim_vocab_.push_back(node);
    }
    sqlite3_finalize(stmt);

    std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                   "Loaded %d animation action(s).", (int)anim_vocab_.size());
}

void SettingsTab::LoadWeaponAnimStyleKeysForVocab(sqlite3* db) {
    vocab_weapon_anim_style_keys_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT style_key FROM weapon_anim_styles WHERE enabled = 1 ORDER BY style_key",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        vocab_weapon_anim_style_keys_.push_back(ColText(stmt, 0));
    }
    sqlite3_finalize(stmt);
}

bool SettingsTab::AnimVocabNameExists(sqlite3* db, const std::string& name) {
    (void)db;
    for (const auto& n : anim_vocab_) {
        if (n.name == name) return true;
    }
    return false;
}

bool SettingsTab::AnimVocabHasChildren(int id) const {
    for (const auto& n : anim_vocab_) {
        if (n.parent_id == id) return true;
    }
    return false;
}

void SettingsTab::AnimVocabAddNode(sqlite3* db, const std::string& name, int parent_id) {
    if (name.empty()) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_), "Name cannot be empty.");
        return;
    }
    if (AnimVocabNameExists(db, name)) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "An action named '%s' already exists.", name.c_str());
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO anim_vocabulary (name, parent_id) VALUES (?, ?)",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Insert error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, parent_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Insert error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_finalize(stmt);

    std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                   "Added '%s'.", name.c_str());
    anim_vocab_need_fetch_ = true;
}

void SettingsTab::AnimVocabRenameNode(sqlite3* db, int id, const std::string& new_name) {
    if (new_name.empty()) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_), "Name cannot be empty.");
        return;
    }
    if (AnimVocabNameExists(db, new_name)) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "An action named '%s' already exists.", new_name.c_str());
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "UPDATE anim_vocabulary SET name = ? WHERE id = ?",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Rename error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Rename error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_finalize(stmt);

    std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_), "Renamed to '%s'.", new_name.c_str());
    anim_vocab_need_fetch_ = true;
}

void SettingsTab::AnimVocabDeleteNode(sqlite3* db, int id) {
    if (AnimVocabHasChildren(id)) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Cannot delete: this action has children. Delete or move them first.");
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM anim_vocabulary WHERE id = ?",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Delete error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_),
                       "Delete error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_finalize(stmt);

    std::snprintf(anim_vocab_status_, sizeof(anim_vocab_status_), "Deleted.");
    anim_vocab_need_fetch_ = true;
}

void SettingsTab::DrawAnimVocabNode(sqlite3* db, const AnimVocabNode& node, const std::string& parent_name) {
    ImGui::PushID(node.id);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    bool open = ImGui::TreeNodeEx((void*)(intptr_t)node.id, flags, "%s", node.name.c_str());

    // Detected purely by naming convention ("{parent}_{style_key}") — there
    // is no DB column marking a node as form-generated, since
    // DrawAddWeaponBindingForm/AnimVocabAddNode create it as an ordinary
    // anim_vocabulary row. Used below both for the dead-style warning
    // (item 5) and the Rename warning (item 3, batch-creation follow-up).
    bool is_weapon_composite = false;
    std::string composite_style_suffix;
    if (!parent_name.empty()) {
        const std::string prefix = parent_name + "_";
        if (node.name.size() > prefix.size() &&
            node.name.compare(0, prefix.size(), prefix) == 0) {
            is_weapon_composite = true;
            composite_style_suffix = node.name.substr(prefix.size());
        }
    }

    // Dead weapon-binding warning (item 5): if the style_key suffix no
    // longer matches any ENABLED weapon_anim_styles row, this binding can
    // never be reached by BroadcastAnimate's fallback walk
    // (combat_events.go) for any real weapon — flag it inline instead of
    // letting the dev wonder why it "isn't working".
    if (is_weapon_composite) {
        bool style_alive = false;
        for (const auto& s : vocab_weapon_anim_style_keys_) {
            if (s == composite_style_suffix) { style_alive = true; break; }
        }
        if (!style_alive) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.35f, 0.3f, 1.f},
                "(weapon anim style '%s' não existe mais — este binding nunca será alcançado)",
                composite_style_suffix.c_str());
        }
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Rename")) {
        anim_vocab_rename_id_ = node.id;
        std::snprintf(anim_vocab_rename_buf_, sizeof(anim_vocab_rename_buf_), "%s", node.name.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Child")) {
        AnimVocabAddNode(db, "NewAction", node.id);
    }
    ImGui::SameLine();
    bool hasChildren = AnimVocabHasChildren(node.id);
    ImGui::BeginDisabled(hasChildren);
    if (ImGui::SmallButton("Delete")) {
        AnimVocabDeleteNode(db, node.id);
    }
    ImGui::EndDisabled();
    if (hasChildren) {
        ImGui::SameLine();
        ImGui::TextDisabled("(has children)");
    }

    if (anim_vocab_rename_id_ == node.id) {
        // Not a blocking confirm — the same naming-convention detection used
        // for the dead-style warning above, surfaced as a heads-up so the
        // dev doesn't unknowingly strip the "which weapon does this belong
        // to" signal the {Base}_{StyleKey} name carries. Renaming still
        // proceeds on Save regardless.
        if (is_weapon_composite) {
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.f},
                "Este nó foi gerado automaticamente (Ação+Arma) — renomear pode "
                "dificultar rastrear a qual arma ele pertence.");
        }
        ImGui::PushItemWidth(180);
        ImGui::InputText("##rename", anim_vocab_rename_buf_, sizeof(anim_vocab_rename_buf_));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::SmallButton("Save##rename")) {
            AnimVocabRenameNode(db, node.id, anim_vocab_rename_buf_);
            anim_vocab_rename_id_ = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##rename")) {
            anim_vocab_rename_id_ = 0;
        }
    }

    if (open) {
        for (const auto& child : anim_vocab_) {
            if (child.parent_id == node.id) {
                DrawAnimVocabNode(db, child, node.name);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void SettingsTab::DrawAnimVocabulary(sqlite3* db) {
    if (anim_vocab_need_fetch_) {
        LoadAnimVocabulary(db);
        anim_vocab_need_fetch_ = false;
    }
    if (vocab_weapon_anim_style_keys_need_fetch_) {
        LoadWeaponAnimStyleKeysForVocab(db);
        vocab_weapon_anim_style_keys_need_fetch_ = false;
    }

    ImGui::TextWrapped(
        "Tree of animation action names. A child's parent is its fallback "
        "action (used when an actor has no animation bound for the child). "
        "Root actions (no parent) have no fallback.");

    if (ImGui::Button("Refresh")) {
        anim_vocab_need_fetch_ = true;
        vocab_weapon_anim_style_keys_need_fetch_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", anim_vocab_status_);
    ImGui::Separator();

    ImGui::PushItemWidth(180);
    ImGui::InputText("##new_root_name", anim_vocab_new_root_name_, sizeof(anim_vocab_new_root_name_));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("+ Add Root")) {
        AnimVocabAddNode(db, anim_vocab_new_root_name_, 0);
        anim_vocab_new_root_name_[0] = '\0';
    }

    ImGui::Separator();

    DrawAddWeaponBindingForm(db);

    ImGui::Separator();

    for (const auto& node : anim_vocab_) {
        if (node.parent_id == 0) {
            DrawAnimVocabNode(db, node, "");
        }
    }
}

void SettingsTab::DrawAddWeaponBindingForm(sqlite3* db) {
    ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Add Weapon-Specific Binding");
    ImGui::TextWrapped(
        "Use this to give a weapon style its own animation clip — e.g. so a "
        "sword swing looks different from a staff swing on the SAME base "
        "action. Pick a base action and a weapon style below; this creates a "
        "composite action ({Base}_{StyleKey}) as a CHILD of the base action, "
        "so it inherits that action as its fallback the exact same way any "
        "other child in the tree does — no new binding type, just a node "
        "whose parent happens to be the generic action. Styles come from "
        "Weapon Anim Styles (PHYSICAL grip/pose — sword_1h, staff, bow...), "
        "NOT Weapon Kits (skills) — many differently-kitted weapons can "
        "share one grip and should resolve to the same binding here.");

    if (vocab_weapon_anim_style_keys_.empty()) {
        ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.f},
            "No enabled Weapon Anim Styles found — create one in the Weapon Anim Styles tab first.");
        return;
    }

    // Base action can be ANY node in the tree, not just a root — e.g. "Slash"
    // (a child of "Attack") is a valid, more specific base for a weapon
    // composite than the generic "Attack". The fallback walk (AnimVocabNode
    // parent_id chase, mirrored server-side in AnimFallbackParent) already
    // supports a composite child at any depth, so this is purely a UI list
    // change — see docs/TECH_DEBT.md investigation note on this form.
    std::vector<std::pair<const AnimVocabNode*, int>> allNodes; // (node, depth)
    {
        std::function<void(int, int)> addChildren = [&](int parent_id, int depth) {
            for (const auto& n : anim_vocab_) {
                if (n.parent_id != parent_id) continue;
                allNodes.emplace_back(&n, depth);
                addChildren(n.id, depth + 1);
            }
        };
        addChildren(0, 0);
    }
    if (allNodes.empty()) {
        ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.f},
            "No actions in the vocabulary yet — add a root one above first.");
        return;
    }

    if (wsb_style_idx_ >= (int)vocab_weapon_anim_style_keys_.size()) wsb_style_idx_ = -1;

    // Multi-select base actions (item 2, batch creation): checkboxes in a
    // scrollable child region instead of a single-value combo, so the dev
    // can pick e.g. Attack + Jump in one pass. Selection is tracked by node
    // id (wsb_base_selected_ids_), not list index, so it survives the tree
    // being re-fetched between frames (unlike the old single-combo index).
    ImGui::TextUnformatted("Base Action(s)");
    ImGui::BeginChild("##wsb_base_list", ImVec2(220, 120), true);
    for (const auto& entry : allNodes) {
        const AnimVocabNode* n = entry.first;
        int depth = entry.second;
        bool selected = std::find(wsb_base_selected_ids_.begin(), wsb_base_selected_ids_.end(), n->id)
                        != wsb_base_selected_ids_.end();
        ImGui::PushID(n->id);
        std::string indented(depth * 2, ' ');
        indented += n->name;
        if (ImGui::Checkbox(indented.c_str(), &selected)) {
            if (selected) {
                wsb_base_selected_ids_.push_back(n->id);
            } else {
                wsb_base_selected_ids_.erase(
                    std::remove(wsb_base_selected_ids_.begin(), wsb_base_selected_ids_.end(), n->id),
                    wsb_base_selected_ids_.end());
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine(0.f, 12.f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted("+");
    ImGui::SameLine();

    ImGui::PushItemWidth(160);
    const char* style_label = (wsb_style_idx_ >= 0) ? vocab_weapon_anim_style_keys_[wsb_style_idx_].c_str() : "(weapon anim style)";
    if (ImGui::BeginCombo("Weapon Anim Style##wsb_style", style_label)) {
        for (int i = 0; i < (int)vocab_weapon_anim_style_keys_.size(); ++i) {
            bool is_sel = (i == wsb_style_idx_);
            if (ImGui::Selectable(vocab_weapon_anim_style_keys_[i].c_str(), is_sel)) wsb_style_idx_ = i;
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // Resolve selected ids back to nodes (in allNodes tree order, so the
    // preview list below reads top-to-bottom the same way the checkbox list
    // does) and build the composite name each one would produce.
    std::vector<const AnimVocabNode*> selectedBases;
    for (const auto& entry : allNodes) {
        if (std::find(wsb_base_selected_ids_.begin(), wsb_base_selected_ids_.end(), entry.first->id)
            != wsb_base_selected_ids_.end()) {
            selectedBases.push_back(entry.first);
        }
    }

    const std::string style = (wsb_style_idx_ >= 0) ? vocab_weapon_anim_style_keys_[wsb_style_idx_] : std::string();

    if (selectedBases.empty() || style.empty()) {
        ImGui::TextDisabled("(pick at least one base action and a weapon anim style)");
        return;
    }

    // Name-collision warning (item 1): a base action whose name matches the
    // style key (case-insensitively — e.g. action "Bow" + style "bow") is
    // valid but produces a redundant-looking name like "Bow_bow". Not
    // blocked (see investigation: this can legitimately happen when an
    // action's name already carries weapon semantics, e.g. a custom "Bow"
    // action or the seeded "Shoot"/style "bow" pairing) — just surfaced so
    // it's not a silent surprise.
    auto ciEquals = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    };
    for (const auto* base : selectedBases) {
        if (ciEquals(base->name, style)) {
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.f},
                "Aviso: a acao base '%s' tem o mesmo nome do weapon anim style '%s' "
                "(sem diferenciar maiusculas/minusculas) — isso vai gerar '%s_%s', "
                "que parece redundante. Confirma que e intencional antes de criar.",
                base->name.c_str(), style.c_str(), base->name.c_str(), style.c_str());
        }
    }

    // Preview list of every composite name this batch would create, with
    // duplicates (already in the vocabulary) called out and skipped rather
    // than silently re-attempted — same duplicate rule AnimVocabAddNode's
    // caller already relied on for the single-binding form.
    ImGui::TextUnformatted("Will create:");
    int toCreateCount = 0;
    for (const auto* base : selectedBases) {
        std::string composite = base->name + "_" + style;
        bool duplicate = AnimVocabNameExists(db, composite);
        if (duplicate) {
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.f}, "  %s (already exists — skipped)", composite.c_str());
        } else {
            ImGui::TextColored({0.6f, 1.0f, 0.6f, 1.f}, "  %s", composite.c_str());
            ++toCreateCount;
        }
    }

    ImGui::BeginDisabled(toCreateCount == 0);
    if (ImGui::Button("Add Weapon Bindings")) {
        int created = 0;
        for (const auto* base : selectedBases) {
            std::string composite = base->name + "_" + style;
            if (AnimVocabNameExists(db, composite)) continue;
            AnimVocabAddNode(db, composite, base->id);
            ++created;
        }
        std::snprintf(wsb_status_, sizeof(wsb_status_), "Added %d binding(s).", created);
        wsb_style_idx_ = -1;
        wsb_base_selected_ids_.clear();
    }
    ImGui::EndDisabled();

    if (wsb_status_[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", wsb_status_);
    }
}

// ---------------------------------------------------------------------------
// Socket Vocabulary (Arco B / B2)
// ---------------------------------------------------------------------------

void SettingsTab::LoadSocketVocab(sqlite3* db) {
    socket_vocab_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name FROM socket_vocabulary ORDER BY name",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Load socket_vocabulary: %s", sqlite3_errmsg(db));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SocketEntry e;
        e.id   = sqlite3_column_int(stmt, 0);
        e.name = ColText(stmt, 1);
        socket_vocab_.push_back(e);
    }
    sqlite3_finalize(stmt);

    std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                  "Loaded %d socket(s).", (int)socket_vocab_.size());
}

bool SettingsTab::SocketNameExists(const std::string& name) const {
    for (const auto& e : socket_vocab_) {
        if (e.name == name) return true;
    }
    return false;
}

void SettingsTab::SocketAdd(sqlite3* db, const std::string& name) {
    if (name.empty()) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_), "Name cannot be empty.");
        return;
    }
    if (SocketNameExists(name)) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "A socket named '%s' already exists.", name.c_str());
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO socket_vocabulary (name) VALUES (?)",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Insert error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Insert error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_finalize(stmt);

    std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_), "Added '%s'.", name.c_str());
    socket_vocab_need_fetch_ = true;
}

void SettingsTab::SocketRename(sqlite3* db, int id, const std::string& new_name) {
    if (new_name.empty()) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_), "Name cannot be empty.");
        return;
    }
    if (SocketNameExists(new_name)) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "A socket named '%s' already exists.", new_name.c_str());
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "UPDATE socket_vocabulary SET name = ? WHERE id = ?",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Rename error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Rename error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_finalize(stmt);

    std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_), "Renamed to '%s'.", new_name.c_str());
    socket_vocab_need_fetch_ = true;
}

void SettingsTab::SocketDelete(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM socket_vocabulary WHERE id = ?",
        -1, &stmt, nullptr) != SQLITE_OK) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Delete error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_),
                      "Delete error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_finalize(stmt);

    std::snprintf(socket_vocab_status_, sizeof(socket_vocab_status_), "Deleted.");
    socket_vocab_need_fetch_ = true;
}

void SettingsTab::DrawSocketVocab(sqlite3* db) {
    if (socket_vocab_need_fetch_) {
        LoadSocketVocab(db);
        socket_vocab_need_fetch_ = false;
    }

    ImGui::TextWrapped(
        "Flat list of attachment socket names. Sockets are mapped to bones "
        "per actor in B3. Items select a socket in B4.");

    if (ImGui::Button("Refresh")) {
        socket_vocab_need_fetch_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", socket_vocab_status_);
    ImGui::Separator();

    ImGui::PushItemWidth(200);
    ImGui::InputText("##new_socket_name", socket_vocab_new_name_, sizeof(socket_vocab_new_name_));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("+ Add Socket")) {
        SocketAdd(db, socket_vocab_new_name_);
        socket_vocab_new_name_[0] = '\0';
    }

    ImGui::Separator();

    for (const auto& e : socket_vocab_) {
        ImGui::PushID(e.id);

        ImGui::Text("%s", e.name.c_str());
        ImGui::SameLine();

        if (ImGui::SmallButton("Rename")) {
            socket_vocab_rename_id_ = e.id;
            std::snprintf(socket_vocab_rename_buf_, sizeof(socket_vocab_rename_buf_), "%s", e.name.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            SocketDelete(db, e.id);
        }

        if (socket_vocab_rename_id_ == e.id) {
            ImGui::PushItemWidth(200);
            ImGui::InputText("##rename_socket", socket_vocab_rename_buf_, sizeof(socket_vocab_rename_buf_));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::SmallButton("Save##rsock")) {
                SocketRename(db, e.id, socket_vocab_rename_buf_);
                socket_vocab_rename_id_ = 0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##rsock")) {
                socket_vocab_rename_id_ = 0;
            }
        }

        ImGui::PopID();
    }
}

} // namespace gue
