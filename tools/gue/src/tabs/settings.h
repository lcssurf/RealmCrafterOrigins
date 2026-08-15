#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>
#include <utility>

namespace rco::renderer { class Pipeline; }

namespace gue {

class SettingsTab {
public:
    void Draw(sqlite3* db);

    void SetPipeline(rco::renderer::Pipeline* p) { pipeline_ = p; }

private:
    void EnsureTables(sqlite3* db);
    void LoadModels(sqlite3* db);
    void LoadBloodFXKeys(sqlite3* db);
    void LoadSettings(sqlite3* db);
    bool SaveSettings(sqlite3* db);
    void DrawGeneralSettings(sqlite3* db);

    // Animation Vocabulary sub-tab (Phase A.1)
    struct AnimVocabNode {
        int id;
        std::string name;
        int parent_id;
    };
    void LoadAnimVocabulary(sqlite3* db);
    void DrawAnimVocabulary(sqlite3* db);
    // parent_name: "" for root nodes, else the parent's name — used only to
    // detect the "{parent_name}_{style_key}" weapon-composite naming
    // convention so the dead-style warning (item 5) can be drawn inline.
    void DrawAnimVocabNode(sqlite3* db, const AnimVocabNode& node, const std::string& parent_name);
    bool AnimVocabNameExists(sqlite3* db, const std::string& name);
    void AnimVocabAddNode(sqlite3* db, const std::string& name, int parent_id);
    void AnimVocabRenameNode(sqlite3* db, int id, const std::string& new_name);
    void AnimVocabDeleteNode(sqlite3* db, int id);
    bool AnimVocabHasChildren(int id) const;

    // Weapon-specific binding generator (Option A UX, item 1) — lives in the
    // same Animation Vocabulary sub-tab as the tree above, reuses
    // AnimVocabAddNode/AnimVocabNameExists directly. Produces a node whose
    // name is the composed "{base}_{style_key}" and whose parent is the
    // chosen base action, so it shows up nested under it via the SAME
    // recursive draw the tree already does (no new grouping mechanism — see
    // item 2).
    //
    // Source is weapon_anim_styles (PHYSICAL grip/pose archetype: sword_1h,
    // staff, bow...) — NOT weapon_kits (skill pool). Corrected from an
    // earlier iteration of this form that read weapon_kits.kit_key, which
    // was the wrong granularity: many differently-kitted weapons (different
    // skills, different stats) can share one grip/pose and should map to the
    // SAME composite binding. See docs/TECH_DEBT.md for the "weapon_type
    // mixed empunhadura+dimensão" precedent this repeats if kit_key is used.
    void LoadWeaponAnimStyleKeysForVocab(sqlite3* db);
    void DrawAddWeaponBindingForm(sqlite3* db);

    rco::renderer::Pipeline* pipeline_ = nullptr;

    std::vector<std::pair<int, std::string>> media_models_;
    std::vector<std::string> blood_fx_keys_;
    int   default_drop_model_id_    = 0;
    float default_drop_model_scale_ = 1.f;
    float black_cutout_threshold_   = 0.005f;
    std::string blood_fx_key_;
    std::string blood_fx_mode_ = "basic";
    bool need_fetch_ = true;
    bool dirty_ = false;
    char status_msg_[256] = {};

    std::vector<AnimVocabNode> anim_vocab_;
    bool anim_vocab_need_fetch_ = true;
    char anim_vocab_status_[256] = {};
    char anim_vocab_new_root_name_[64] = {};
    char anim_vocab_rename_buf_[64] = {};
    int  anim_vocab_rename_id_ = 0;

    // "Add Weapon-Specific Binding" form state (session-only).
    std::vector<std::string> vocab_weapon_anim_style_keys_;
    bool vocab_weapon_anim_style_keys_need_fetch_ = true;
    // Selected base-action node ids (multi-select — batch creation, item 2).
    // Keyed by node id (not list index) so selection survives the tree being
    // re-sorted/re-fetched between frames.
    std::vector<int> wsb_base_selected_ids_;
    int  wsb_style_idx_ = -1;
    char wsb_status_[256] = {};

    // Socket Vocabulary sub-tab (Arco B / B2)
    struct SocketEntry {
        int id;
        std::string name;
    };
    void LoadSocketVocab(sqlite3* db);
    void DrawSocketVocab(sqlite3* db);
    bool SocketNameExists(const std::string& name) const;
    void SocketAdd(sqlite3* db, const std::string& name);
    void SocketRename(sqlite3* db, int id, const std::string& new_name);
    void SocketDelete(sqlite3* db, int id);

    std::vector<SocketEntry> socket_vocab_;
    bool socket_vocab_need_fetch_ = true;
    char socket_vocab_status_[256] = {};
    char socket_vocab_new_name_[64] = {};
    char socket_vocab_rename_buf_[64] = {};
    int  socket_vocab_rename_id_ = 0;
};

} // namespace gue
