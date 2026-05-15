#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/codec.h"

namespace rco::ui {

struct PartyMemberEntry {
    uint32_t    runtime_id = 0;
    std::string name;
    uint16_t    level = 1;
    uint16_t    health = 0;
    uint16_t    health_max = 0;
    bool        online = false;
    bool        is_leader = false;
};

class PartyPanel {
public:
    std::function<void(uint8_t action, const std::string& target_name)> on_action;

    bool visible = true;

    void Clear();
    bool ApplyPacket(rco::net::Reader& reader);
    void Render(int screen_w, int screen_h);

private:
    std::vector<PartyMemberEntry> members_;
    uint32_t party_id_ = 0;
    uint32_t leader_rid_ = 0;
    std::string pending_invite_from_;
    uint8_t notice_code_ = 0;
    std::string notice_text_;
    float notice_until_ = 0.f;
    int selected_member_index_ = 0;
    std::array<char, 64> invite_target_input_ = {};
};

} // namespace rco::ui
