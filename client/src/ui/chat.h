#pragma once
#include <string>
#include <deque>
#include <array>

namespace rco::ui {

class Chat {
public:
    static constexpr int kMaxMessages = 100;
    static constexpr int kInputLen    = 256;

    struct Message {
        std::string sender;
        std::string text;
        float       timestamp = 0.f;  // ImGui time when received
    };

    void AddMessage(const std::string& sender, const std::string& text);
    void Render(int screenW, int screenH, float now);

    // Returns true and clears the buffer when the user hits Enter with text.
    bool PollSend(std::string& out_text);

    bool visible = true;

private:
    std::deque<Message>  messages_;
    std::array<char, kInputLen> input_ = {};
    bool                 focus_input_  = false;
    bool                 pending_send_ = false;
    std::string          pending_text_;
};

} // namespace rco::ui
