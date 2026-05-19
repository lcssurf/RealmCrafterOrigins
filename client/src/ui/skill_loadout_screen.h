#pragma once

#include <cstdint>
#include <functional>

namespace rco { namespace ui {

class SkillLoadoutScreen {
public:
    std::function<void(uint32_t kit_id, uint8_t slot_index, uint32_t ability_id)> on_set_slot;
    std::function<void(uint32_t kit_id, uint8_t slot_index)> on_clear_slot;
    std::function<void(uint32_t kit_id)> on_clear_kit;

    void Toggle() { open_ = !open_; }
    void SetOpen(bool v) { open_ = v; }
    bool IsOpen() const { return open_; }

    void Render(int screen_w, int screen_h);

private:
    bool open_ = false;
    char status_msg_[256] = {};
};

}} // namespace rco::ui

