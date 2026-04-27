#pragma once
#include <string>
#include "../input/input_system.h"

namespace rco::ui {

// In-game "Configure Controls" window.
// Shows gameplay context bindings only; allows per-action rebind and save.
class ControlsUI {
public:
    void Init(rco::input::InputSystem* input_sys) { input_sys_ = input_sys; }

    void Toggle()              { visible_ = !visible_; }
    bool IsVisible() const     { return visible_; }
    void SetVisible(bool v)    { visible_ = v; }

    // Draw the ImGui window. Call every frame when in-game.
    // player_name is used to build the save path: users/<player_name>/input.json
    void Draw(const std::string& player_name);

private:
    rco::input::InputSystem* input_sys_ = nullptr;
    bool visible_      = false;
    bool capturing_    = false;   // waiting for the next key press
    std::string capture_action_;
    std::string capture_context_;
    std::string capture_trigger_str_;
    char status_[128]  = {};
};

} // namespace rco::ui
