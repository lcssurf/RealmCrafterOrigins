#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>
#include <cstdint>

namespace rco::input {

enum class TriggerType : uint8_t {
    Press = 0, Release = 1, Hold = 2, Double = 3, Axis = 4
};

struct InputBinding {
    std::string  context;
    std::string  key;
    std::string  modifier;
    TriggerType  trigger_type = TriggerType::Press;
    std::string  action;
    float        axis_value  = 1.f;
    bool         remappable  = true;
};

struct ChordKey {
    std::string  key;
    std::string  modifier;
    TriggerType  trigger;
    bool operator==(const ChordKey& o) const {
        return key == o.key && modifier == o.modifier && trigger == o.trigger;
    }
};

struct ChordKeyHash {
    size_t operator()(const ChordKey& c) const noexcept {
        size_t h = std::hash<std::string>{}(c.key);
        h ^= std::hash<std::string>{}(c.modifier) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(c.trigger)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

using ActionHandler = std::function<void(const std::string& action, uint8_t state, float axis)>;

// ── InputSystem ───────────────────────────────────────────────────────────────
//
// Single-threaded input mapping system. Runs on the main thread.
// Maps (context, key, modifier, trigger_type) → action → PPlayerAction packet.
//
// Usage:
//   1. Construct with a SendFn that forwards raw bytes to the server.
//   2. Call LoadBindings() with the action table.
//   3. Wire GLFW key callbacks to OnKeyDown/OnKeyUp.
//   4. Call SetContext() to switch input mode (e.g. "gameplay" → "menu").
//   5. Register local handlers via OnAction() for UI actions.
class InputSystem {
public:
    // send_fn: called with a fully-framed PPlayerAction packet.
    using SendFn = std::function<void(const uint8_t*, size_t)>;
    explicit InputSystem(SendFn send_fn) : send_(std::move(send_fn)) {}

    // Load (or replace) the full binding table from the server.
    // Stores both the canonical copy (original_bindings_) and a working copy
    // (all_bindings_) so SaveLocalOverrides can diff them.
    void LoadBindings(const std::vector<InputBinding>& bindings);

    void SetContext(const std::string& context);
    const std::string& Context() const { return current_context_; }

    // Called by GLFW event handlers
    void OnKeyDown(const std::string& key, const std::string& modifier);
    void OnKeyUp  (const std::string& key, const std::string& modifier);

    // Register a local action handler (UI actions that don't need network)
    void OnAction(ActionHandler h);

    // Returns all bindings for a context, sorted by action name (for UI display).
    std::vector<InputBinding> GetBindings(const std::string& context) const;

    // Rebind: find existing binding by (context, action, trigger_type_str),
    // change key+modifier. trigger_type_str is "press", "hold", etc.
    void Rebind(const std::string& context, const std::string& action,
                const std::string& trigger_type_str,
                const std::string& new_key, const std::string& new_modifier);

    // Reset all current bindings back to the originals set by LoadBindings.
    void ResetToDefault();

    // Load per-player key remaps from a minimal JSON file (see implementation
    // for format). Only patches existing (context, action, trigger_type) pairs —
    // cannot inject new actions, preventing client-side privilege escalation.
    void LoadLocalOverrides(const std::string& path);

    // Write the diff between all_bindings_ and original_bindings_ to path as
    // a JSON overrides file. No-op if there are no changes.
    void SaveLocalOverrides(const std::string& path) const;

private:
    void Dispatch(const std::string& action, uint8_t state, float axis);
    void RebuildLookupTables();

    double TimeNow() const {
        using clk = std::chrono::steady_clock;
        static const auto start = clk::now();
        return std::chrono::duration<double>(clk::now() - start).count();
    }

    SendFn      send_;
    std::string current_context_ = "gameplay";

    // Flat lists — all_bindings_ is the working set, original_bindings_ is
    // the server-supplied snapshot used to compute the overrides diff.
    std::vector<InputBinding> all_bindings_;
    std::vector<InputBinding> original_bindings_;

    // context → { (key, modifier, trigger) → binding }
    // Rebuilt from all_bindings_ by RebuildLookupTables().
    std::unordered_map<std::string,
        std::unordered_map<ChordKey, InputBinding, ChordKeyHash>> by_context_;

    std::unordered_set<std::string>         hold_active_;
    std::unordered_map<std::string, double> last_press_time_;
    std::vector<ActionHandler>              handlers_;
};

} // namespace rco::input
