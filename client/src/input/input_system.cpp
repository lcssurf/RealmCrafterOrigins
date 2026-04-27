#include "input_system.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

// PPlayerAction packet ID
static constexpr uint16_t kPPlayerAction = 119;

namespace rco::input {

// ---------------------------------------------------------------------------
// Helpers — minimal JSON parsing / writing
// ---------------------------------------------------------------------------

// Extract the string value for a key inside a JSON object literal.
// Returns "" if not found. Handles "key": "value" patterns only.
static std::string jsonGetString(const std::string& obj, const std::string& key) {
    // Look for "key":
    std::string needle = "\"" + key + "\"";
    auto pos = obj.find(needle);
    if (pos == std::string::npos) return {};
    pos = obj.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = obj.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    auto end = obj.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return obj.substr(pos + 1, end - pos - 1);
}

// Escape a string for JSON output (handles backslash and double-quote only;
// sufficient for file paths, key names, and context strings in this engine).
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if      (c == '\\') out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else                out += c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// InputSystem implementation
// ---------------------------------------------------------------------------

void InputSystem::LoadBindings(const std::vector<InputBinding>& bindings) {
    all_bindings_      = bindings;
    original_bindings_ = bindings;
    RebuildLookupTables();
}

void InputSystem::RebuildLookupTables() {
    by_context_.clear();
    for (const auto& b : all_bindings_) {
        ChordKey ck{b.key, b.modifier, b.trigger_type};
        by_context_[b.context][ck] = b;
    }
}

void InputSystem::SetContext(const std::string& context) {
    current_context_ = context;
}

void InputSystem::OnAction(ActionHandler h) {
    handlers_.push_back(std::move(h));
}

void InputSystem::Dispatch(const std::string& action, uint8_t state, float axis) {
    // Build PPlayerAction packet:
    //   [u16 type LE][u32 payloadLen LE][u16 action_len][action bytes][u8 state][f32 axis]
    uint16_t action_len   = static_cast<uint16_t>(action.size());
    uint32_t payload_size = static_cast<uint32_t>(2 + action.size() + 1 + 4);
    size_t   total        = 6 + payload_size;

    std::vector<uint8_t> buf(total);

    // Header: type (LE)
    buf[0] = static_cast<uint8_t>(kPPlayerAction & 0xFF);
    buf[1] = static_cast<uint8_t>((kPPlayerAction >> 8) & 0xFF);
    // Header: length (LE)
    buf[2] = static_cast<uint8_t>(payload_size & 0xFF);
    buf[3] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
    buf[4] = static_cast<uint8_t>((payload_size >> 16) & 0xFF);
    buf[5] = static_cast<uint8_t>((payload_size >> 24) & 0xFF);

    // Payload: action string (u16 len + bytes)
    size_t off = 6;
    buf[off++] = static_cast<uint8_t>(action_len & 0xFF);
    buf[off++] = static_cast<uint8_t>((action_len >> 8) & 0xFF);
    std::memcpy(buf.data() + off, action.data(), action.size());
    off += action.size();

    // state (u8)
    buf[off++] = state;

    // axis (f32, IEEE 754 LE)
    uint32_t axis_bits;
    std::memcpy(&axis_bits, &axis, 4);
    buf[off++] = static_cast<uint8_t>(axis_bits & 0xFF);
    buf[off++] = static_cast<uint8_t>((axis_bits >> 8) & 0xFF);
    buf[off++] = static_cast<uint8_t>((axis_bits >> 16) & 0xFF);
    buf[off++] = static_cast<uint8_t>((axis_bits >> 24) & 0xFF);

    send_(buf.data(), buf.size());

    // Also call local handlers
    for (auto& h : handlers_) h(action, state, axis);
}

void InputSystem::OnKeyDown(const std::string& key, const std::string& mod) {
    auto ctx_it = by_context_.find(current_context_);
    if (ctx_it == by_context_.end()) return;
    auto& ctx_map = ctx_it->second;

    // Press
    {
        ChordKey ck{key, mod, TriggerType::Press};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end())
            Dispatch(it->second.action, 0, 0.f);
    }

    // Hold start
    {
        ChordKey ck{key, mod, TriggerType::Hold};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end()) {
            hold_active_.insert(it->second.action);
            Dispatch(it->second.action, 1, 0.f);
        }
    }

    // Double
    {
        ChordKey ck{key, mod, TriggerType::Double};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end()) {
            double now    = TimeNow();
            std::string lpkey = key + "+" + mod;
            auto lpit = last_press_time_.find(lpkey);
            if (lpit != last_press_time_.end() && now - lpit->second < 0.3) {
                Dispatch(it->second.action, 0, 0.f);
                last_press_time_.erase(lpit);
            } else {
                last_press_time_[lpkey] = now;
            }
        }
    }

    // Axis
    {
        ChordKey ck{key, mod, TriggerType::Axis};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end())
            Dispatch(it->second.action, 0, it->second.axis_value);
    }
}

void InputSystem::OnKeyUp(const std::string& key, const std::string& mod) {
    auto ctx_it = by_context_.find(current_context_);
    if (ctx_it == by_context_.end()) return;
    auto& ctx_map = ctx_it->second;

    // Release
    {
        ChordKey ck{key, mod, TriggerType::Release};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end())
            Dispatch(it->second.action, 0, 0.f);
    }

    // Hold end
    {
        ChordKey ck{key, mod, TriggerType::Hold};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end()) {
            hold_active_.erase(it->second.action);
            Dispatch(it->second.action, 2, 0.f);
        }
    }

    // Axis returns to zero
    {
        ChordKey ck{key, mod, TriggerType::Axis};
        auto it = ctx_map.find(ck);
        if (it != ctx_map.end())
            Dispatch(it->second.action, 0, 0.f);
    }
}

// ---------------------------------------------------------------------------
// GetBindings
// ---------------------------------------------------------------------------
std::vector<InputBinding> InputSystem::GetBindings(const std::string& context) const {
    std::vector<InputBinding> result;
    for (const auto& b : all_bindings_) {
        if (b.context == context)
            result.push_back(b);
    }
    std::sort(result.begin(), result.end(),
              [](const InputBinding& a, const InputBinding& b) {
                  return a.action < b.action;
              });
    return result;
}

// ---------------------------------------------------------------------------
// Rebind
// ---------------------------------------------------------------------------
void InputSystem::Rebind(const std::string& ctx, const std::string& action,
                         const std::string& trigger_str,
                         const std::string& new_key, const std::string& new_mod) {
    TriggerType tt = TriggerType::Press;
    if      (trigger_str == "release") tt = TriggerType::Release;
    else if (trigger_str == "hold")    tt = TriggerType::Hold;
    else if (trigger_str == "double")  tt = TriggerType::Double;
    else if (trigger_str == "axis")    tt = TriggerType::Axis;

    for (auto& b : all_bindings_) {
        if (b.context == ctx && b.action == action && b.trigger_type == tt) {
            // Remove old chord from lookup
            auto ctx_it = by_context_.find(ctx);
            if (ctx_it != by_context_.end()) {
                ChordKey old_ck{b.key, b.modifier, tt};
                ctx_it->second.erase(old_ck);
            }
            // Update binding
            b.key      = new_key;
            b.modifier = new_mod;
            // Insert under new chord
            ChordKey new_ck{new_key, new_mod, tt};
            by_context_[ctx][new_ck] = b;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ResetToDefault
// ---------------------------------------------------------------------------
void InputSystem::ResetToDefault() {
    all_bindings_ = original_bindings_;
    RebuildLookupTables();
}

// ---------------------------------------------------------------------------
// LoadLocalOverrides
// ---------------------------------------------------------------------------
// JSON format:
// {
//   "preset_base": "Default",
//   "overrides": [
//     { "context": "gameplay", "action": "Jump", "trigger": "press",
//       "key": "F", "modifier": "" }
//   ]
// }
//
// Security: only existing (context, action, trigger_type) pairs are modified.
// Unknown actions are silently skipped.
void InputSystem::LoadLocalOverrides(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return; // no local file yet — silently ignore

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();

    // Find the "overrides" array.
    auto arrStart = src.find("\"overrides\"");
    if (arrStart == std::string::npos) return;
    arrStart = src.find('[', arrStart);
    if (arrStart == std::string::npos) return;
    auto arrEnd = src.find(']', arrStart);
    if (arrEnd == std::string::npos) return;

    // Iterate over objects { ... } inside the array.
    bool modified = false;
    size_t pos = arrStart + 1;
    while (pos < arrEnd) {
        auto objStart = src.find('{', pos);
        if (objStart == std::string::npos || objStart >= arrEnd) break;
        auto objEnd = src.find('}', objStart);
        if (objEnd == std::string::npos || objEnd > arrEnd) break;

        std::string obj = src.substr(objStart + 1, objEnd - objStart - 1);

        std::string ctx     = jsonGetString(obj, "context");
        std::string action  = jsonGetString(obj, "action");
        std::string trigger = jsonGetString(obj, "trigger");
        std::string new_key = jsonGetString(obj, "key");
        std::string new_mod = jsonGetString(obj, "modifier");

        if (!ctx.empty() && !action.empty() && !new_key.empty()) {
            // Resolve trigger string → TriggerType
            TriggerType tt = TriggerType::Press;
            if      (trigger == "release") tt = TriggerType::Release;
            else if (trigger == "hold")    tt = TriggerType::Hold;
            else if (trigger == "double")  tt = TriggerType::Double;
            else if (trigger == "axis")    tt = TriggerType::Axis;

            // Find matching entry in all_bindings_ by (context, action, trigger_type)
            for (auto& b : all_bindings_) {
                if (b.context == ctx && b.action == action && b.trigger_type == tt) {
                    b.key      = new_key;
                    b.modifier = new_mod;
                    modified = true;
                    break;
                }
            }
        }

        pos = objEnd + 1;
    }

    if (modified) {
        RebuildLookupTables();
    }
}

// ---------------------------------------------------------------------------
// SaveLocalOverrides
// ---------------------------------------------------------------------------
// Writes only the bindings where (key, modifier) differ from original_bindings_.
void InputSystem::SaveLocalOverrides(const std::string& path) const {
    // Build diff list
    struct Override {
        std::string context, action, trigger, key, modifier;
    };
    std::vector<Override> diffs;

    for (size_t i = 0; i < all_bindings_.size() && i < original_bindings_.size(); ++i) {
        const auto& cur = all_bindings_[i];
        const auto& org = original_bindings_[i];
        if (cur.key != org.key || cur.modifier != org.modifier) {
            std::string trigger;
            switch (cur.trigger_type) {
                case TriggerType::Press:   trigger = "press";   break;
                case TriggerType::Release: trigger = "release"; break;
                case TriggerType::Hold:    trigger = "hold";    break;
                case TriggerType::Double:  trigger = "double";  break;
                case TriggerType::Axis:    trigger = "axis";    break;
            }
            diffs.push_back({cur.context, cur.action, trigger, cur.key, cur.modifier});
        }
    }

    // Ensure the directory exists before writing
    {
        std::filesystem::path p(path);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path());
    }

    // Always write the file (even if empty diffs — clears stale overrides)
    std::ofstream f(path);
    if (!f.is_open()) return;

    f << "{\n";
    f << "  \"preset_base\": \"Default\",\n";
    f << "  \"overrides\": [\n";
    for (size_t i = 0; i < diffs.size(); ++i) {
        const auto& d = diffs[i];
        f << "    { \"context\": \""  << jsonEscape(d.context)
          << "\", \"action\": \""     << jsonEscape(d.action)
          << "\", \"trigger\": \""    << jsonEscape(d.trigger)
          << "\", \"key\": \""        << jsonEscape(d.key)
          << "\", \"modifier\": \""   << jsonEscape(d.modifier) << "\" }";
        if (i + 1 < diffs.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
}

} // namespace rco::input
