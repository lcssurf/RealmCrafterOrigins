#include "chat.h"
#include <imgui.h>
#include <cstring>

namespace rco::ui {

void Chat::AddMessage(const std::string& sender, const std::string& text) {
    Message m;
    m.sender    = sender;
    m.text      = text;
    m.timestamp = static_cast<float>(ImGui::GetTime());
    messages_.push_back(std::move(m));
    if (static_cast<int>(messages_.size()) > kMaxMessages)
        messages_.pop_front();
}

bool Chat::PollSend(std::string& out_text) {
    if (!pending_send_) return false;
    pending_send_ = false;
    out_text      = pending_text_;
    pending_text_.clear();
    return true;
}

void Chat::Render(int screenW, int screenH, float now) {
    if (!visible) return;

    constexpr float kW       = 480.f;
    constexpr float kLogH    = 160.f;
    constexpr float kInputH  = 28.f;
    constexpr float kPadB    = 8.f;
    constexpr float kFadeAge = 8.f;   // seconds before a message starts fading
    constexpr float kDeadAge = 12.f;  // fully transparent after this

    const float wx = 8.f;
    const float wy = static_cast<float>(screenH) - kLogH - kInputH - kPadB - 4.f;

    ImGuiWindowFlags log_flags =
        ImGuiWindowFlags_NoDecoration   |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs       |
        ImGuiWindowFlags_NoNav          |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    // ---- Message log ----
    ImGui::SetNextWindowPos({wx, wy}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kW, kLogH}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##chatlog", nullptr, log_flags);

    for (const auto& msg : messages_) {
        float age   = now - msg.timestamp;
        float alpha = 1.f;
        if (!focus_input_) {
            if (age > kDeadAge)       alpha = 0.f;
            else if (age > kFadeAge)  alpha = 1.f - (age - kFadeAge) / (kDeadAge - kFadeAge);
        }
        if (alpha <= 0.f) continue;

        ImGui::PushStyleColor(ImGuiCol_Text,
            msg.sender.empty()
                ? ImVec4{0.75f, 0.75f, 0.75f, alpha}   // system message
                : ImVec4{1.f,   1.f,   1.f,   alpha});

        if (msg.sender.empty())
            ImGui::TextWrapped("%s", msg.text.c_str());
        else
            ImGui::TextWrapped("[%s]: %s", msg.sender.c_str(), msg.text.c_str());

        ImGui::PopStyleColor();
    }

    // Auto-scroll to bottom when input is focused
    if (focus_input_)
        ImGui::SetScrollHereY(1.f);

    ImGui::End();

    // ---- Input box ----
    ImGuiWindowFlags input_flags =
        ImGuiWindowFlags_NoDecoration   |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos({wx, wy + kLogH + 2.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kW, kInputH + 8.f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(focus_input_ ? 0.75f : 0.0f);
    ImGui::Begin("##chatinput", nullptr, input_flags);

    // Press Enter to focus input
    if (!focus_input_ && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        focus_input_ = true;
        ImGui::SetKeyboardFocusHere();
    }

    if (focus_input_) {
        ImGui::SetNextItemWidth(kW - 16.f);

        ImGuiInputTextFlags tf =
            ImGuiInputTextFlags_EnterReturnsTrue;

        if (ImGui::InputText("##chatbox", input_.data(), kInputLen, tf)) {
            std::string txt(input_.data());
            if (!txt.empty()) {
                pending_send_ = true;
                pending_text_ = txt;
            }
            std::memset(input_.data(), 0, kInputLen);
            focus_input_ = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            std::memset(input_.data(), 0, kInputLen);
            focus_input_ = false;
        }
    } else {
        ImGui::TextDisabled("Enter to chat");
    }

    ImGui::End();
}

} // namespace rco::ui
