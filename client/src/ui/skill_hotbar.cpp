#include "skill_hotbar.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

namespace rco::ui {

namespace {

std::vector<std::string> WrapTextForTooltip(const std::string& text, float max_width_px) {
    std::vector<std::string> out;
    if (text.empty()) return out;

    std::string current;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) break;

        std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        const std::string word = text.substr(start, i - start);
        const std::string candidate = current.empty() ? word : (current + " " + word);
        if (current.empty() || ImGui::CalcTextSize(candidate.c_str()).x <= max_width_px) {
            current = candidate;
        } else {
            out.push_back(current);
            current = word;
        }
    }

    if (!current.empty()) {
        out.push_back(current);
    }
    if (out.empty()) {
        out.push_back(text);
    }

    constexpr std::size_t kMaxLines = 4;
    if (out.size() > kMaxLines) {
        out.resize(kMaxLines);
        if (out.back().size() > 3) {
            out.back() = out.back().substr(0, out.back().size() - 3) + "...";
        }
    }
    return out;
}

} // namespace

void SkillHotbar::OnSkillStateUpdated(const gameplay::SkillState& state) {
    int max_slot = -1;
    for (const auto& ab : state.abilities()) {
        max_slot = std::max(max_slot, static_cast<int>(ab.slot_index));
    }

    int slot_count = 4;
    if (state.has_kit()) {
        slot_count = std::max(4, max_slot + 1);
    }
    if (slot_count < 1) slot_count = 1;

    slot_cooldowns_.assign(static_cast<std::size_t>(slot_count), SlotCooldownState{});
    std::vector<const gameplay::SkillStateAbility*> by_slot(static_cast<std::size_t>(slot_count), nullptr);
    for (const auto& ab : state.abilities()) {
        const int idx = static_cast<int>(ab.slot_index);
        if (idx >= 0 && idx < slot_count) {
            by_slot[static_cast<std::size_t>(idx)] = &ab;
        }
    }

    const int64_t now_ms = static_cast<int64_t>(ImGui::GetTime() * 1000.0);
    for (int i = 0; i < slot_count; ++i) {
        auto& cd = slot_cooldowns_[static_cast<std::size_t>(i)];
        const auto* ab = by_slot[static_cast<std::size_t>(i)];
        if (ab == nullptr || ab->cooldown_remaining_ms == 0) {
            cd = SlotCooldownState{};
            continue;
        }
        cd.cooldown_total_ms_at_receipt = ab->cooldown_remaining_ms;
        cd.cooldown_set_at_ms = now_ms;
    }
}

uint32_t SkillHotbar::ComputeLocalRemainingMs(int slot_index, int64_t now_ms) {
    if (slot_index < 0 || static_cast<std::size_t>(slot_index) >= slot_cooldowns_.size()) {
        return 0;
    }
    auto& cd = slot_cooldowns_[static_cast<std::size_t>(slot_index)];
    if (cd.cooldown_total_ms_at_receipt == 0 || cd.cooldown_set_at_ms <= 0) {
        return 0;
    }
    const int64_t elapsed = (std::max)(int64_t{0}, now_ms - cd.cooldown_set_at_ms);
    if (elapsed >= static_cast<int64_t>(cd.cooldown_total_ms_at_receipt)) {
        cd = SlotCooldownState{};
        return 0;
    }
    return static_cast<uint32_t>(static_cast<int64_t>(cd.cooldown_total_ms_at_receipt) - elapsed);
}

void SkillHotbar::Render(int screen_w, int screen_h) {
    const auto& state = gameplay::PlayerSkillState();

    int max_slot = -1;
    for (const auto& ab : state.abilities()) {
        max_slot = std::max(max_slot, static_cast<int>(ab.slot_index));
    }

    // Current packet does not carry explicit "hotbar slots granted" yet.
    // Heuristic for this phase:
    // - active kit: show at least 4 slots, or grow to max received slot index.
    // - no active kit: show empty placeholders (4 slots).
    int slot_count = 4;
    if (state.has_kit()) {
        slot_count = std::max(4, max_slot + 1);
    }
    if (slot_count < 1) slot_count = 1;

    if (slot_cooldowns_.size() < static_cast<std::size_t>(slot_count)) {
        slot_cooldowns_.resize(static_cast<std::size_t>(slot_count));
    }

    constexpr float kSlotW = 60.f;
    constexpr float kSlotH = 60.f;
    constexpr float kGap = 4.f;
    const float total_w = slot_count * kSlotW + (slot_count - 1) * kGap;
    const float ox = (static_cast<float>(screen_w) - total_w) * 0.5f;
    const float oy = static_cast<float>(screen_h) - kSlotH - 96.f; // above legacy SpellBar

    ImGui::SetNextWindowPos({ox - 8.f, oy - 8.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({total_w + 16.f, kSlotH + 16.f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##skill_hotbar", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs);

    std::vector<const gameplay::SkillStateAbility*> by_slot(static_cast<std::size_t>(slot_count), nullptr);
    for (const auto& ab : state.abilities()) {
        const int idx = static_cast<int>(ab.slot_index);
        if (idx >= 0 && idx < slot_count) {
            by_slot[static_cast<std::size_t>(idx)] = &ab;
        }
    }

    struct SlotRect {
        ImVec2 top_left;
        ImVec2 size;
        const gameplay::SkillStateAbility* ability = nullptr;
    };
    std::vector<SlotRect> slot_rects;
    slot_rects.reserve(static_cast<std::size_t>(slot_count));
    const int64_t now_ms = static_cast<int64_t>(ImGui::GetTime() * 1000.0);

    for (int i = 0; i < slot_count; ++i) {
        const float x0 = ox + i * (kSlotW + kGap);
        const float y0 = oy;
        const auto* ability = by_slot[static_cast<std::size_t>(i)];
        if (by_slot[static_cast<std::size_t>(i)] != nullptr) {
            const uint32_t remaining_local_ms = ComputeLocalRemainingMs(i, now_ms);
            uint32_t cooldown_total_ms_at_receipt = 0;
            if (static_cast<std::size_t>(i) < slot_cooldowns_.size()) {
                cooldown_total_ms_at_receipt = slot_cooldowns_[static_cast<std::size_t>(i)].cooldown_total_ms_at_receipt;
            }
            RenderAbilitySlot(i, *ability, remaining_local_ms, cooldown_total_ms_at_receipt,
                              x0, y0, kSlotW, kSlotH);
        } else {
            if (static_cast<std::size_t>(i) < slot_cooldowns_.size()) {
                slot_cooldowns_[static_cast<std::size_t>(i)] = SlotCooldownState{};
            }
            RenderEmptySlot(i, x0, y0, kSlotW, kSlotH);
        }
        slot_rects.push_back(SlotRect{
            ImVec2{x0, y0},
            ImVec2{kSlotW, kSlotH},
            ability,
        });
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    for (const auto& rect : slot_rects) {
        if (rect.ability == nullptr) continue;

        const bool inside =
            mouse.x >= rect.top_left.x &&
            mouse.x <= rect.top_left.x + rect.size.x &&
            mouse.y >= rect.top_left.y &&
            mouse.y <= rect.top_left.y + rect.size.y;
        if (!inside) continue;

        DrawSkillTooltip(*rect.ability, mouse);
        break;
    }

    ImGui::End();
}

void SkillHotbar::RenderEmptySlot(int slot_index, float x0, float y0, float slot_w, float slot_h) const {
    auto* dl = ImGui::GetWindowDrawList();
    const float x1 = x0 + slot_w;
    const float y1 = y0 + slot_h;
    const ImVec2 p0{x0, y0};
    const ImVec2 p1{x1, y1};

    dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 24, 200), 5.f);
    dl->AddRect(p0, p1, IM_COL32(70, 70, 85, 180), 5.f, 0, 1.5f);

    char hk[4];
    std::snprintf(hk, sizeof(hk), "%d", slot_index + 1);
    dl->AddText({x0 + 4.f, y1 - 15.f}, IM_COL32(140, 140, 150, 180), hk);
}

void SkillHotbar::RenderAbilitySlot(int slot_index, const gameplay::SkillStateAbility& ability,
                                    uint32_t cooldown_remaining_local_ms,
                                    uint32_t cooldown_total_ms_at_receipt,
                                    float x0, float y0, float slot_w, float slot_h) const {
    auto* dl = ImGui::GetWindowDrawList();
    const float x1 = x0 + slot_w;
    const float y1 = y0 + slot_h;
    const ImVec2 p0{x0, y0};
    const ImVec2 p1{x1, y1};

    dl->AddRectFilled(p0, p1, IM_COL32(20, 28, 52, 220), 5.f);
    dl->AddRect(p0, p1, IM_COL32(100, 140, 255, 210), 5.f, 0, 1.8f);

    char abbr[12];
    std::snprintf(abbr, sizeof(abbr), "%.10s", ability.ability_name.c_str());
    ImVec2 ts = ImGui::CalcTextSize(abbr);
    dl->AddText({x0 + (slot_w - ts.x) * 0.5f, y0 + 6.f}, IM_COL32(240, 240, 240, 220), abbr);

    if (cooldown_remaining_local_ms > 0 && cooldown_total_ms_at_receipt > 0) {
        const float ratio = std::clamp(
            static_cast<float>(cooldown_remaining_local_ms) / static_cast<float>(cooldown_total_ms_at_receipt),
            0.f, 1.f);
        dl->AddRectFilled(p0, {x1, y0 + slot_h * ratio}, IM_COL32(0, 0, 0, 150), 5.f);

        const float seconds_left = static_cast<float>(cooldown_remaining_local_ms) / 1000.0f;
        char cd_text[16];
        if (seconds_left >= 1.0f) {
            std::snprintf(cd_text, sizeof(cd_text), "%.0f", std::ceil(seconds_left));
        } else {
            std::snprintf(cd_text, sizeof(cd_text), "%.1f", seconds_left);
        }
        const ImVec2 cd_size = ImGui::CalcTextSize(cd_text);
        const ImVec2 cd_pos{
            x0 + (slot_w - cd_size.x) * 0.5f,
            y0 + (slot_h - cd_size.y) * 0.5f,
        };
        dl->AddText(cd_pos, IM_COL32(255, 255, 255, 240), cd_text);
    }

    char hk[4];
    std::snprintf(hk, sizeof(hk), "%d", slot_index + 1);
    dl->AddText({x0 + 4.f, y1 - 15.f}, IM_COL32(180, 180, 190, 210), hk);
}

void SkillHotbar::DrawSkillTooltip(const gameplay::SkillStateAbility& ability, ImVec2 mouse_pos) const {
    auto* dl = ImGui::GetForegroundDrawList();

    std::vector<std::pair<std::string, ImU32>> lines;
    lines.reserve(8);

    char line1[128];
    if (ability.mastery_level > 0) {
        std::snprintf(line1, sizeof(line1), "%s (Lv %u)",
                      ability.ability_name.c_str(),
                      static_cast<unsigned>(ability.mastery_level));
    } else {
        std::snprintf(line1, sizeof(line1), "%s", ability.ability_name.c_str());
    }
    lines.push_back({line1, IM_COL32(255, 255, 255, 255)});

    const auto desc_lines = WrapTextForTooltip(ability.description, 340.0f);
    for (const auto& d : desc_lines) {
        lines.push_back({d, IM_COL32(200, 210, 230, 255)});
    }

    char cooldown_line[64];
    std::snprintf(cooldown_line, sizeof(cooldown_line), "Cooldown: %u ms", ability.cooldown_ms);
    lines.push_back({cooldown_line, IM_COL32(180, 180, 180, 255)});

    char mastery_line[96];
    if (ability.mastery_level > 0 && ability.mastery_max_level > 0) {
        if (ability.mastery_level >= ability.mastery_max_level) {
            std::snprintf(mastery_line, sizeof(mastery_line), "MAX LEVEL");
            lines.push_back({mastery_line, IM_COL32(255, 215, 0, 255)});
        } else {
            std::snprintf(mastery_line, sizeof(mastery_line), "XP: %u / %u",
                          ability.mastery_xp, ability.mastery_xp_for_next);
            lines.push_back({mastery_line, IM_COL32(150, 200, 150, 255)});
        }
    }

    constexpr float kPadding = 6.0f;
    float tooltip_w = 0.0f;
    float text_h_total = 0.0f;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const ImVec2 size = ImGui::CalcTextSize(lines[i].first.c_str());
        tooltip_w = std::max(tooltip_w, size.x);
        text_h_total += size.y;
        if (i + 1 < lines.size()) {
            text_h_total += kPadding;
        }
    }
    tooltip_w += kPadding * 2.0f;
    const float tooltip_h = text_h_total + kPadding * 2.0f;

    ImVec2 pos(mouse_pos.x + 14.0f, mouse_pos.y - tooltip_h - 8.0f);
    const ImGuiIO& io = ImGui::GetIO();
    if (pos.y < 0.0f) pos.y = mouse_pos.y + 18.0f;
    if (pos.x + tooltip_w > io.DisplaySize.x) pos.x = io.DisplaySize.x - tooltip_w - 4.0f;
    if (pos.x < 0.0f) pos.x = 0.0f;

    const ImVec2 p1(pos.x + tooltip_w, pos.y + tooltip_h);
    dl->AddRectFilled(pos, p1, IM_COL32(20, 20, 25, 230), 4.0f);
    dl->AddRect(pos, p1, IM_COL32(80, 80, 90, 255), 4.0f);

    float y = pos.y + kPadding;
    for (const auto& line : lines) {
        dl->AddText(ImVec2(pos.x + kPadding, y), line.second, line.first.c_str());
        y += ImGui::CalcTextSize(line.first.c_str()).y + kPadding;
    }
}

} // namespace rco::ui
