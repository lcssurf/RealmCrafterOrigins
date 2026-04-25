#pragma once

#include <string>
#include <vector>
#include <functional>

#include "game_state.h"  // CharacterInfo

namespace rco::ui {

// ---------------------------------------------------------------------------
// Callbacks fired by CharSelect UI interactions
// ---------------------------------------------------------------------------
struct CharSelectCallbacks {
    // User chose an existing character (slot index).
    std::function<void(int slot)> OnSelect;

    // User submitted the create-character form.
    std::function<void(int slot,
                       const std::string& name,
                       uint16_t actor_def_id,
                       int gender)> OnCreate;

    // User wants to delete the character in the given slot.
    std::function<void(int slot)> OnDelete;

    // User wants to return to the login screen.
    std::function<void()> OnLogout;
};

// ---------------------------------------------------------------------------
// CharSelect — ImGui character selection / creation screen
// ---------------------------------------------------------------------------
class CharSelect {
public:
    explicit CharSelect(CharSelectCallbacks cb);

    // Replace the character list (called after PCharListResult is parsed).
    void SetCharacters(const std::vector<rco::CharacterInfo>& chars);

    // Replace the playable def list (called after PPlayableDefs is parsed).
    void SetPlayableDefs(const std::vector<rco::PlayableDef>& defs);

    // Render the full-screen ImGui panel.  Call every frame while in
    // CharacterSelect state.
    void Render(int screenW, int screenH);

    // Display a localised error message.
    void SetError(const std::string& msg);

private:
    CharSelectCallbacks             cb_;
    std::vector<rco::CharacterInfo> characters_;
    std::vector<rco::PlayableDef>   playable_defs_;

    // Which slot index (0–8) is currently highlighted, or -1 for none.
    int selected_slot_ = -1;

    std::string error_msg_;

    // Create-character form state
    char new_name_[33]{};
    int  new_def_idx_ = 0;
    int  new_gender_  = 0;
    bool show_create_ = false;

    // Slot whose create / delete confirm popup is active (-1 = none).
    int  confirm_delete_slot_ = -1;

    // Returns pointer to CharacterInfo for the given slot, or nullptr.
    const rco::CharacterInfo* FindChar(int slot) const;

    void RenderSlotGrid();
    void RenderCharDetail(int screenW, int screenH);
    void RenderCreateForm(int screenW, int screenH);
};

} // namespace rco::ui
