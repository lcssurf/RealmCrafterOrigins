#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct InputPreset {
    int         id         = 0;
    std::string name;
    std::string description;
    bool        is_default = false;
};

struct InputBinding {
    int         id           = 0;
    int         preset_id    = 0;
    std::string context      = "gameplay";
    std::string key;
    std::string modifier;
    std::string trigger_type = "press";
    std::string action;
    float       axis_value   = 1.f;
    bool        enabled      = true;
    bool        remappable   = true;
};

class InputMapsTab {
public:
    void Draw(sqlite3* db);
    void EnsureTables(sqlite3* db);

private:
    void FetchAll   (sqlite3* db);
    void FetchBindings(sqlite3* db, int preset_id);
    void SavePreset (sqlite3* db, InputPreset& p);
    void DeletePreset(sqlite3* db, int id);
    void SaveBinding(sqlite3* db, InputBinding& b);
    void DeleteBinding(sqlite3* db, int id);

    // Key capture UI — renders either a "Press a key..." indicator or a button
    // that enters capture mode. Updates out_key and out_modifier when a key is
    // captured. Returns true when a key was just captured.
    bool DrawKeyCapture(const char* btn_label, std::string& out_key,
                        std::string& out_modifier);

    // Build a list of unique action strings currently present in all actor
    // defs, plus a hardcoded set of universal UI actions.
    void BuildActionSuggestions(sqlite3* db);

    std::vector<InputPreset>  presets_;
    std::vector<InputBinding> bindings_;  // bindings for the selected preset

    int         sel_preset_      = -1;
    int         edit_preset_idx_ = -1;
    InputPreset edit_preset_;
    bool        dirty_preset_    = false;
    bool        new_preset_      = false;

    // Per-binding editing
    std::string  filter_context_ = "gameplay";
    int          sel_binding_    = -1;
    InputBinding edit_binding_;
    bool         dirty_binding_  = false;
    bool         new_binding_    = false;

    // Key capture state
    bool         capturing_key_ = false;
    int          capturing_row_ = -1;  // binding index being captured (-1 = edit_binding_)

    // Action suggestions (rebuilt lazily when preset changes)
    std::vector<std::string> action_suggestions_;
    bool                     suggestions_dirty_ = true;

    bool needs_fetch_ = true;
    char status_msg_[256] = {};
};

} // namespace gue
