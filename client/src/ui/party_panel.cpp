#include "party_panel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "net/protocol.h"

namespace rco::ui {

namespace {
constexpr uint8_t kPartyUpdateModeSnapshot = 0;
constexpr uint8_t kPartyUpdateModeDelta = 1;

void ClampSelectedIndex(int* value, int count) {
    if (!value) return;
    if (count <= 0) {
        *value = 0;
        return;
    }
    if (*value < 0) *value = 0;
    if (*value >= count) *value = count - 1;
}

PartyMemberEntry ReadMemberEntry(rco::net::Reader& reader, uint32_t leader_rid) {
    PartyMemberEntry member;
    member.runtime_id = reader.ReadU32();
    member.name = reader.ReadString();
    member.level = reader.ReadU16();
    member.health = reader.ReadU16();
    member.health_max = reader.ReadU16();
    member.online = reader.ReadBool();
    member.is_leader = (member.runtime_id == leader_rid);
    return member;
}

void SortMembers(std::vector<PartyMemberEntry>* members) {
    if (!members) return;
    std::sort(members->begin(), members->end(),
        [](const PartyMemberEntry& a, const PartyMemberEntry& b) {
            if (a.is_leader != b.is_leader) return a.is_leader > b.is_leader;
            return a.name < b.name;
        });
}
}

void PartyPanel::Clear() {
    members_.clear();
    party_id_ = 0;
    leader_rid_ = 0;
    pending_invite_from_.clear();
    notice_code_ = rco::net::kPartyNoticeNone;
    notice_text_.clear();
    notice_until_ = 0.f;
    selected_member_index_ = 0;
    std::memset(invite_target_input_.data(), 0, invite_target_input_.size());
}

bool PartyPanel::ApplyPacket(rco::net::Reader& reader) {
    const uint8_t mode = reader.ReadU8();
    if (!reader.OK()) return false;

    if (mode == kPartyUpdateModeSnapshot) {
        party_id_ = reader.ReadU32();
        leader_rid_ = reader.ReadU32();
        const uint8_t member_count = reader.ReadU8();
        if (!reader.OK()) return false;

        std::vector<PartyMemberEntry> parsed_members;
        parsed_members.reserve(member_count);
        for (uint8_t i = 0; i < member_count && reader.OK(); ++i) {
            parsed_members.push_back(ReadMemberEntry(reader, leader_rid_));
        }
        if (!reader.OK()) return false;

        pending_invite_from_ = reader.ReadString();
        notice_code_ = reader.ReadU8();
        const std::string notice = reader.ReadString();
        if (!reader.OK()) return false;

        members_ = std::move(parsed_members);
        SortMembers(&members_);
        ClampSelectedIndex(&selected_member_index_, static_cast<int>(members_.size()));

        if (!notice.empty()) {
            notice_text_ = notice;
            notice_until_ = static_cast<float>(ImGui::GetTime()) + 5.0f;
        }
        return true;
    }

    if (mode == kPartyUpdateModeDelta) {
        party_id_ = reader.ReadU32();
        leader_rid_ = reader.ReadU32();
        const uint8_t upsert_count = reader.ReadU8();
        if (!reader.OK()) return false;

        for (uint8_t i = 0; i < upsert_count && reader.OK(); ++i) {
            PartyMemberEntry incoming = ReadMemberEntry(reader, leader_rid_);
            auto it = std::find_if(members_.begin(), members_.end(),
                [&](const PartyMemberEntry& m) { return m.runtime_id == incoming.runtime_id; });
            if (it != members_.end()) {
                *it = incoming;
            } else {
                members_.push_back(std::move(incoming));
            }
        }
        if (!reader.OK()) return false;

        const uint8_t remove_count = reader.ReadU8();
        if (!reader.OK()) return false;
        for (uint8_t i = 0; i < remove_count && reader.OK(); ++i) {
            const uint32_t remove_rid = reader.ReadU32();
            members_.erase(
                std::remove_if(members_.begin(), members_.end(),
                    [&](const PartyMemberEntry& m) { return m.runtime_id == remove_rid; }),
                members_.end());
        }
        if (!reader.OK()) return false;

        pending_invite_from_ = reader.ReadString();
        notice_code_ = reader.ReadU8();
        const std::string notice = reader.ReadString();
        if (!reader.OK()) return false;

        for (auto& member : members_) member.is_leader = (member.runtime_id == leader_rid_);
        SortMembers(&members_);
        ClampSelectedIndex(&selected_member_index_, static_cast<int>(members_.size()));

        if (!notice.empty()) {
            notice_text_ = notice;
            notice_until_ = static_cast<float>(ImGui::GetTime()) + 5.0f;
        }
        return true;
    }

    return false;
}

void PartyPanel::Render(int screen_w, int screen_h) {
    if (!visible) return;

    const float now = static_cast<float>(ImGui::GetTime());
    if (!notice_text_.empty() && now <= notice_until_) {
        const ImVec2 size{540.f, 42.f};
        ImGui::SetNextWindowPos({screen_w * 0.5f - size.x * 0.5f, 16.f}, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.78f);
        ImGui::Begin("Party Notice##party_notice", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoInputs);
        const bool is_error = notice_code_ >= rco::net::kPartyNoticeErrorTargetOffline;
        ImGui::PushStyleColor(ImGuiCol_Text, is_error ? ImVec4(1.f, 0.35f, 0.35f, 1.f)
                                                      : ImVec4(0.90f, 1.f, 0.90f, 1.f));
        ImGui::TextWrapped("%s", notice_text_.c_str());
        ImGui::PopStyleColor();
        ImGui::End();
    }

    ImGui::SetNextWindowPos({14.f, 240.f}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({320.f, 0.f}, ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.84f);
    if (!ImGui::Begin("Party##party_panel", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }

    if (party_id_ == 0) {
        ImGui::TextDisabled("No active party");
    } else {
        ImGui::Text("Party #%u", party_id_);
        ImGui::Separator();

        if (members_.empty()) {
            ImGui::TextDisabled("No members");
        } else {
            for (int i = 0; i < static_cast<int>(members_.size()); ++i) {
                const auto& member = members_[i];
                char row[160];
                std::snprintf(row, sizeof(row), "%s%s  (Lv %d)",
                    member.name.c_str(),
                    member.is_leader ? " [L]" : "",
                    static_cast<int>(member.level));
                if (ImGui::Selectable(row, selected_member_index_ == i)) {
                    selected_member_index_ = i;
                }
                const int max_hp = std::max<int>(1, static_cast<int>(member.health_max));
                const int cur_hp = std::clamp<int>(static_cast<int>(member.health), 0, max_hp);
                const float fill = static_cast<float>(cur_hp) / static_cast<float>(max_hp);
                char hp_label[64];
                std::snprintf(hp_label, sizeof(hp_label), "%d / %d", cur_hp, max_hp);
                ImGui::ProgressBar(fill, {-1.f, 0.f}, hp_label);
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Leave Party", {140.f, 28.f}) && on_action) {
            on_action(rco::net::kPartyActionLeave, "");
        }

        if (!members_.empty() &&
            selected_member_index_ >= 0 &&
            selected_member_index_ < static_cast<int>(members_.size())) {
            const auto& target = members_[selected_member_index_];
            if (!target.is_leader) {
                ImGui::SameLine();
                if (ImGui::Button("Kick", {70.f, 28.f}) && on_action) {
                    on_action(rco::net::kPartyActionKick, target.name);
                }
                ImGui::SameLine();
                if (ImGui::Button("Make Lead", {90.f, 28.f}) && on_action) {
                    on_action(rco::net::kPartyActionTransferLead, target.name);
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Invite");
    ImGui::SetNextItemWidth(190.f);
    ImGui::InputText("##party_invite_name", invite_target_input_.data(), invite_target_input_.size());
    ImGui::SameLine();
    if (ImGui::Button("Invite", {90.f, 0.f}) && on_action) {
        std::string target_name = invite_target_input_.data();
        if (!target_name.empty()) {
            on_action(rco::net::kPartyActionInvite, target_name);
            std::memset(invite_target_input_.data(), 0, invite_target_input_.size());
        }
    }

    if (!pending_invite_from_.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Pending Invite");
        ImGui::TextWrapped("Invite from %s", pending_invite_from_.c_str());
        if (ImGui::Button("Accept Invite", {140.f, 28.f}) && on_action) {
            on_action(rco::net::kPartyActionAccept, "");
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", {90.f, 28.f}) && on_action) {
            on_action(rco::net::kPartyActionDecline, "");
        }
    }

    ImGui::End();
}

} // namespace rco::ui
