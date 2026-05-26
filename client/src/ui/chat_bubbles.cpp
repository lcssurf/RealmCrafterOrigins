#include "chat_bubbles.h"

namespace rco::ui {

void ChatBubbles::Add(uint32_t actor_rid, float y_offset, const std::string& text, float now) {
    bubbles_.push_back({actor_rid, y_offset, text, now});
}

} // namespace rco::ui
