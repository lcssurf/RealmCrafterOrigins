#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <sstream>

// OpenGL / GLFW
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Dear ImGui
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// RCO headers
#include "window.h"
#include "paths.h"
#include "player_controller.h"
#include "../net/connection.h"
#include "../net/protocol.h"
#include "../net/codec.h"
#include "../ui/game_state.h"
#include "../ui/login_screen.h"
#include "../ui/char_select.h"
#include "../ui/chat.h"
#include "../ui/inventory.h"
#include "../ui/floating_numbers.h"
#include "../ui/ui_texture.h"
#include "../ui/spellbar.h"
#include "../ui/skill_hotbar.h"
#include "../ui/spell_effects.h"
#include "../ui/quest_log.h"
#include "../ui/party_panel.h"
#include "../ui/chat_bubbles.h"
#include "../ui/controls_ui.h"
#include "../ui/skill_loadout_screen.h"
#include "../gameplay/ingame_packet_gate.h"
#include "../gameplay/kit_pool.h"
#include "../gameplay/skill_state.h"
#include "../renderer/camera.h"
#include "../renderer/terrain/terrain.h"
#include "../renderer/actors/actor.h"
#include "../renderer/particles.h"
#include "../renderer/anim_controller.h"
#include "../input/input_system.h"
#include "../audio/audio.h"

#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"
#include "rco/renderer/model_cache.h"

// Scroll callback routes wheel input to the camera.
static rco::renderer::Camera* g_camera = nullptr;
static void ScrollCallback(GLFWwindow*, double, double y) {
    if (g_camera) g_camera->ProcessScroll(static_cast<float>(y));
}

static std::string TrimConfigToken(std::string s) {
    const char* ws = " \t\r\n";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string{};
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static bool ParseConfigBool(std::string raw, bool& out) {
    raw = TrimConfigToken(std::move(raw));
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        raw = raw.substr(1, raw.size() - 2);
    }
    std::transform(raw.begin(), raw.end(), raw.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (raw == "1" || raw == "true" || raw == "on" || raw == "yes") {
        out = true;
        return true;
    }
    if (raw == "0" || raw == "false" || raw == "off" || raw == "no") {
        out = false;
        return true;
    }
    return false;
}

static bool ReadConfigBool(const char* section_name,
                           const char* key_name,
                           bool& out_value) {
    std::ifstream f("config.toml");
    if (!f) return false;

    bool in_section = false;
    std::string line;
    while (std::getline(f, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = TrimConfigToken(std::move(line));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            const std::string section = TrimConfigToken(line.substr(1, line.size() - 2));
            in_section = (section == section_name);
            continue;
        }
        if (!in_section) continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimConfigToken(line.substr(0, eq));
        if (key != key_name) continue;
        std::string val = TrimConfigToken(line.substr(eq + 1));
        return ParseConfigBool(val, out_value);
    }
    return false;
}

static bool ReadConfigString(const char* section_name,
                             const char* key_name,
                             std::string& out_value) {
    std::ifstream f("config.toml");
    if (!f) return false;

    bool in_section = false;
    std::string line;
    while (std::getline(f, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = TrimConfigToken(std::move(line));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            const std::string section = TrimConfigToken(line.substr(1, line.size() - 2));
            in_section = (section == section_name);
            continue;
        }
        if (!in_section) continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimConfigToken(line.substr(0, eq));
        if (key != key_name) continue;

        std::string val = TrimConfigToken(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        out_value = val;
        return true;
    }
    return false;
}

enum class CursorReleaseMode {
    Hold,
    Toggle,
};

struct InputConfig {
    CursorReleaseMode cursor_release_mode = CursorReleaseMode::Hold;
    int cursor_release_key_glfw = GLFW_KEY_LEFT_ALT;
    std::string cursor_release_key_name = "left_alt";
};

static int MapCursorReleaseKey(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "left_alt") return GLFW_KEY_LEFT_ALT;
    if (name == "right_alt") return GLFW_KEY_RIGHT_ALT;
    if (name == "left_ctrl") return GLFW_KEY_LEFT_CONTROL;
    if (name == "right_ctrl") return GLFW_KEY_RIGHT_CONTROL;
    if (name == "tab") return GLFW_KEY_TAB;
    return GLFW_KEY_LEFT_ALT;
}

static CursorReleaseMode MapCursorReleaseMode(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "toggle") return CursorReleaseMode::Toggle;
    return CursorReleaseMode::Hold;
}

static const char* CursorReleaseKeyLabel(int key) {
    switch (key) {
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT:
        return "Alt";
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL:
        return "Ctrl";
    case GLFW_KEY_TAB:
        return "Tab";
    default:
        return "Alt";
    }
}

static InputConfig ResolveInputConfig() {
    InputConfig cfg{};
    std::string mode_raw;
    if (ReadConfigString("input", "cursor_release_mode", mode_raw)) {
        cfg.cursor_release_mode = MapCursorReleaseMode(mode_raw);
    }
    std::string key_raw;
    if (ReadConfigString("input", "cursor_release_key", key_raw)) {
        cfg.cursor_release_key_name = key_raw;
        cfg.cursor_release_key_glfw = MapCursorReleaseKey(key_raw);
    }
    return cfg;
}

static bool SceneDebugLogsEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("RCO_CLIENT_DEBUG_LOGS");
        if (!v) return false;
        return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
               std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0 ||
               std::strcmp(v, "ON") == 0;
    }();
    return enabled;
}

static bool EntryPerfEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("RCO_PERF_ENTRY");
        bool parsed = false;
        if (v && ParseConfigBool(v, parsed)) return parsed;

        bool cfg_enabled = false;
        if (ReadConfigBool("perf", "entry_log_enabled", cfg_enabled)) {
            return cfg_enabled;
        }
        return false;
    }();
    return enabled;
}

class EntryPerfLogger {
public:
    EntryPerfLogger() {
        enabled_ = EntryPerfEnabled();
        if (!enabled_) return;
        start_tp_ = Clock::now();
        std::time_t tt = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        file_path_ = "perf_entry_" + std::string(stamp) + ".jsonl";
    }

    ~EntryPerfLogger() {
        if (!enabled_) return;
        if (entry_active_ && !entry_finalized_) Finalize("process_exit");
        Flush();
    }

    bool Enabled() const { return enabled_; }
    int EntryID() const { return entry_id_; }
    bool EntryActive() const { return entry_active_ && !entry_finalized_; }
    const std::string& FilePath() const { return file_path_; }

    void StartEntry(int slot) {
        if (!enabled_) return;
        if (entry_active_ && !entry_finalized_) Finalize("entry_restart");
        ++entry_id_;
        entry_active_ = true;
        entry_finalized_ = false;
        entry_start_us_ = NowUs();
        core_done_logged_ = false;
        gate_release_logged_ = false;
        client_world_ready_us_ = 0;
        last_actor_init_us_ = 0;
        LogKV("entry_start",
              ",\"slot\":" + std::to_string(slot));
    }

    void LogPacketArrival(const char* packet_name) {
        if (!EntryActive()) return;
        LogKV("packet_arrival",
              ",\"packet\":\"" + Escape(packet_name ? packet_name : "") + "\"");
    }

    void LogWorldObjectsSummary(
        uint16_t object_count,
        const std::unordered_map<std::string, int>& model_counts) {
        if (!EntryActive()) return;
        LogKV("world_objects_summary",
              ",\"object_count\":" + std::to_string(object_count) +
              ",\"unique_models\":" + std::to_string(model_counts.size()));
        for (const auto& [model, count] : model_counts) {
            LogKV("world_object_model_count",
                  ",\"model\":\"" + Escape(model) +
                  "\",\"count\":" + std::to_string(count));
        }
    }

    void LogLazyStage(const char* stage, uint64_t dur_us, bool ok = true) {
        if (!EntryActive()) return;
        LogKV("lazy_stage",
              ",\"stage\":\"" + Escape(stage ? stage : "") +
              "\",\"dur_us\":" + std::to_string(dur_us) +
              ",\"ok\":" + std::string(ok ? "true" : "false"));
    }

    void LogLoadEnvironment(const char* callsite,
                            const std::string& path,
                            uint64_t dur_us) {
        if (!EntryActive()) return;
        LogKV("load_environment_call",
              ",\"callsite\":\"" + Escape(callsite ? callsite : "") +
              "\",\"path\":\"" + Escape(path) +
              "\",\"dur_us\":" + std::to_string(dur_us));
    }

    void LogLoadEnvironmentSkipped(const char* callsite,
                                   const std::string& path) {
        if (!EntryActive()) return;
        LogKV("load_environment_skipped",
              ",\"callsite\":\"" + Escape(callsite ? callsite : "") +
              "\",\"path\":\"" + Escape(path) + "\"");
    }

    void LogClientWorldReady(uint32_t total_ms, uint32_t client_init_ms) {
        if (!EntryActive()) return;
        client_world_ready_us_ = NowUs();
        LogKV("client_world_ready_sent",
              ",\"total_ms\":" + std::to_string(total_ms) +
              ",\"client_init_ms\":" + std::to_string(client_init_ms));
    }

    void LogCoreDone(std::size_t core_total, std::size_t core_done, int pending_total) {
        if (!EntryActive() || core_done_logged_) return;
        core_done_logged_ = true;
        LogKV("gate_core_done",
              ",\"core_total\":" + std::to_string(core_total) +
              ",\"core_done\":" + std::to_string(core_done) +
              ",\"pending_total\":" + std::to_string(pending_total));
    }

    void LogGateRelease(const char* reason,
                        std::size_t core_total,
                        std::size_t core_done,
                        int pending_total,
                        double elapsed_ms) {
        if (!EntryActive() || gate_release_logged_) return;
        gate_release_logged_ = true;
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(3);
        os << ",\"reason\":\"" << Escape(reason ? reason : "")
           << "\",\"core_total\":" << core_total
           << ",\"core_done\":" << core_done
           << ",\"pending_total\":" << pending_total
           << ",\"elapsed_ms\":" << elapsed_ms;
        LogKV("gate_release", os.str());
    }

    void LogActorInit(const char* stage,
                      const std::string& model_path,
                      bool cache_hit,
                      bool post_loading,
                      uint64_t dur_us,
                      uint64_t frame_index) {
        if (!EntryActive()) return;
        last_actor_init_us_ = NowUs();
        LogKV("actor_init",
              ",\"stage\":\"" + Escape(stage ? stage : "") +
              "\",\"model\":\"" + Escape(model_path) +
              "\",\"cache_hit\":" + std::string(cache_hit ? "true" : "false") +
              ",\"post_loading\":" + std::string(post_loading ? "true" : "false") +
              ",\"dur_us\":" + std::to_string(dur_us) +
              ",\"frame\":" + std::to_string(frame_index));
    }

    void LogRebuildMaterials(const char* stage,
                             bool post_loading,
                             uint64_t dur_us,
                             uint64_t frame_index) {
        if (!EntryActive()) return;
        LogKV("rebuild_materials",
              ",\"stage\":\"" + Escape(stage ? stage : "") +
              "\",\"post_loading\":" + std::string(post_loading ? "true" : "false") +
              ",\"dur_us\":" + std::to_string(dur_us) +
              ",\"frame\":" + std::to_string(frame_index));
    }

    void LogModelCacheGet(const char* path, bool hit, const char* context) {
        if (!EntryActive()) return;
        LogKV("model_cache_get",
              ",\"path\":\"" + Escape(path ? path : "") +
              "\",\"hit\":" + std::string(hit ? "true" : "false") +
              ",\"context\":\"" + Escape(context ? context : "") + "\"");
    }

    void Finalize(const char* reason) {
        if (!EntryActive()) return;
        uint64_t streaming_us = 0;
        if (client_world_ready_us_ > 0 && last_actor_init_us_ > client_world_ready_us_) {
            streaming_us = last_actor_init_us_ - client_world_ready_us_;
        }
        LogKV("entry_finalize",
              ",\"reason\":\"" + Escape(reason ? reason : "") +
              "\",\"world_ready_us\":" + std::to_string(client_world_ready_us_) +
              ",\"last_actor_init_us\":" + std::to_string(last_actor_init_us_) +
              ",\"streaming_tail_us\":" + std::to_string(streaming_us));
        entry_finalized_ = true;
        Flush();
    }

    void MaybeAutoFinalize(bool world_entry_loading, int pending_static, int pending_npc) {
        if (!EntryActive()) return;
        if (world_entry_loading) return;
        const uint64_t now_us = NowUs();
        constexpr uint64_t kIdleFinalizeUs = 2'000'000;
        constexpr uint64_t kHardFinalizeUs = 120'000'000;

        if (client_world_ready_us_ > 0 && now_us > client_world_ready_us_ + kHardFinalizeUs) {
            Finalize("hard_timeout");
            return;
        }
        if (pending_static != 0 || pending_npc != 0) return;

        if (last_actor_init_us_ > 0) {
            if (now_us > last_actor_init_us_ + kIdleFinalizeUs) {
                Finalize("idle_after_last_init");
            }
            return;
        }
        if (client_world_ready_us_ > 0 && now_us > client_world_ready_us_ + kIdleFinalizeUs) {
            Finalize("idle_no_actor_init");
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    static std::string Escape(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 16);
        for (char c : in) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"':  out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c; break;
            }
        }
        return out;
    }

    uint64_t NowUs() const {
        if (!enabled_) return 0;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - start_tp_).count());
    }

    void LogKV(const char* event, const std::string& extra) {
        if (!enabled_) return;
        const uint64_t ts_us = NowUs();
        std::ostringstream os;
        os << "{\"event\":\"" << Escape(event ? event : "") << "\""
           << ",\"entry_id\":" << entry_id_
           << ",\"ts_us\":" << ts_us;
        if (entry_active_ && ts_us >= entry_start_us_) {
            os << ",\"entry_rel_us\":" << (ts_us - entry_start_us_);
        }
        os << extra << "}";
        lines_.push_back(os.str());
        if (lines_.size() >= 8192) Flush();
    }

    void Flush() {
        if (!enabled_ || lines_.empty()) return;
        std::ofstream out(file_path_, std::ios::out | std::ios::app);
        if (!out) return;
        for (const auto& l : lines_) out << l << '\n';
        lines_.clear();
    }

    bool enabled_ = false;
    bool entry_active_ = false;
    bool entry_finalized_ = false;
    bool core_done_logged_ = false;
    bool gate_release_logged_ = false;
    int entry_id_ = 0;
    uint64_t entry_start_us_ = 0;
    uint64_t client_world_ready_us_ = 0;
    uint64_t last_actor_init_us_ = 0;
    Clock::time_point start_tp_{};
    std::string file_path_;
    std::vector<std::string> lines_;
};

static EntryPerfLogger* g_entry_perf_logger = nullptr;

static void ModelCachePerfObserver(const char* path, bool hit, const char* context) {
    if (!g_entry_perf_logger) return;
    g_entry_perf_logger->LogModelCacheGet(path, hit, context);
}

class ModelCacheContextScope {
public:
    explicit ModelCacheContextScope(const char* context) {
        rco::renderer::ModelCacheSetContext(context);
    }
    ~ModelCacheContextScope() {
        rco::renderer::ModelCacheSetContext(nullptr);
    }
};

struct LoadingPresetConfig {
    const char* name;
    float core_preload_radius;
    int core_preload_max_objects;
    double core_preload_timeout_ms;
    int core_init_per_frame;
    double core_init_budget_ms;
    int core_cold_loads_per_frame;
    int loading_exit_pending_max;
    int global_init_per_frame_after_core;
    double global_init_budget_ms_after_core;
    int static_init_per_frame;
    double static_init_budget_ms;
    int static_max_cold_loads_per_frame;
};

static LoadingPresetConfig ResolveLoadingPreset() {
    auto trim = [](std::string s) {
        const char* ws = " \t\r\n";
        const std::size_t b = s.find_first_not_of(ws);
        if (b == std::string::npos) return std::string{};
        const std::size_t e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    };

    std::string v = "medium";
    std::ifstream f("config.toml");
    if (f) {
        std::string line;
        bool in_loading = false;
        while (std::getline(f, line)) {
            const auto hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            line = trim(line);
            if (line.empty()) continue;

            if (line.front() == '[' && line.back() == ']') {
                const std::string section = trim(line.substr(1, line.size() - 2));
                in_loading = (section == "loading");
                continue;
            }
            if (!in_loading) continue;

            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (key != "preset") continue;
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
                val = val.substr(1, val.size() - 2);
            std::transform(val.begin(), val.end(), val.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (val == "low" || val == "medium" || val == "high") v = val;
            break;
        }
    }

    if (v == "low") {
        return {
            "low",
            90.f, 40, 7000.0, 3, 6.0, 1, 0,
            2, 3.0,
            32, 8.0, 4
        };
    }
    if (v == "high") {
        return {
            "high",
            170.f, 110, 20000.0, 8, 16.0, 4, 0,
            6, 10.0,
            64, 12.0, 8
        };
    }
    // default: medium
    return {
        "medium",
        140.f, 70, 15000.0, 6, 12.0, 3, 0,
        4, 6.0,
        48, 10.0, 6
    };
}

struct AreaLightingProfile {
    std::string preset_name = "clear_day";
    glm::vec3 sun_dir = glm::normalize(glm::vec3(0.24f, 0.92f, 0.30f));
    glm::vec3 sun_color = glm::vec3(1.10f, 1.08f, 1.02f);
    bool volumetrics_default = true;
    float sun_intensity_mul = 1.00f;
    float sky_intensity_mul = 1.00f;
    float fog_density_mul = 1.00f;
    glm::vec3 fog_color = glm::vec3(0.70f, 0.80f, 0.93f);
};

struct AreaLightingConfig {
    std::unordered_map<std::string, AreaLightingProfile> presets;
    std::unordered_map<std::string, std::string> area_preset;
    std::string default_preset = "clear_day";
};

struct RenderColorProfile {
    float contrast = 1.08f;
    float saturation = 1.08f;
    float vibrance = 0.20f;
    float black_point = 0.010f;
    float vignette_strength = 0.04f;
    float vignette_softness = 0.55f;
};

static rco::renderer::TerrainRenderTuning ResolveTerrainRenderTuning() {
    rco::renderer::TerrainRenderTuning cfg{};
    cfg.tiling_mul = 1.00f;
    cfg.macro_strength_mul = 1.00f;
    cfg.height_blend_slop = 0.20f;
    return cfg;
}

static std::string NormalizeLightingKey(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (char& c : s) {
        if (c == ' ' || c == '-' || c == '/' || c == '\\' || c == '.') c = '_';
    }
    return s;
}

static std::string TrimCopy(std::string s) {
    const char* ws = " \t\r\n";
    const std::size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string{};
    const std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static bool ParseFloatStrict(const std::string& raw, float& out) {
    const std::string s = TrimCopy(raw);
    if (s.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    while (end && *end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return false;
        ++end;
    }
    out = v;
    return true;
}

static ImU32 ParseColorRGBA01OrFallback(const std::string& raw, ImU32 fallback) {
    float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
    if (std::sscanf(raw.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) != 4) {
        return fallback;
    }
    r = std::clamp(r, 0.f, 1.f);
    g = std::clamp(g, 0.f, 1.f);
    b = std::clamp(b, 0.f, 1.f);
    a = std::clamp(a, 0.f, 1.f);
    const int ri = static_cast<int>(r * 255.f + 0.5f);
    const int gi = static_cast<int>(g * 255.f + 0.5f);
    const int bi = static_cast<int>(b * 255.f + 0.5f);
    const int ai = static_cast<int>(a * 255.f + 0.5f);
    return IM_COL32(ri, gi, bi, ai);
}

static ImU32 DefaultInnerTelegraphColorForReason(std::string reason_tag) {
    std::transform(reason_tag.begin(), reason_tag.end(), reason_tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (reason_tag.find("phase_3") != std::string::npos ||
        reason_tag.find("enrage") != std::string::npos) {
        return IM_COL32(255, 135, 110, 240);
    }
    if (reason_tag.find("phase_2") != std::string::npos) {
        return IM_COL32(255, 195, 95, 240);
    }
    return IM_COL32(255, 230, 80, 235);
}

static std::string TelegraphPhaseLabelFromReason(std::string reason_tag) {
    std::transform(reason_tag.begin(), reason_tag.end(), reason_tag.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (reason_tag.find("phase_3") != std::string::npos ||
        reason_tag.find("enrage") != std::string::npos) {
        return "Phase 3 / Enrage";
    }
    if (reason_tag.find("phase_2") != std::string::npos) return "Phase 2";
    if (reason_tag.find("phase_1") != std::string::npos) return "Phase 1";
    return std::string{};
}

struct CombatTelegraphMeta {
    bool     valid = false;
    float    radius = 0.f;
    ImU32    outer_color = 0;
    int      parry_window_ms = 0;
    std::string style;
    std::string reason_tag;
};

static CombatTelegraphMeta ParseCombatTelegraphMeta(const std::string& raw_text) {
    CombatTelegraphMeta meta{};
    if (raw_text.rfind("meta:", 0) != 0) return meta;

    std::string payload = raw_text.substr(5);
    bool has_telegraph_hint = false;
    bool has_radius = false;
    bool has_color = false;
    bool has_reason = false;

    std::size_t cursor = 0;
    while (cursor <= payload.size()) {
        std::size_t sep = payload.find(';', cursor);
        if (sep == std::string::npos) sep = payload.size();
        std::string token = TrimCopy(payload.substr(cursor, sep - cursor));
        cursor = (sep == payload.size()) ? (payload.size() + 1) : (sep + 1);
        if (token.empty()) continue;
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos) continue;

        std::string key = TrimCopy(token.substr(0, eq));
        std::string val = TrimCopy(token.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (key == "telegraph") {
            has_telegraph_hint = true;
        } else if (key == "radius") {
            float radius = 0.f;
            if (ParseFloatStrict(val, radius) && radius > 0.f) {
                meta.radius = radius;
                has_radius = true;
            }
        } else if (key == "color") {
            meta.outer_color = ParseColorRGBA01OrFallback(val, 0);
            has_color = (meta.outer_color != 0);
        } else if (key == "window_ms" || key == "parry_window_ms") {
            float ms = 0.f;
            if (ParseFloatStrict(val, ms) && ms > 0.f) {
                meta.parry_window_ms = static_cast<int>(ms + 0.5f);
            }
        } else if (key == "style") {
            meta.style = val;
        } else if (key == "reason") {
            meta.reason_tag = val;
            has_reason = !meta.reason_tag.empty();
        }
    }

    meta.valid = has_telegraph_hint || has_radius || has_color || has_reason;
    return meta;
}

static AreaLightingConfig ResolveAreaLightingConfig() {
    AreaLightingConfig cfg{};

    // Built-in safe defaults.
    // Authoritative values can arrive from server via PAreaConfig.
    cfg.presets["clear_day"] = AreaLightingProfile{
        .preset_name = "clear_day",
        .sun_dir = glm::normalize(glm::vec3(0.18f, 0.96f, 0.20f)),
        .sun_color = glm::vec3(1.14f, 1.12f, 1.05f),
        .volumetrics_default = true,
        .sun_intensity_mul = 1.00f,
        .sky_intensity_mul = 1.00f,
        .fog_density_mul = 1.00f,
        .fog_color = glm::vec3(0.70f, 0.80f, 0.93f),
    };
    cfg.presets["overcast"] = AreaLightingProfile{
        .preset_name = "overcast",
        .sun_dir = glm::normalize(glm::vec3(0.10f, 0.88f, 0.18f)),
        .sun_color = glm::vec3(0.92f, 0.95f, 1.00f),
        .volumetrics_default = true,
        .sun_intensity_mul = 0.78f,
        .sky_intensity_mul = 0.92f,
        .fog_density_mul = 1.18f,
        .fog_color = glm::vec3(0.67f, 0.75f, 0.88f),
    };
    cfg.presets["golden_hour"] = AreaLightingProfile{
        .preset_name = "golden_hour",
        .sun_dir = glm::normalize(glm::vec3(0.45f, 0.62f, 0.28f)),
        .sun_color = glm::vec3(1.26f, 1.02f, 0.82f),
        .volumetrics_default = true,
        .sun_intensity_mul = 0.92f,
        .sky_intensity_mul = 0.98f,
        .fog_density_mul = 1.08f,
        .fog_color = glm::vec3(0.82f, 0.74f, 0.66f),
    };
    cfg.area_preset["training_camp"] = "clear_day";
    cfg.area_preset["starter_zone"] = "clear_day";

    for (auto& [name, p] : cfg.presets) {
        if (glm::dot(p.sun_dir, p.sun_dir) > 0.0001f) p.sun_dir = glm::normalize(p.sun_dir);
        else p.sun_dir = glm::normalize(glm::vec3(0.24f, 0.92f, 0.30f));
        p.sun_intensity_mul = glm::clamp(p.sun_intensity_mul, 0.0f, 2.0f);
        p.sky_intensity_mul = glm::clamp(p.sky_intensity_mul, 0.0f, 2.0f);
        p.fog_density_mul = glm::clamp(p.fog_density_mul, 0.0f, 2.0f);
        p.fog_color.r = glm::clamp(p.fog_color.r, 0.0f, 2.0f);
        p.fog_color.g = glm::clamp(p.fog_color.g, 0.0f, 2.0f);
        p.fog_color.b = glm::clamp(p.fog_color.b, 0.0f, 2.0f);
    }
    return cfg;
}

static AreaLightingProfile ResolveAreaLightingProfile(const std::string& area_name,
                                                      const AreaLightingConfig& cfg) {
    const std::string area = NormalizeLightingKey(area_name);
    std::string preset = cfg.default_preset.empty() ? std::string("clear_day") : cfg.default_preset;
    if (auto it = cfg.area_preset.find(area); it != cfg.area_preset.end()) {
        preset = NormalizeLightingKey(it->second);
    }

    if (auto it = cfg.presets.find(preset); it != cfg.presets.end()) return it->second;
    if (auto it = cfg.presets.find("clear_day"); it != cfg.presets.end()) return it->second;
    return AreaLightingProfile{};
}

static rco::renderer::Pipeline::CharacterReadabilityTuning ResolveCharacterReadabilityTuning() {
    rco::renderer::Pipeline::CharacterReadabilityTuning tuning{};
    tuning.shadowLift = 0.30f;
    tuning.rimStrength = 0.18f;
    tuning.rimExponent = 2.40f;
    tuning.minNdotL = 0.10f;
    tuning.ambientBoost = 0.12f;
    return tuning;
}

static rco::renderer::Pipeline::SceneLookTuning ResolveSceneLookTuning() {
    rco::renderer::Pipeline::SceneLookTuning tuning{};
    tuning.iblIntensity = 1.00f;
    tuning.skyIntensity = 1.16f;
    tuning.worldShadowLift = 0.10f;
    tuning.directScale = 1.32f;
    tuning.ambientScale = 0.88f;
    tuning.flatAmbient = 0.03f;
    tuning.worldMinNdotL = 0.05f;
    tuning.albedoMinLuma = 0.18f;
    tuning.albedoLiftStrength = 0.00f;
    tuning.specularScale = 0.88f;
    tuning.exposureFactor = 1.10f;
    tuning.sunIntensity = 1.36f;
    return tuning;
}

static RenderColorProfile ResolveRenderColorProfile() {
    RenderColorProfile cfg{};
    cfg.contrast = 1.08f;
    cfg.saturation = 1.08f;
    cfg.vibrance = 0.20f;
    cfg.black_point = 0.010f;
    cfg.vignette_strength = 0.04f;
    cfg.vignette_softness = 0.55f;
    return cfg;
}

static std::string ResolveIblPathFromAreaConfig(const std::string& skybox_hdr) {
    namespace fs = std::filesystem;
    const std::string fallback = "assets/ibl/default.hdr";
    auto trim = [](std::string s) {
        const char* ws = " \t\r\n";
        const std::size_t b = s.find_first_not_of(ws);
        if (b == std::string::npos) return std::string{};
        const std::size_t e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    };
    auto hasExt = [](const std::string& s) {
        return fs::path(s).has_extension();
    };

    std::string raw = trim(skybox_hdr);
    if (raw.empty()) return fallback;

    std::vector<std::string> candidates;
    const bool already_scoped =
        raw.rfind("assets/ibl/", 0) == 0 || raw.rfind("assets\\ibl\\", 0) == 0;
    if (already_scoped) candidates.push_back(raw);
    else candidates.push_back("assets/ibl/" + raw);

    if (!hasExt(raw)) {
        if (already_scoped) candidates.push_back(raw + ".hdr");
        else candidates.push_back("assets/ibl/" + raw + ".hdr");
    }

    for (const auto& c : candidates) {
        if (fs::exists(c) && fs::is_regular_file(c)) return c;
    }

    std::fprintf(stderr,
        "[ibl] warning: skybox '%s' not found in assets/ibl (tried %zu path(s)); using default.hdr\n",
        raw.c_str(), candidates.size());
    return fallback;
}

int main() {
    // Anchor all relative paths (shaders/, assets/, data/) to the exe's dir,
    // not the launcher's cwd. After this call everything resolves from dist/client/.
    rco::SetCwdToExeDir();

    // -----------------------------------------------------------------------
    // Window + OpenGL context
    // -----------------------------------------------------------------------
    rco::Window window(1280, 720, "RealmCrafter: Origins");

    // -----------------------------------------------------------------------
    // Dear ImGui
    // -----------------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    (void)io;

    ImGui_ImplGlfw_InitForOpenGL(window.Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 460");
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.f;
    style.FrameRounding  = 4.f;
    style.GrabRounding   = 4.f;
    style.ItemSpacing    = {8.f, 6.f};
    style.WindowPadding  = {12.f, 10.f};

    // -----------------------------------------------------------------------
    // Network
    // -----------------------------------------------------------------------
    rco::net::Connection conn;
    EntryPerfLogger perf_entry;
    g_entry_perf_logger = perf_entry.Enabled() ? &perf_entry : nullptr;
    rco::renderer::ModelCacheSetObserver(
        perf_entry.Enabled() ? &ModelCachePerfObserver : nullptr);

    // -----------------------------------------------------------------------
    // Game state
    // -----------------------------------------------------------------------
    rco::GameState               state = rco::GameState::Login;
    rco::PlayerState             player{};
    std::vector<rco::CharacterInfo> characters;
    std::vector<rco::PlayableDef>   playable_defs;
    std::string                  login_error;

    // Appearance data received in PNewActor. We store it on the entry (instead
    // of instantiating rco::renderer::Actor immediately) because PNewActor
    // packets arrive BEFORE the lazy renderer init runs — renderer_ready is
    // still false at packet-handling time. The actual Actor is constructed on
    // first render once renderer_ready becomes true.
    struct WorldAiMat {
        std::string ai_name;
        std::string albedo, normal, orm;
        float       ar = 0, ag = 0, ab = 0, roughness = 0, metallic = 0;
    };
    struct WorldMesh {
        uint8_t     slot       = 0;
        std::string model_path;
        float       scale      = 1.f;
        std::string albedo, normal, orm;
        float       ar = 0, ag = 0, ab = 0, roughness = 0, metallic = 0;
        // Per-aiMaterial map — server resolves the model's "blinn1=ID01"
        // mapping into concrete texture paths. Applied on the client via
        // Actor::ApplyMaterialsByName so multi-material meshes paint correctly.
        std::vector<WorldAiMat> material_map;
    };
    struct WorldAnim {
        std::string action;
        std::string source_path;
        std::string clip_override;
        int32_t     start_frame = 0;
        int32_t     end_frame   = -1;
        float       fps         = 30.f;
        bool        loop        = true;
        float       speed       = 1.f;
        float       blend_in    = 0.15f;
        std::string return_to;
        uint8_t     priority    = 0;
        struct AnimEvent { int32_t frame; std::string event_type; std::string payload; };
        std::vector<AnimEvent> events;
    };

    struct WorldActorEntry {
        float x = 0, y = 0, z = 0, yaw = 0;
        float prev_x = 0, prev_z = 0; // for velocity estimation
        std::string name;
        uint16_t level = 1;
        int32_t health = 100, health_max = 100;
        uint8_t     actor_type = 0; // 0=player, 1=combat NPC, 2=dialog NPC
        std::string anim_name  = "Idle";
        float       anim_t     = 0.f;

        // Appearance from the Media Actor Def bound to this NPC (if any).
        std::vector<WorldMesh> meshes;
        std::vector<WorldAnim> anims;

        // Offsets from the Actor Def — corrects orientation/ground alignment.
        float yaw_offset = 0.f;
        float y_offset   = 0.f;

        // Per-actor rendering — lazily instantiated from `meshes` on first
        // render when renderer_ready is true. nullptr = fall back to the
        // shared player_actor model.
        std::unique_ptr<rco::renderer::Actor> actor;

        // Animation state machine
        rco::anim::AnimController anim_ctrl;

        WorldActorEntry() = default;
        WorldActorEntry(WorldActorEntry&&) = default;
        WorldActorEntry& operator=(WorldActorEntry&&) = default;
        WorldActorEntry(const WorldActorEntry&) = delete;
        WorldActorEntry& operator=(const WorldActorEntry&) = delete;
    };
    std::unordered_map<uint32_t, WorldActorEntry> world_actors;
    struct CorpseEntry {
        uint32_t rid = 0;
        WorldActorEntry actor;
        double death_time = 0.0;
        double sink_start_time = 0.0;
        double remove_time = 0.0;
        float  base_y = 0.f;
        float  sink_depth = 1.15f;
        CorpseEntry() = default;
        CorpseEntry(CorpseEntry&&) = default;
        CorpseEntry& operator=(CorpseEntry&&) = default;
        CorpseEntry(const CorpseEntry&) = delete;
        CorpseEntry& operator=(const CorpseEntry&) = delete;
    };
    std::vector<CorpseEntry> world_corpses;
    static constexpr double kCorpseLieTimeSec = 4.25;
    static constexpr double kCorpseSinkDurationSec = 6.50;
    static constexpr float  kCorpseSinkDepth = 1.20f;

    // AnimController for the local player
    rco::anim::AnimController player_anim_ctrl;
    player_anim_ctrl.log_enabled = false;
    float player_yaw_offset = 0.f;
    float player_y_offset   = 0.f;

    // InputSystem — created after the connection is established
    std::unique_ptr<rco::input::InputSystem> input_system;

    struct PortalEntry {
        float x, z, radius;
        std::string target_area;
    };
    std::vector<PortalEntry> area_portals;

    // -----------------------------------------------------------------------
    // Renderer
    // -----------------------------------------------------------------------
    rco::renderer::Camera  camera;
    rco::renderer::Terrain terrain;
    rco::renderer::ColData col_data;
    rco::renderer::Actor   player_actor;
    bool renderer_ready = false;
    bool initial_material_rebuild_pending_log = false;

    rco::renderer::Engine engine;
    std::unique_ptr<rco::renderer::Pipeline> pipeline;

    rco::ui::Chat            chat;
    rco::ui::Inventory       inventory;
    rco::ui::FloatingNumbers float_nums;
    rco::ui::SpellBar        spellbar;
    rco::ui::SkillHotbar     skill_hotbar;
    rco::ui::SpellEffects    spell_fx;
    rco::ui::QuestLog        quest_log;
    rco::ui::PartyPanel      party_panel;
    rco::ui::ChatBubbles     chat_bubbles;
    rco::ui::ControlsUI      controls_ui;
    rco::ui::SkillLoadoutScreen skill_loadout_screen;

    rco::renderer::ParticleSystem particles;
    rco::audio::AudioSystem       audio;
    const InputConfig input_config = ResolveInputConfig();
    const LoadingPresetConfig loading_preset = ResolveLoadingPreset();
    const rco::renderer::Pipeline::CharacterReadabilityTuning character_readability_tuning =
        ResolveCharacterReadabilityTuning();
    const rco::renderer::Pipeline::SceneLookTuning scene_look_tuning =
        ResolveSceneLookTuning();
    const AreaLightingConfig area_lighting_fallback_config = ResolveAreaLightingConfig();
    const RenderColorProfile render_color_profile =
        ResolveRenderColorProfile();
    const rco::renderer::TerrainRenderTuning terrain_render_tuning =
        ResolveTerrainRenderTuning();
    rco::renderer::Pipeline::CharacterReadabilityTuning active_character_readability_tuning =
        character_readability_tuning;
    rco::renderer::Pipeline::SceneLookTuning active_scene_look_tuning =
        scene_look_tuning;
    RenderColorProfile active_render_color_profile =
        render_color_profile;
    rco::renderer::TerrainRenderTuning active_terrain_render_tuning =
        terrain_render_tuning;
    AreaLightingProfile active_area_lighting =
        ResolveAreaLightingProfile("training_camp", area_lighting_fallback_config);
    std::string pending_area_skybox_hdr;
    bool area_environment_pending_apply = false;

    // World-enter timing (Etapa A / Fase 2 baseline):
    // StartGame sent -> PStartGame received -> renderer_ready (first playable frame).
    bool world_enter_pending = false;
    bool world_enter_logged  = false;
    std::chrono::steady_clock::time_point world_enter_start_tp{};
    std::chrono::steady_clock::time_point world_enter_pstart_tp{};
    bool world_entry_loading = false;
    double world_entry_loading_start = 0.0;
    bool world_entry_world_objects_received = false;
    std::vector<std::size_t> world_entry_core_indices;
    std::size_t world_entry_core_cursor = 0;

    struct DialogState {
        bool                     open = false;
        std::string              npc_name;
        std::string              text;
        std::vector<std::string> options;
    } dialog;

    // Dropped items in the world
    struct WorldItemEntry {
        uint32_t    rid;
        float       x, y, z;
        uint16_t    item_id;
        uint8_t     quantity;
        std::string name;
        uint8_t     item_type;
    };
    std::vector<WorldItemEntry> world_items;

    // Static world objects (received via PWorldObjects on area enter)
    struct WorldObjectEntry {
        std::string model_path;
        float scale = 1.f;
        float x = 0.f, y = 0.f, z = 0.f, yaw = 0.f;
        std::unique_ptr<rco::renderer::Actor> actor;
        WorldObjectEntry() = default;
        WorldObjectEntry(WorldObjectEntry&&) = default;
        WorldObjectEntry& operator=(WorldObjectEntry&&) = default;
        WorldObjectEntry(const WorldObjectEntry&) = delete;
        WorldObjectEntry& operator=(const WorldObjectEntry&) = delete;
    };
    std::vector<WorldObjectEntry> world_static_objects;
    std::vector<std::string> static_model_prewarm_queue;
    std::size_t static_model_prewarm_cursor = 0;
    constexpr std::size_t kStaticInitSampleWindow = 256;
    std::vector<uint64_t> static_init_hit_samples_us;
    std::vector<uint64_t> static_init_miss_samples_us;
    static_init_hit_samples_us.reserve(kStaticInitSampleWindow);
    static_init_miss_samples_us.reserve(kStaticInitSampleWindow);
    auto record_static_init_sample = [&](uint64_t dur_us, bool cache_hit) {
        if (dur_us == 0) return;
        auto& samples = cache_hit ? static_init_hit_samples_us : static_init_miss_samples_us;
        if (samples.size() >= kStaticInitSampleWindow) {
            samples.erase(samples.begin());
        }
        samples.push_back(dur_us);
    };
    auto estimate_static_hit_init_p99_us = [&]() -> double {
        if (static_init_hit_samples_us.empty()) return 0.0;
        std::vector<uint64_t> sorted = static_init_hit_samples_us;
        std::sort(sorted.begin(), sorted.end());
        const double pos = (static_cast<double>(sorted.size() - 1) * 0.99);
        const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
        const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
        if (lo == hi) return static_cast<double>(sorted[lo]);
        const double w = pos - static_cast<double>(lo);
        return static_cast<double>(sorted[lo]) * (1.0 - w) +
               static_cast<double>(sorted[hi]) * w;
    };

    // Shop UI
    struct ShopEntry {
        uint16_t    item_id;
        std::string name;
        uint8_t     item_type, slot_type;
        uint16_t    weapon_damage, armor_level;
        uint32_t    buy_price, sell_price;
    };
    struct ShopState {
        bool                  open = false;
        std::vector<ShopEntry> items;
        int                   tab  = 0; // 0=buy, 1=sell
    } shop;

    uint32_t player_gold = 0;

    // Local player appearance — populated from PNewActor when rid == player.runtimeId.
    std::vector<WorldMesh> player_meshes;
    std::vector<WorldAnim> player_anims;

    // Combat state
    uint32_t combat_target     = 0;
    double   last_attack_sent  = 0.0;
    bool     player_dead       = false;
    glm::mat4 view_mat{1.f}, proj_mat{1.f};
    bool      dodge_roll_active = false;
    glm::vec2 dodge_roll_dir{0.f};
    double    dodge_roll_start = 0.0;
    double    dodge_roll_end = 0.0;
    bool      dodge_roll_pending = false;
    glm::vec2 dodge_roll_pending_dir{0.f};
    double    dodge_roll_pending_until = 0.0;
    constexpr float kDodgeRollSpeedStart = 12.0f;
    constexpr float kDodgeRollSpeedEnd = 4.5f;
    struct SpecialParryTelegraph {
        uint32_t target_rid = 0;
        double   start_time = 0.0;
        double   end_time = 0.0;
        float    radius = 1.45f;
        int      parry_window_ms = 220;
        ImU32    outer_color = IM_COL32(255, 70, 70, 190);
        ImU32    inner_color = IM_COL32(255, 230, 80, 235);
        std::string reason_tag;
    };
    std::unordered_map<uint32_t, SpecialParryTelegraph> special_parry_telegraphs; // key = source mob rid
    struct ParryJudgementFx {
        uint32_t source_rid = 0;
        bool success = false;
        int32_t metric = 0; // success: parry timing (ms), fail: damage received
        double start_time = 0.0;
        double end_time = 0.0;
    };
    std::vector<ParryJudgementFx> parry_judgements;

    // Player movement controller (gravity, slope, jump, sprint, click-to-move)
    rco::PlayerController player_ctrl{};
    glm::vec2 last_player_pos{0.f}; // XZ position from previous frame (walk detection)
    uint32_t  pending_interact   = 0;     // RID of NPC we're walking toward to interact
    constexpr float kInteractRange = 5.f;

    inventory.on_swap = [&](int src, int dst) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(static_cast<uint8_t>(src));
        w.WriteU8(static_cast<uint8_t>(dst));
        conn.SendPacket(rco::net::kPInventorySwap, w);
    };

    inventory.on_use = [&](int slot) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(static_cast<uint8_t>(slot));
        conn.SendPacket(rco::net::kPUseItem, w);
    };

    quest_log.on_action = [&](uint8_t action, uint32_t quest_id) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(action);
        w.WriteU32(quest_id);
        conn.SendPacket(rco::net::kPQuestAction, w);
    };

    party_panel.on_action = [&](uint8_t action, const std::string& target_name) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(action);
        w.WriteString(target_name);
        conn.SendPacket(rco::net::kPPartyAction, w);
    };

    skill_loadout_screen.on_set_slot = [&](uint32_t kit_id, uint8_t slot_index, uint32_t ability_id) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(rco::net::kSkillLoadoutActionSetSlot);
        w.WriteU32(kit_id);
        w.WriteU8(slot_index);
        w.WriteU32(ability_id);
        conn.SendPacket(rco::net::kPSkillLoadoutAction, w);
    };
    skill_loadout_screen.on_clear_slot = [&](uint32_t kit_id, uint8_t slot_index) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(rco::net::kSkillLoadoutActionClearSlot);
        w.WriteU32(kit_id);
        w.WriteU8(slot_index);
        w.WriteU32(0);
        conn.SendPacket(rco::net::kPSkillLoadoutAction, w);
    };
    skill_loadout_screen.on_clear_kit = [&](uint32_t kit_id) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(rco::net::kSkillLoadoutActionClearKit);
        w.WriteU32(kit_id);
        w.WriteU8(0);
        w.WriteU32(0);
        conn.SendPacket(rco::net::kPSkillLoadoutAction, w);
    };

    auto send_combat_action = [&](uint8_t action, uint32_t target_rid) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(action);
        w.WriteU32(target_rid);
        conn.SendPacket(rco::net::kPCombatAction, w);
    };

    auto send_cast_skill_slot = [&](uint8_t slot_index, uint32_t target_rid) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU8(1); // version
        w.WriteU8(slot_index);
        w.WriteU32(target_rid);
        conn.SendPacket(rco::net::kPCastSkillSlot, w);
    };

    auto rebind_player_anim_controller = [&]() -> bool {
        if (player_anims.empty()) return false;
        std::vector<rco::anim::AnimBinding> bindings;
        bindings.reserve(player_anims.size());
        for (const auto& wa : player_anims) {
            rco::anim::AnimBinding ab;
            ab.action      = wa.action;
            ab.source_path = wa.source_path;
            ab.start_frame = wa.start_frame;
            ab.end_frame   = wa.end_frame;
            ab.fps         = wa.fps;
            ab.loop        = wa.loop;
            ab.speed       = wa.speed;
            ab.blend_in    = wa.blend_in;
            ab.return_to   = wa.return_to;
            ab.priority    = wa.priority;
            for (const auto& ev : wa.events) {
                rco::anim::AnimEvent aev;
                aev.frame      = ev.frame;
                aev.event_type = ev.event_type;
                aev.payload    = ev.payload;
                ab.events.push_back(std::move(aev));
            }
            bindings.push_back(std::move(ab));
        }
        player_anim_ctrl.Bind(bindings);
        return player_anim_ctrl.RequestStateByName("Idle");
    };

    spellbar.on_cast = [&](uint16_t spell_id, uint32_t target_rid) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU16(spell_id);
        w.WriteU32(target_rid);
        w.WriteF32(0.f); // ground_x (unused for single/aoe-around-target)
        w.WriteF32(0.f); // ground_z
        conn.SendPacket(rco::net::kPCastSpell, w);

        // Trigger visual effect
        glm::vec3 from{player.x, player.y, player.z};
        glm::vec3 to = from;
        if (target_rid != 0) {
            auto it = world_actors.find(target_rid);
            if (it != world_actors.end())
                to = {it->second.x, terrain.SampleHeight(it->second.x, it->second.z), it->second.z};
        }
        rco::ui::SpellFxKind kind;
        switch (spell_id) {
            case 1:  kind = rco::ui::SpellFxKind::Fire;      break;
            case 2:  kind = rco::ui::SpellFxKind::Heal;      break;
            case 3:  kind = rco::ui::SpellFxKind::Lightning; break;
            default: kind = rco::ui::SpellFxKind::Fire;      break;
        }
        spell_fx.Add(from, to, static_cast<float>(glfwGetTime()), kind);

        // Play spell sound immediately on cast (client-side, no round-trip needed).
        switch (spell_id) {
            case 1:  audio.PlaySfx(rco::audio::SfxId::SpellFire);  break;
            case 2:  audio.PlaySfx(rco::audio::SfxId::SpellHeal);  break;
            case 3:  audio.PlaySfx(rco::audio::SfxId::SpellLight); break;
            default: break;
        }
    };

    spellbar.on_cast_ground = [&](uint16_t spell_id, float gx, float gz) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU16(spell_id);
        w.WriteU32(0);   // target_rid = 0 for ground AoE
        w.WriteF32(gx);
        w.WriteF32(gz);
        conn.SendPacket(rco::net::kPCastSpell, w);
        // Visual: explosion emitter at ground point (client-side preview)
        if (renderer_ready)
            particles.SpawnEmitter(rco::renderer::EmitterType::Explosion,
                                   {gx, terrain.SampleHeight(gx, gz), gz},
                                   static_cast<float>(glfwGetTime()), 0.f);
    };

    g_camera = &camera;
    glfwSetScrollCallback(window.Handle(), ScrollCallback);

    // -----------------------------------------------------------------------
    // Packet handler
    // -----------------------------------------------------------------------
    auto handle_packet = [&](const rco::net::InboundPacket& pkt) {
        rco::net::Reader r(pkt.payload.data(), pkt.payload.size());

        if (rco::gameplay::HandleIngamePacketGate(pkt.type, r)) {
            return;
        }

        switch (pkt.type) {

            case rco::net::kPLoginResult: {
                uint8_t     result = r.ReadU8();
                std::string msg    = r.ReadString();
                if (!r.OK()) break;
                if (result == rco::net::kResultOK) {
                    login_error.clear();
                    rco::net::Writer w;
                    conn.SendPacket(rco::net::kPFetchCharacter, w);
                } else {
                    login_error = msg.empty() ? "Login failed." : msg;
                }
                break;
            }

            case rco::net::kPPlayableDefs: {
                playable_defs.clear();
                uint8_t count = r.ReadU8();
                for (int i = 0; i < count && r.OK(); ++i) {
                    rco::PlayableDef d;
                    d.id        = r.ReadU16();
                    d.name      = r.ReadString();
                    d.race      = r.ReadString();
                    d.charClass = r.ReadString();
                    playable_defs.push_back(std::move(d));
                }
                break;
            }

            case rco::net::kPCharListResult: {
                characters.clear();
                uint8_t count = r.ReadU8();
                for (int i = 0; i < count && r.OK(); ++i) {
                    rco::CharacterInfo c;
                    c.slot        = static_cast<int>(r.ReadU8());
                    c.name        = r.ReadString();
                    c.race        = r.ReadString();
                    c.charClass   = r.ReadString();
                    c.level       = r.ReadU16();
                    c.area        = r.ReadString();
                    c.health      = static_cast<int32_t>(r.ReadU32());
                    c.healthMax   = static_cast<int32_t>(r.ReadU32());
                    c.actorDefID  = r.ReadU16();
                    characters.push_back(std::move(c));
                }
                state = rco::GameState::CharacterSelect;
                break;
            }

            case rco::net::kPCreateCharResult: {
                uint8_t     result = r.ReadU8();
                std::string msg    = r.ReadString();
                if (!r.OK()) break;
                if (result == rco::net::kResultOK) {
                    rco::net::Writer w;
                    conn.SendPacket(rco::net::kPFetchCharacter, w);
                } else {
                    login_error = msg.empty() ? "Could not create character." : msg;
                }
                break;
            }

            case rco::net::kPDeleteCharResult: {
                uint8_t     result = r.ReadU8();
                std::string msg    = r.ReadString();
                if (!r.OK()) break;
                if (result == rco::net::kResultOK) {
                    rco::net::Writer w;
                    conn.SendPacket(rco::net::kPFetchCharacter, w);
                } else {
                    login_error = msg.empty() ? "Could not delete character." : msg;
                }
                break;
            }

            case rco::net::kPStartGame: {
                perf_entry.LogPacketArrival("PStartGame");
                world_enter_pstart_tp = std::chrono::steady_clock::now();
                if (world_enter_pending) {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        world_enter_pstart_tp - world_enter_start_tp).count();
                    std::fprintf(stderr, "[perf-world-enter] phase=pstart_received ms=%lld\n",
                                 static_cast<long long>(ms));
                }
                player.runtimeId = r.ReadU32();
                player.areaName  = r.ReadString();
                player.x         = r.ReadF32();
                player.y         = r.ReadF32();
                player.z         = r.ReadF32();
                player.yaw       = r.ReadF32();
                player.health    = static_cast<int32_t>(r.ReadU32());
                player.healthMax = static_cast<int32_t>(r.ReadU32());
                player.mana      = static_cast<int32_t>(r.ReadU32());
                player.manaMax   = static_cast<int32_t>(r.ReadU32());
                player.stamina   = 100;
                player.staminaMax= 100;
                if (!r.Done()) {
                    player.stamina = static_cast<int32_t>(r.ReadU32());
                    player.staminaMax = static_cast<int32_t>(r.ReadU32());
                }
                if (!r.OK()) break;

                // Read appearance section (same format as PNewActor tail).
                player_meshes.clear();
                player_anims.clear();
                {
                    uint8_t nm = r.ReadU8();
                    for (uint8_t i = 0; i < nm && r.OK(); ++i) {
                        WorldMesh wm;
                        wm.slot       = r.ReadU8();
                        wm.model_path = r.ReadString();
                        wm.scale      = r.ReadF32();
                        wm.albedo     = r.ReadString();
                        wm.normal     = r.ReadString();
                        wm.orm        = r.ReadString();
                        wm.ar         = r.ReadF32();
                        wm.ag         = r.ReadF32();
                        wm.ab         = r.ReadF32();
                        wm.roughness  = r.ReadF32();
                        wm.metallic   = r.ReadF32();
                        uint8_t nmm   = r.ReadU8();
                        for (uint8_t j = 0; j < nmm && r.OK(); ++j) {
                            WorldAiMat wam;
                            wam.ai_name  = r.ReadString();
                            wam.albedo   = r.ReadString();
                            wam.normal   = r.ReadString();
                            wam.orm      = r.ReadString();
                            wam.ar       = r.ReadF32();
                            wam.ag       = r.ReadF32();
                            wam.ab       = r.ReadF32();
                            wam.roughness= r.ReadF32();
                            wam.metallic = r.ReadF32();
                            wm.material_map.push_back(std::move(wam));
                        }
                        player_meshes.push_back(std::move(wm));
                    }
                    // NEW format: binding_count(u16) then full AnimBinding per entry
                    uint16_t na = r.ReadU16();
                    for (uint16_t i = 0; i < na && r.OK(); ++i) {
                        WorldAnim wa;
                        wa.action        = r.ReadString();
                        wa.source_path   = r.ReadString();
                        wa.clip_override = r.ReadString();
                        wa.start_frame   = static_cast<int32_t>(r.ReadU32());
                        wa.end_frame     = static_cast<int32_t>(r.ReadU32());
                        wa.fps           = r.ReadF32();
                        wa.loop          = (r.ReadU8() != 0);
                        wa.speed         = r.ReadF32();
                        wa.blend_in      = r.ReadF32();
                        wa.return_to     = r.ReadString();
                        wa.priority      = r.ReadU8();
                        uint16_t ev_count = r.ReadU16();
                        for (uint16_t ei = 0; ei < ev_count && r.OK(); ++ei) {
                            WorldAnim::AnimEvent ev;
                            ev.frame      = static_cast<int32_t>(r.ReadU32());
                            ev.event_type = r.ReadString();
                            ev.payload    = r.ReadString();
                            wa.events.push_back(std::move(ev));
                        }
                        player_anims.push_back(std::move(wa));
                    }
                    player_yaw_offset = r.ReadF32();
                    player_y_offset   = r.ReadF32();
                }
                // Bind player AnimController from PStartGame appearance data
                {
                    std::vector<rco::anim::AnimBinding> bindings;
                    bindings.reserve(player_anims.size());
                    for (const auto& wa : player_anims) {
                        rco::anim::AnimBinding ab;
                        ab.action      = wa.action;
                        ab.source_path = wa.source_path;
                        ab.start_frame = wa.start_frame;
                        ab.end_frame   = wa.end_frame;
                        ab.fps         = wa.fps;
                        ab.loop        = wa.loop;
                        ab.speed       = wa.speed;
                        ab.blend_in    = wa.blend_in;
                        ab.return_to   = wa.return_to;
                        ab.priority    = wa.priority;
                        for (const auto& ev : wa.events) {
                            rco::anim::AnimEvent aev;
                            aev.frame      = ev.frame;
                            aev.event_type = ev.event_type;
                            aev.payload    = ev.payload;
                            ab.events.push_back(std::move(aev));
                        }
                        bindings.push_back(std::move(ab));
                    }
                    player_anim_ctrl.Bind(bindings);
                    player_anim_ctrl.RequestStateByName("Idle");
                }
                std::fprintf(stderr,
                    "[PStartGame] rid=%u player_meshes=%u player_anims=%u\n",
                    player.runtimeId,
                    (unsigned)player_meshes.size(),
                    (unsigned)player_anims.size());

                last_player_pos = {player.x, player.z};
                dodge_roll_active = false;
                dodge_roll_pending = false;
                world_corpses.clear();
                special_parry_telegraphs.clear();
                parry_judgements.clear();
                state = rco::GameState::InGame;
                world_entry_loading = true;
                world_entry_loading_start = glfwGetTime();
                world_entry_world_objects_received = false;
                world_entry_core_indices.clear();
                world_entry_core_cursor = 0;
                active_character_readability_tuning = character_readability_tuning;
                active_scene_look_tuning = scene_look_tuning;
                active_render_color_profile = render_color_profile;
                active_terrain_render_tuning = terrain_render_tuning;
                active_area_lighting =
                    ResolveAreaLightingProfile(player.areaName, area_lighting_fallback_config);
                area_environment_pending_apply = true;
                pending_area_skybox_hdr.clear();

                // Initialize InputSystem now that we have a live connection
                input_system = std::make_unique<rco::input::InputSystem>(
                    [&](const uint8_t* data, size_t len) {
                        conn.SendRaw(std::vector<uint8_t>(data, data + len));
                    }
                );
                // Load default bindings (hardcoded; later loaded from DB)
                {
                    using T = rco::input::TriggerType;
                    std::vector<rco::input::InputBinding> default_bindings = {
                        {"gameplay", "W",      "", T::Axis,  "MoveForward",  +1.f, true},
                        {"gameplay", "S",      "", T::Axis,  "MoveBack",     -1.f, true},
                        {"gameplay", "A",      "", T::Axis,  "MoveLeft",     -1.f, true},
                        {"gameplay", "D",      "", T::Axis,  "MoveRight",    +1.f, true},
                        {"gameplay", "Space",  "", T::Press, "Jump",          1.f, true},
                        {"gameplay", "Mouse1", "", T::Press, "Attack",        1.f, true},
                        {"gameplay", "Mouse2", "", T::Hold,  "Block",         1.f, true},
                        // OpenControls is non-remappable so the controls window can
                        // always be opened even after a bad rebind.
                        {"gameplay", "K",      "", T::Press, "OpenControls",  1.f, false},
                    };
                    input_system->LoadBindings(default_bindings);
                    // Apply any saved per-player overrides
                    std::string overrides_path = "users/" + player.name + "/input.json";
                    input_system->LoadLocalOverrides(overrides_path);
                }

                // Init controls UI with the new InputSystem
                controls_ui.Init(input_system.get());

                // Seed character sheet stats
                inventory.stat_name   = player.name;
                inventory.stat_race   = player.race;
                inventory.stat_class  = player.charClass;
                inventory.stat_hp     = player.health;
                inventory.stat_hp_max = player.healthMax;
                inventory.stat_mp     = player.mana;
                inventory.stat_mp_max = player.manaMax;
                inventory.stat_sp     = player.stamina;
                inventory.stat_sp_max = player.staminaMax;
                break;
            }

            case rco::net::kPChangeArea: {
                std::string area = r.ReadString();
                float cx = r.ReadF32(), cy = r.ReadF32(),
                      cz = r.ReadF32(), cyaw = r.ReadF32();
                if (!r.OK()) break;
                player.areaName = area;
                player.x = cx; player.y = cy;
                player.z = cz; player.yaw = cyaw;
                last_player_pos = {player.x, player.z};
                world_actors.clear();
                world_corpses.clear();
                world_static_objects.clear();
                world_entry_loading = false;
                world_entry_world_objects_received = false;
                world_entry_core_indices.clear();
                world_entry_core_cursor = 0;
                static_model_prewarm_queue.clear();
                static_model_prewarm_cursor = 0;
                area_portals.clear();
                world_items.clear();
                shop.open = false;
                combat_target = 0;
                special_parry_telegraphs.clear();
                parry_judgements.clear();
                active_character_readability_tuning = character_readability_tuning;
                active_scene_look_tuning = scene_look_tuning;
                active_render_color_profile = render_color_profile;
                active_terrain_render_tuning = terrain_render_tuning;
                active_area_lighting =
                    ResolveAreaLightingProfile(player.areaName, area_lighting_fallback_config);
                area_environment_pending_apply = true;
                pending_area_skybox_hdr.clear();
                spellbar.Clear();
                spell_fx.Clear();
                chat_bubbles.Clear();
                dialog.open = false;
                // Reload editor-painted terrain + collision volumes for the new area
                if (renderer_ready) {
                    terrain.SetRenderTuning(active_terrain_render_tuning);
                    terrain.LoadFromEditor(area);
                }
                col_data = rco::renderer::LoadColData(area);
                player_ctrl.Reset();
                dodge_roll_active = false;
                dodge_roll_pending = false;
                // Server will send PNewActor + PKnownSpells packets for the new area.
                break;
            }

            case rco::net::kPNewActor: {
                uint32_t    rid   = r.ReadU32();
                std::string name  = r.ReadString();
                /*race*/           r.ReadString();
                /*class*/          r.ReadString();
                uint16_t    level = r.ReadU16();
                float x = r.ReadF32(), y = r.ReadF32(),
                      z = r.ReadF32(), yaw = r.ReadF32();
                int32_t hp       = static_cast<int32_t>(r.ReadU32());
                int32_t hpmax    = static_cast<int32_t>(r.ReadU32());
                uint8_t atype    = r.ReadU8();

                // Appearance — variable-length section.
                struct IncomingAiMat {
                    std::string ai_name, albedo, normal, orm;
                    float       ar, ag, ab, roughness, metallic;
                };
                struct IncomingMesh {
                    uint8_t     slot;
                    std::string model_path;
                    float       scale;
                    std::string albedo, normal, orm;
                    float       ar, ag, ab, roughness, metallic;
                    std::vector<IncomingAiMat> material_map;
                };
                struct IncomingAnim {
                    std::string action, source_path, clip_override, return_to;
                    int32_t     start_frame = 0, end_frame = -1;
                    float       fps = 30.f, speed = 1.f, blend_in = 0.15f;
                    uint8_t     loop = 1, priority = 0;
                    struct Ev { int32_t frame; std::string type, payload; };
                    std::vector<Ev> events;
                };
                uint8_t num_meshes = r.ReadU8();
                std::vector<IncomingMesh> meshes;
                meshes.reserve(num_meshes);
                for (uint8_t i = 0; i < num_meshes && r.OK(); ++i) {
                    IncomingMesh m;
                    m.slot       = r.ReadU8();
                    m.model_path = r.ReadString();
                    m.scale      = r.ReadF32();
                    m.albedo     = r.ReadString();
                    m.normal     = r.ReadString();
                    m.orm        = r.ReadString();
                    m.ar         = r.ReadF32();
                    m.ag         = r.ReadF32();
                    m.ab         = r.ReadF32();
                    m.roughness  = r.ReadF32();
                    m.metallic   = r.ReadF32();
                    uint8_t nmm  = r.ReadU8();
                    m.material_map.reserve(nmm);
                    for (uint8_t j = 0; j < nmm && r.OK(); ++j) {
                        IncomingAiMat am;
                        am.ai_name  = r.ReadString();
                        am.albedo   = r.ReadString();
                        am.normal   = r.ReadString();
                        am.orm      = r.ReadString();
                        am.ar       = r.ReadF32();
                        am.ag       = r.ReadF32();
                        am.ab       = r.ReadF32();
                        am.roughness= r.ReadF32();
                        am.metallic = r.ReadF32();
                        m.material_map.push_back(std::move(am));
                    }
                    meshes.push_back(std::move(m));
                }
                // NEW format: binding_count(u16) then full AnimBinding per entry
                uint16_t binding_count = r.ReadU16();
                std::vector<IncomingAnim> anims;
                anims.reserve(binding_count);
                for (uint16_t bi = 0; bi < binding_count && r.OK(); ++bi) {
                    IncomingAnim a;
                    a.action        = r.ReadString();
                    a.source_path   = r.ReadString();
                    a.clip_override = r.ReadString();
                    a.start_frame   = static_cast<int32_t>(r.ReadU32());
                    a.end_frame   = static_cast<int32_t>(r.ReadU32());
                    a.fps         = r.ReadF32();
                    a.loop        = r.ReadU8();
                    a.speed       = r.ReadF32();
                    a.blend_in    = r.ReadF32();
                    a.return_to   = r.ReadString();
                    a.priority    = r.ReadU8();
                    uint16_t ev_count = r.ReadU16();
                    a.events.reserve(ev_count);
                    for (uint16_t ei = 0; ei < ev_count && r.OK(); ++ei) {
                        IncomingAnim::Ev ev;
                        ev.frame   = static_cast<int32_t>(r.ReadU32());
                        ev.type    = r.ReadString();
                        ev.payload = r.ReadString();
                        a.events.push_back(std::move(ev));
                    }
                    anims.push_back(std::move(a));
                }
                float actor_yaw_offset = r.ReadF32();
                float actor_y_offset   = r.ReadF32();
                if (!r.OK()) break;

                // If this is the local player's own PNewActor, store the
                // appearance data for use when initializing player_actor.
                if (rid == player.runtimeId) {
                    std::fprintf(stderr,
                        "[PNewActor-self] intercepted rid=%u meshes=%u — storing player appearance\n",
                        rid, (unsigned)meshes.size());
                    player_meshes.clear();
                    player_meshes.reserve(meshes.size());
                    for (auto& m : meshes) {
                        WorldMesh wm;
                        wm.slot       = m.slot;
                        wm.model_path = std::move(m.model_path);
                        wm.scale      = m.scale;
                        wm.albedo     = std::move(m.albedo);
                        wm.normal     = std::move(m.normal);
                        wm.orm        = std::move(m.orm);
                        wm.ar = m.ar; wm.ag = m.ag; wm.ab = m.ab;
                        wm.roughness = m.roughness;
                        wm.metallic  = m.metallic;
                        wm.material_map.reserve(m.material_map.size());
                        for (auto& am : m.material_map) {
                            WorldAiMat wam;
                            wam.ai_name = std::move(am.ai_name);
                            wam.albedo  = std::move(am.albedo);
                            wam.normal  = std::move(am.normal);
                            wam.orm     = std::move(am.orm);
                            wam.ar = am.ar; wam.ag = am.ag; wam.ab = am.ab;
                            wam.roughness = am.roughness;
                            wam.metallic  = am.metallic;
                            wm.material_map.push_back(std::move(wam));
                        }
                        player_meshes.push_back(std::move(wm));
                    }
                    player_anims.clear();
                    player_anims.reserve(anims.size());
                    for (auto& a : anims) {
                        WorldAnim wa;
                        wa.action        = a.action;
                        wa.source_path   = a.source_path;
                        wa.clip_override = a.clip_override;
                        wa.start_frame   = a.start_frame;
                        wa.end_frame     = a.end_frame;
                        wa.fps           = a.fps;
                        wa.loop          = (a.loop != 0);
                        wa.speed         = a.speed;
                        wa.blend_in      = a.blend_in;
                        wa.return_to     = a.return_to;
                        wa.priority      = a.priority;
                        for (auto& ev : a.events) {
                            WorldAnim::AnimEvent wev;
                            wev.frame      = ev.frame;
                            wev.event_type = ev.type;
                            wev.payload    = ev.payload;
                            wa.events.push_back(std::move(wev));
                        }
                        player_anims.push_back(std::move(wa));
                    }
                    // Build AnimController bindings for the player
                    {
                        std::vector<rco::anim::AnimBinding> bindings;
                        bindings.reserve(player_anims.size());
                        for (const auto& wa : player_anims) {
                            rco::anim::AnimBinding ab;
                            ab.action      = wa.action;
                            ab.source_path = wa.source_path;
                            ab.start_frame = wa.start_frame;
                            ab.end_frame   = wa.end_frame;
                            ab.fps         = wa.fps;
                            ab.loop        = wa.loop;
                            ab.speed       = wa.speed;
                            ab.blend_in    = wa.blend_in;
                            ab.return_to   = wa.return_to;
                            ab.priority    = wa.priority;
                            for (const auto& ev : wa.events) {
                                rco::anim::AnimEvent aev;
                                aev.frame      = ev.frame;
                                aev.event_type = ev.event_type;
                                aev.payload    = ev.payload;
                                ab.events.push_back(std::move(aev));
                            }
                            bindings.push_back(std::move(ab));
                        }
                        player_anim_ctrl.Bind(bindings);
                        player_anim_ctrl.RequestStateByName("Idle");
                    }
                    player_yaw_offset = actor_yaw_offset;
                    player_y_offset   = actor_y_offset;
                    // Re-init player_actor if renderer is already up.
                    if (renderer_ready && !player_meshes.empty()) {
                        const WorldMesh& body = player_meshes[0];
                        player_actor.Init("shaders",
                                          body.model_path.c_str(),
                                          &engine.materials());
                        player_actor.SetReadabilityProfile(
                            rco::renderer::Actor::ReadabilityProfile::Character);
                        player_actor.scale      = body.scale > 0.f ? body.scale : 1.f;
                        player_actor.yaw_offset = player_yaw_offset;
                        player_actor.y_offset   = player_y_offset;
                        if (player_actor.IsLoaded() && !body.material_map.empty()) {
                            std::unordered_map<std::string,
                                rco::renderer::Model::MaterialPaths> by_name;
                            for (const auto& am : body.material_map) {
                                rco::renderer::Model::MaterialPaths mp;
                                mp.albedo = am.albedo;
                                mp.normal = am.normal;
                                mp.orm    = am.orm;
                                by_name[am.ai_name] = std::move(mp);
                            }
                            player_actor.ApplyMaterialsByName(engine.materials(), by_name);
                        }
                        for (auto& wa : player_anims) {
                            if (!wa.source_path.empty()) {
                                player_actor.LoadAnim(wa.source_path.c_str(),
                                                      wa.action.c_str());
                                player_anim_ctrl.SetClipDuration(
                                    wa.action, player_actor.ClipDuration(wa.action));
                            }
                        }
                        for (auto& wa : player_anims) {
                            if (!wa.clip_override.empty())
                                player_actor.AliasClip(wa.clip_override, wa.action);
                        }
                        for (auto& wa : player_anims) {
                            player_anim_ctrl.SetClipDuration(
                                wa.action, player_actor.ClipDuration(wa.action));
                        }
                        engine.MarkMaterialsDirty();
                        camera.SetActorHeight(player_actor.ModelHeight());
                    }
                    break;
                }

                auto& e = world_actors[rid];  // in-place; avoids copy (actor is unique_ptr)
                e.x = x; e.y = y; e.z = z; e.yaw = yaw;
                e.prev_x = x; e.prev_z = z;
                e.name = name; e.level = level;
                e.health = hp; e.health_max = hpmax;
                e.actor_type = atype;
                e.anim_name  = "Idle";
                e.anim_t     = 0.f;
                e.yaw_offset = actor_yaw_offset;
                e.y_offset   = actor_y_offset;

                // Store appearance; the renderer-side Actor is created lazily
                // on first render (see render loop) because renderer_ready is
                // typically false when PNewActor arrives right after PStartGame.
                e.meshes.clear();
                e.meshes.reserve(meshes.size());
                for (auto& m : meshes) {
                    WorldMesh wm;
                    wm.slot       = m.slot;
                    wm.model_path = std::move(m.model_path);
                    wm.scale      = m.scale;
                    wm.albedo     = std::move(m.albedo);
                    wm.normal     = std::move(m.normal);
                    wm.orm        = std::move(m.orm);
                    wm.ar = m.ar; wm.ag = m.ag; wm.ab = m.ab;
                    wm.roughness = m.roughness;
                    wm.metallic  = m.metallic;
                    wm.material_map.reserve(m.material_map.size());
                    for (auto& am : m.material_map) {
                        WorldAiMat wam;
                        wam.ai_name = std::move(am.ai_name);
                        wam.albedo  = std::move(am.albedo);
                        wam.normal  = std::move(am.normal);
                        wam.orm     = std::move(am.orm);
                        wam.ar = am.ar; wam.ag = am.ag; wam.ab = am.ab;
                        wam.roughness = am.roughness;
                        wam.metallic  = am.metallic;
                        wm.material_map.push_back(std::move(wam));
                    }
                    e.meshes.push_back(std::move(wm));
                }
                e.anims.clear();
                e.anims.reserve(anims.size());
                for (auto& a : anims) {
                    WorldAnim wa;
                    wa.action        = a.action;
                    wa.source_path   = a.source_path;
                    wa.clip_override = a.clip_override;
                    wa.start_frame   = a.start_frame;
                    wa.end_frame     = a.end_frame;
                    wa.fps           = a.fps;
                    wa.loop          = (a.loop != 0);
                    wa.speed         = a.speed;
                    wa.blend_in      = a.blend_in;
                    wa.return_to     = a.return_to;
                    wa.priority      = a.priority;
                    for (auto& ev : a.events) {
                        WorldAnim::AnimEvent wev;
                        wev.frame      = ev.frame;
                        wev.event_type = ev.type;
                        wev.payload    = ev.payload;
                        wa.events.push_back(std::move(wev));
                    }
                    e.anims.push_back(std::move(wa));
                }
                // Bind AnimController for this actor
                {
                    std::vector<rco::anim::AnimBinding> bindings;
                    bindings.reserve(e.anims.size());
                    for (const auto& wa : e.anims) {
                        rco::anim::AnimBinding ab;
                        ab.action      = wa.action;
                        ab.source_path = wa.source_path;
                        ab.start_frame = wa.start_frame;
                        ab.end_frame   = wa.end_frame;
                        ab.fps         = wa.fps;
                        ab.loop        = wa.loop;
                        ab.speed       = wa.speed;
                        ab.blend_in    = wa.blend_in;
                        ab.return_to   = wa.return_to;
                        ab.priority    = wa.priority;
                        for (const auto& ev : wa.events) {
                            rco::anim::AnimEvent aev;
                            aev.frame      = ev.frame;
                            aev.event_type = ev.event_type;
                            aev.payload    = ev.payload;
                            ab.events.push_back(std::move(aev));
                        }
                        bindings.push_back(std::move(ab));
                    }
                    e.anim_ctrl.Bind(bindings);
                    e.anim_ctrl.RequestStateByName("Idle");
                }
                // Drop any pre-existing Actor — it'll be rebuilt on next render
                // against the (possibly new) appearance data.
                e.actor.reset();

                break;
            }

            case rco::net::kPActorGone: {
                uint32_t rid = r.ReadU32();
                if (!r.OK()) break;
                world_actors.erase(rid);
                special_parry_telegraphs.erase(rid);
                for (auto it = special_parry_telegraphs.begin();
                     it != special_parry_telegraphs.end();) {
                    if (it->second.target_rid == rid) {
                        it = special_parry_telegraphs.erase(it);
                    } else {
                        ++it;
                    }
                }
                break;
            }

            case rco::net::kPStandardUpdate: {
                uint32_t rid = r.ReadU32();
                float x = r.ReadF32(), y = r.ReadF32(),
                      z = r.ReadF32(), yaw = r.ReadF32();
                r.ReadU8(); // flags (unused in Phase 2)
                if (!r.OK()) break;
                if (rid == player.runtimeId) {
                    // Server echo — local prediction already applied, ignore.
                } else {
                    auto it = world_actors.find(rid);
                    if (it != world_actors.end()) {
                        it->second.x   = x;
                        it->second.y   = y;
                        it->second.z   = z;
                        it->second.yaw = yaw;
                    }
                }
                break;
            }

            case rco::net::kPKickedPlayer: {
                std::string reason = r.ReadString();
                std::fprintf(stderr, "[net] Kicked: %s\n", reason.c_str());
                conn.Disconnect();
                state       = rco::GameState::Login;
                login_error = reason.empty() ? "Disconnected by server." : reason;
                characters.clear();
                world_actors.clear();
                world_corpses.clear();
                world_static_objects.clear();
                world_entry_loading = false;
                world_entry_world_objects_received = false;
                world_entry_core_indices.clear();
                world_entry_core_cursor = 0;
                static_model_prewarm_queue.clear();
                static_model_prewarm_cursor = 0;
                quest_log.Clear();
                quest_log.journal_visible = false;
                party_panel.Clear();
                rco::gameplay::MutablePlayerSkillState().Clear();
                rco::gameplay::MutableActiveKitPool().Clear();
                skill_loadout_screen.SetOpen(false);
                dodge_roll_active = false;
                dodge_roll_pending = false;
                special_parry_telegraphs.clear();
                parry_judgements.clear();
                renderer_ready = false;
                break;
            }

            case rco::net::kPChatMessage: {
                /*channel*/ r.ReadU8();
                std::string sender = r.ReadString();
                std::string text   = r.ReadString();
                if (!r.OK()) break;
                chat.AddMessage(sender, text);
                // Attach speech bubble to the speaker.
                float bx = 0.f, by = 0.f, bz = 0.f;
                bool found = false;
                if (sender == player.name) {
                    bx = player.x; by = player.y; bz = player.z; found = true;
                } else {
                    for (auto& [rid, e] : world_actors) {
                        if (e.name == sender) {
                            bx = e.x;
                            by = e.y;   // chat bubble sits above the actor's actual Y
                            bz = e.z;
                            found = true;
                            break;
                        }
                    }
                }
                if (found)
                    chat_bubbles.Add(bx, by + 2.3f, bz, text, static_cast<float>(glfwGetTime()));
                break;
            }

            case rco::net::kPInventoryUpdate: {
                inventory.Clear();
                inventory.gold = static_cast<int64_t>(player_gold);
                uint8_t count = r.ReadU8();
                for (uint8_t i = 0; i < count; ++i) {
                    uint8_t     slot   = r.ReadU8();
                    uint16_t    iid    = r.ReadU16();
                    uint8_t     qty    = r.ReadU8();
                    uint8_t     dur    = r.ReadU8();
                    std::string name   = r.ReadString();
                    uint8_t     itype  = r.ReadU8();
                    uint8_t     stype  = r.ReadU8();
                    int16_t     wdmg   = static_cast<int16_t>(r.ReadU16());
                    int16_t     armor  = static_cast<int16_t>(r.ReadU16());
                    if (!r.OK()) break;
                    inventory.SetSlot(slot, iid, qty, dur, name, itype, stype, wdmg, armor);
                }
                break;
            }

            case rco::net::kPAttackActor: {
                char mode = static_cast<char>(r.ReadU8());
                if (mode == 'H') {
                    // We hit someone: [targetRID u32][damage+1 u16][dmgType u8][isCrit u8]
                    r.ReadU32(); // targetRID (we already track via FloatingNumber)
                    r.ReadU16(); // damage+1
                    r.ReadU8();  // dmgType
                    r.ReadU8();  // isCrit
                } else if (mode == 'Y') {
                    // We were hit: [attackerRID u32][damage+1 u16][dmgType u8][isCrit u8]
                    r.ReadU32(); r.ReadU16(); r.ReadU8(); r.ReadU8();
                }
                // 'O' observer packets carry no data we need beyond mode
                break;
            }

            case rco::net::kPCombatEvent: {
                uint8_t event_code = r.ReadU8();
                uint32_t source_rid = r.ReadU32();
                uint32_t target_rid = r.ReadU32();
                int16_t value = static_cast<int16_t>(r.ReadU16());
                std::string text = r.ReadString();
                if (!r.OK()) break;
                const double event_now = glfwGetTime();
                bool telegraph_meta_consumed = false;
                CombatTelegraphMeta telegraph_meta{};
                if (event_code == rco::net::kCombatEventSpecialWindup && !text.empty()) {
                    telegraph_meta = ParseCombatTelegraphMeta(text);
                    telegraph_meta_consumed = telegraph_meta.valid;
                }
                if (event_code == rco::net::kCombatEventSpecialWindup && source_rid != 0) {
                    const double windup_s =
                        (value > 0) ? (static_cast<double>(value) / 1000.0) : 1.2;
                    SpecialParryTelegraph tg;
                    tg.target_rid = target_rid;
                    tg.start_time = event_now;
                    const double windup_clamped = (windup_s < 0.25) ? 0.25 : windup_s;
                    tg.end_time = event_now + windup_clamped;
                    if (telegraph_meta.radius > 0.01f) {
                        tg.radius = telegraph_meta.radius;
                    }
                    if (telegraph_meta.outer_color != 0) {
                        tg.outer_color = telegraph_meta.outer_color;
                    }
                    if (telegraph_meta.parry_window_ms > 0) {
                        tg.parry_window_ms = telegraph_meta.parry_window_ms;
                    }
                    if (!telegraph_meta.reason_tag.empty()) {
                        tg.reason_tag = telegraph_meta.reason_tag;
                    }
                    tg.inner_color = DefaultInnerTelegraphColorForReason(tg.reason_tag);
                    special_parry_telegraphs[source_rid] = tg;
                } else if ((event_code == rco::net::kCombatEventSpecialParry ||
                            event_code == rco::net::kCombatEventSpecialHit) &&
                           source_rid != 0) {
                    special_parry_telegraphs.erase(source_rid);
                }
                if ((event_code == rco::net::kCombatEventSpecialParry ||
                     event_code == rco::net::kCombatEventSpecialHit) &&
                    target_rid == player.runtimeId) {
                    ParryJudgementFx fx;
                    fx.source_rid = source_rid;
                    fx.success = (event_code == rco::net::kCombatEventSpecialParry);
                    fx.metric = static_cast<int32_t>(value);
                    fx.start_time = event_now;
                    fx.end_time = event_now + (fx.success ? 0.85 : 0.78);
                    parry_judgements.push_back(fx);
                }

                // Authoritative dodge-roll trigger:
                // only start local roll when the server accepts dodge.
                if (event_code == rco::net::kCombatEventDodgeStarted &&
                    source_rid == player.runtimeId) {
                    glm::vec2 chosen_dir{0.f, 0.f};
                    if (dodge_roll_pending &&
                        event_now <= dodge_roll_pending_until &&
                        glm::dot(dodge_roll_pending_dir, dodge_roll_pending_dir) > 0.0001f) {
                        chosen_dir = glm::normalize(dodge_roll_pending_dir);
                    } else {
                        float yr = glm::radians(player.yaw);
                        chosen_dir = {-std::sin(yr), -std::cos(yr)}; // fallback: forward roll
                    }
                    dodge_roll_dir = chosen_dir;
                    dodge_roll_start = event_now;
                    const float iframe_s = (value > 0) ? (static_cast<float>(value) / 1000.f) : 0.28f;
                    const float roll_s = std::clamp(iframe_s + 0.08f, 0.24f, 0.45f);
                    dodge_roll_end = event_now + static_cast<double>(roll_s);
                    dodge_roll_active = true;
                    dodge_roll_pending = false;
                } else if (event_code == rco::net::kCombatEventActionRejected &&
                           source_rid == player.runtimeId &&
                           static_cast<uint8_t>(value) == rco::net::kCombatActionDodge) {
                    dodge_roll_active = false;
                    dodge_roll_pending = false;
                }

                auto actor_name = [&](uint32_t rid) -> std::string {
                    if (rid == 0) return std::string("Unknown");
                    if (rid == player.runtimeId) return player.name.empty() ? std::string("You") : player.name;
                    auto it = world_actors.find(rid);
                    if (it != world_actors.end() && !it->second.name.empty()) return it->second.name;
                    return std::string("Actor#") + std::to_string(rid);
                };

                if (!text.empty() && !telegraph_meta_consumed) {
                    chat.AddMessage("", text);
                    break;
                }

                std::string msg;
                switch (event_code) {
                    case rco::net::kCombatEventActionRejected:
                        msg = "Combat action rejected by server.";
                        break;
                    case rco::net::kCombatEventDodgeStarted:
                        msg = actor_name(source_rid) + " used Dodge.";
                        break;
                    case rco::net::kCombatEventGuardStarted:
                        msg = actor_name(source_rid) + " raised Guard.";
                        break;
                    case rco::net::kCombatEventGuardEnded:
                        msg = actor_name(source_rid) + " lowered Guard.";
                        break;
                    case rco::net::kCombatEventParryStarted:
                        msg = actor_name(source_rid) + " opened a Parry window.";
                        break;
                    case rco::net::kCombatEventParryEnded:
                        msg = actor_name(source_rid) + " ended Parry.";
                        break;
                    case rco::net::kCombatEventInterruptSuccess:
                        msg = actor_name(source_rid) + " interrupted " + actor_name(target_rid) + ".";
                        break;
                    case rco::net::kCombatEventHitDodged:
                        msg = actor_name(source_rid) + " dodged an incoming hit.";
                        break;
                    case rco::net::kCombatEventHitGuarded:
                        msg = actor_name(source_rid) + " guarded and took " + std::to_string(value) + " damage.";
                        break;
                    case rco::net::kCombatEventHitParried:
                        msg = actor_name(source_rid) + " parried an incoming hit.";
                        break;
                    case rco::net::kCombatEventSpecialWindup:
                        msg = actor_name(source_rid) + " is charging a special attack. Parry at the last moment.";
                        if (auto it = special_parry_telegraphs.find(source_rid);
                            it != special_parry_telegraphs.end()) {
                            const std::string phase_label =
                                TelegraphPhaseLabelFromReason(it->second.reason_tag);
                            if (!phase_label.empty()) {
                                msg += " [" + phase_label + "]";
                            }
                        }
                        break;
                    case rco::net::kCombatEventSpecialParry:
                        msg = actor_name(target_rid) + " parried " + actor_name(source_rid) + "'s special.";
                        if (value > 0) {
                            msg += " [timing " + std::to_string(value) + "ms]";
                        }
                        break;
                    case rco::net::kCombatEventSpecialHit:
                        msg = actor_name(source_rid) + " landed a special hit on " +
                              actor_name(target_rid) + " for " + std::to_string(value) + " damage.";
                        break;
                    default:
                        msg = "Combat event received.";
                        break;
                }
                chat.AddMessage("", msg);
                break;
            }

            case rco::net::kPActorDead: {
                uint32_t dead_rid   = r.ReadU32();
                /*killer*/ r.ReadU32();
                if (!r.OK()) break;
                if (dead_rid == player.runtimeId) {
                    player_dead    = true;
                    player.health  = 0;
                    combat_target  = 0;
                    dodge_roll_active = false;
                    dodge_roll_pending = false;
                    if (player_anim_ctrl.IsReady()) {
                        player_anim_ctrl.RequestStateByName("Death");
                    } else {
                        player_actor.PlayAnim("Death", false);
                    }
                    special_parry_telegraphs.clear();
                    parry_judgements.clear();
                    audio.PlaySfx(rco::audio::SfxId::PlayerDeath);
                } else {
                    auto wit = world_actors.find(dead_rid);
                    if (wit != world_actors.end()) {
                        const double corpse_now = glfwGetTime();
                        CorpseEntry corpse;
                        corpse.rid = dead_rid;
                        corpse.actor = std::move(wit->second);
                        corpse.death_time = corpse_now;
                        corpse.sink_start_time = corpse_now + kCorpseLieTimeSec;
                        corpse.remove_time = corpse.sink_start_time + kCorpseSinkDurationSec;
                        corpse.base_y = corpse.actor.y;
                        corpse.sink_depth = kCorpseSinkDepth;
                        if (corpse.actor.anim_ctrl.IsReady()) {
                            corpse.actor.anim_ctrl.RequestStateByName("Death");
                        } else if (corpse.actor.actor) {
                            corpse.actor.actor->PlayAnim("Death", false);
                        }
                        world_corpses.push_back(std::move(corpse));
                        world_actors.erase(wit);
                    }
                    special_parry_telegraphs.erase(dead_rid);
                    for (auto it = special_parry_telegraphs.begin();
                         it != special_parry_telegraphs.end();) {
                        if (it->second.target_rid == dead_rid) {
                            it = special_parry_telegraphs.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    if (combat_target == dead_rid) combat_target = 0;
                    audio.PlaySfx(rco::audio::SfxId::NPCDeath);
                }
                break;
            }

            case rco::net::kPRepositionActor: {
                uint32_t rid = r.ReadU32();
                float rx = r.ReadF32(), ry = r.ReadF32(),
                      rz = r.ReadF32(), ryaw = r.ReadF32();
                if (!r.OK()) break;
                if (rid == player.runtimeId) {
                    const bool was_dead = player_dead;
                    player.x = rx; player.y = ry;
                    player.z = rz; player.yaw = ryaw;
                    last_player_pos = {rx, rz};
                    player_dead = false;
                    player_ctrl.Reset();
                    dodge_roll_active = false;
                    dodge_roll_pending = false;
                    if (was_dead) {
                        bool reset_ok = false;
                        if (player_anim_ctrl.IsReady()) {
                            reset_ok = player_anim_ctrl.RequestStateByName("Idle");
                        }
                        if (!reset_ok) {
                            reset_ok = rebind_player_anim_controller();
                        }
                        if (!reset_ok) {
                            player_actor.PlayAnim("Idle", true);
                        }
                    }
                } else {
                    auto it = world_actors.find(rid);
                    if (it != world_actors.end()) {
                        it->second.x = rx; it->second.y = ry;
                        it->second.z = rz; it->second.yaw = ryaw;
                        it->second.prev_x = rx; it->second.prev_z = rz;
                    }
                }
                break;
            }

            case rco::net::kPAnimateActor: {
                uint32_t rid       = r.ReadU32();
                uint8_t  action_id = r.ReadU8();
                if (!r.OK()) break;
                // Local player
                if (rid == player.runtimeId) {
                    bool is_death_action = false;
                    if (action_id < player_anims.size()) {
                        std::string act = player_anims[action_id].action;
                        std::transform(act.begin(), act.end(), act.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        is_death_action = (act == "death");
                    }
                    if (!player_dead && is_death_action) {
                        break; // stale death packet after respawn
                    }
                    player_anim_ctrl.RequestState(action_id);
                    break;
                }
                auto it = world_actors.find(rid);
                if (it != world_actors.end()) {
                    it->second.anim_ctrl.RequestState(action_id);
                    // Sync anim_name for backward compat with SubmitAs
                    if (it->second.anim_ctrl.IsReady()) {
                        it->second.anim_name = it->second.anim_ctrl.CurrentAction();
                    }
                    it->second.anim_t = 0.f;
                }
                break;
            }

            case rco::net::kPStatUpdate: {
                char mode = static_cast<char>(r.ReadU8());
                if (mode == 'A') {
                    uint32_t rid  = r.ReadU32();
                    uint8_t  attr = r.ReadU8();
                    int16_t  val  = static_cast<int16_t>(r.ReadU16());
                    if (!r.OK()) break;
                    if (rid == player.runtimeId) {
                        const bool was_dead = player_dead;
                        if      (attr == 0) { player.health    = val; if (val > 0) player_dead = false; }
                        else if (attr == 1) { player.healthMax = val; }
                        else if (attr == 2) { player.mana      = val; }
                        else if (attr == 3) { player.manaMax   = val; }
                        else if (attr == 4) { player.stamina   = val; }
                        else if (attr == 5) { player.staminaMax= val; }
                        if (was_dead && !player_dead) {
                            bool reset_ok = false;
                            if (player_anim_ctrl.IsReady()) {
                                reset_ok = player_anim_ctrl.RequestStateByName("Idle");
                            }
                            if (!reset_ok) {
                                reset_ok = rebind_player_anim_controller();
                            }
                            if (!reset_ok) {
                                player_actor.PlayAnim("Idle", true);
                            }
                        }
                        inventory.stat_hp     = player.health;
                        inventory.stat_hp_max = player.healthMax;
                        inventory.stat_mp     = player.mana;
                        inventory.stat_mp_max = player.manaMax;
                        inventory.stat_sp     = player.stamina;
                        inventory.stat_sp_max = player.staminaMax;
                    } else {
                        auto it = world_actors.find(rid);
                        if (it != world_actors.end()) {
                            if (attr == 0) it->second.health = val;
                            else if (attr == 1) it->second.health_max = val;
                        }
                    }
                }
                break;
            }

            case rco::net::kPFloatingNumber: {
                uint32_t target_rid = r.ReadU32();
                int16_t  dmg        = static_cast<int16_t>(r.ReadU16());
                bool     is_crit    = r.ReadU8() != 0;
                if (!r.OK()) break;
                // Resolve world position from target RID.
                float wx = 0, wy = 0, wz = 0;
                if (target_rid == player.runtimeId) {
                    wx = player.x; wy = player.y; wz = player.z;
                } else {
                    auto it = world_actors.find(target_rid);
                    if (it != world_actors.end()) {
                        wx = it->second.x;
                        wy = it->second.y;
                        wz = it->second.z;
                    }
                }
                float_nums.Add(wx, wy + 1.8f, wz,
                               dmg == -1 ? -1 : static_cast<int32_t>(dmg),
                               is_crit);
                break;
            }

            case rco::net::kPPortalInfo: {
                area_portals.clear();
                uint8_t count = r.ReadU8();
                for (uint8_t i = 0; i < count; ++i) {
                    PortalEntry p;
                    p.x      = r.ReadF32();
                    p.z      = r.ReadF32();
                    p.radius = r.ReadF32();
                    p.target_area = r.ReadString();
                    if (r.OK()) area_portals.push_back(std::move(p));
                }
                break;
            }

            case rco::net::kPWorldObjects: {
                perf_entry.LogPacketArrival("PWorldObjects");
                world_static_objects.clear();
                static_model_prewarm_queue.clear();
                static_model_prewarm_cursor = 0;
                uint16_t obj_count = r.ReadU16();
                world_static_objects.reserve(obj_count);
                std::unordered_set<std::string> unique_models;
                std::unordered_map<std::string, int> model_counts;
                unique_models.reserve(obj_count);
                for (uint16_t oi = 0; oi < obj_count && r.OK(); ++oi) {
                    WorldObjectEntry e;
                    e.model_path = r.ReadString();
                    e.scale      = r.ReadF32();
                    e.x          = r.ReadF32();
                    e.y          = r.ReadF32();
                    e.z          = r.ReadF32();
                    e.yaw        = r.ReadF32();
                    if (!r.OK() || e.model_path.empty()) break;
                    unique_models.insert(e.model_path);
                    ++model_counts[e.model_path];
                    world_static_objects.push_back(std::move(e));
                }
                static_model_prewarm_queue.reserve(unique_models.size());
                for (const auto& path : unique_models)
                    static_model_prewarm_queue.push_back(path);
                world_entry_world_objects_received = true;
                perf_entry.LogWorldObjectsSummary(obj_count, model_counts);
                break;
            }

            case rco::net::kPXPUpdate: {
                uint16_t lvl     = r.ReadU16();
                uint32_t xp      = r.ReadU32();
                uint32_t xp_next = r.ReadU32();
                if (!r.OK()) break;
                if (lvl > player.level && player.level > 0)
                    audio.PlaySfx(rco::audio::SfxId::LevelUp);
                player.level   = lvl;
                player.xp      = xp;
                player.xp_next = xp_next;
                inventory.stat_level   = lvl;
                inventory.stat_xp      = xp;
                inventory.stat_xp_next = xp_next;
                break;
            }

            case rco::net::kPQuestLog: {
                if (!quest_log.ApplyPacket(r)) {
                    std::fprintf(stderr, "[quest-log] malformed or unsupported payload\n");
                }
                break;
            }

            case rco::net::kPPartyUpdate: {
                if (!party_panel.ApplyPacket(r)) {
                    std::fprintf(stderr, "[party] malformed or unsupported payload\n");
                }
                break;
            }

            case rco::net::kPKnownSpells: {
                spellbar.Clear();
                uint8_t count = r.ReadU8();
                for (uint8_t i = 0; i < count; ++i) {
                    uint16_t    spell_id    = r.ReadU16();
                    std::string name        = r.ReadString();
                    uint8_t     spell_type  = r.ReadU8();
                    uint16_t    mp_cost     = r.ReadU16();
                    uint32_t    cooldown_ms = r.ReadU32();
                    float       range       = r.ReadF32();
                    /*icon*/                 r.ReadU8();
                    uint8_t     aoe_type    = r.ReadU8();
                    float       aoe_radius  = r.ReadF32();
                    if (!r.OK()) break;
                    spellbar.AddSpell(spell_id, name, spell_type, mp_cost, cooldown_ms,
                                      aoe_type, aoe_radius, range);
                }
                break;
            }

            case rco::net::kPSkillState: {
                auto& state = rco::gameplay::MutablePlayerSkillState();
                if (state.ApplyPacket(r)) {
                    skill_hotbar.OnSkillStateUpdated(state);
                }
                break;
            }

            case rco::net::kPKitPool: {
                auto& pool = rco::gameplay::MutableActiveKitPool();
                pool.ApplyPacket(r);
                break;
            }

            case rco::net::kPDialog: {
                dialog.npc_name = r.ReadString();
                dialog.text     = r.ReadString();
                uint8_t opt_count = r.ReadU8();
                dialog.options.clear();
                for (uint8_t i = 0; i < opt_count; ++i)
                    dialog.options.push_back(r.ReadString());
                if (r.OK()) dialog.open = true;
                break;
            }

            case rco::net::kPGoldChange: {
                player_gold = r.ReadU32();
                inventory.gold = static_cast<int64_t>(player_gold);
                break;
            }

            case rco::net::kPWorldItem: {
                WorldItemEntry wi;
                wi.rid      = r.ReadU32();
                wi.x        = r.ReadF32();
                wi.y        = r.ReadF32();
                wi.z        = r.ReadF32();
                wi.item_id  = r.ReadU16();
                wi.quantity = r.ReadU8();
                wi.name     = r.ReadString();
                wi.item_type = r.ReadU8();
                if (r.OK()) world_items.push_back(wi);
                break;
            }

            case rco::net::kPRemoveWorldItem: {
                uint32_t rid = r.ReadU32();
                world_items.erase(
                    std::remove_if(world_items.begin(), world_items.end(),
                        [rid](const WorldItemEntry& e){ return e.rid == rid; }),
                    world_items.end());
                break;
            }

            case rco::net::kPOpenShop: {
                shop.items.clear();
                uint8_t count = r.ReadU8();
                for (uint8_t i = 0; i < count; ++i) {
                    ShopEntry e;
                    e.item_id       = r.ReadU16();
                    e.name          = r.ReadString();
                    e.item_type     = r.ReadU8();
                    e.slot_type     = r.ReadU8();
                    e.weapon_damage = r.ReadU16();
                    e.armor_level   = r.ReadU16();
                    e.buy_price     = r.ReadU32();
                    e.sell_price    = r.ReadU32();
                    if (r.OK()) shop.items.push_back(e);
                }
                if (r.OK()) { shop.open = true; shop.tab = 0; }
                break;
            }

            case rco::net::kPSetInputContext: {
                std::string ctx_name = r.ReadString();
                if (!r.OK()) break;
                if (input_system) input_system->SetContext(ctx_name);
                break;
            }

            case rco::net::kPInputBindings: {
                // Read preset name (unused; always apply to current input_system)
                /*preset_name*/ r.ReadString();
                uint16_t count = r.ReadU16();
                if (!r.OK()) break;
                std::vector<rco::input::InputBinding> bindings;
                bindings.reserve(count);
                for (uint16_t i = 0; i < count; ++i) {
                    rco::input::InputBinding b;
                    b.context      = r.ReadString();
                    b.key          = r.ReadString();
                    b.modifier     = r.ReadString();
                    std::string tt = r.ReadString();
                    b.action       = r.ReadString();
                    b.axis_value   = r.ReadF32();
                    b.remappable   = r.ReadU8() != 0;
                    // Parse trigger_type string → enum
                    using T = rco::input::TriggerType;
                    if      (tt == "press")   b.trigger_type = T::Press;
                    else if (tt == "release") b.trigger_type = T::Release;
                    else if (tt == "hold")    b.trigger_type = T::Hold;
                    else if (tt == "double")  b.trigger_type = T::Double;
                    else if (tt == "axis")    b.trigger_type = T::Axis;
                    else                      b.trigger_type = T::Press;
                    bindings.push_back(std::move(b));
                }
                if (!r.OK()) break;
                if (input_system) {
                    input_system->LoadBindings(bindings);
                    // Apply any local per-player overrides
                    std::string overrides_path = "users/" + player.name + "/input.json";
                    input_system->LoadLocalOverrides(overrides_path);
                }
                break;
            }

            case rco::net::kPPing: {
                rco::net::Writer w;
                conn.SendPacket(rco::net::kPPong, w);
                break;
            }

            case rco::net::kPAreaConfig: {
                perf_entry.LogPacketArrival("PAreaConfig");
                std::string skybox_hdr = r.ReadString();
                if (!r.OK()) break;

                AreaLightingProfile server_light =
                    ResolveAreaLightingProfile(player.areaName, area_lighting_fallback_config);
                server_light.preset_name = "server_authoritative";
                auto server_char_readability = active_character_readability_tuning;
                auto server_scene_look = active_scene_look_tuning;
                auto server_render_color = active_render_color_profile;
                auto server_terrain_tuning = active_terrain_render_tuning;

                if (!r.Done()) {
                    server_light.sun_dir.x = r.ReadF32();
                    server_light.sun_dir.y = r.ReadF32();
                    server_light.sun_dir.z = r.ReadF32();
                    server_light.sun_color.r = r.ReadF32();
                    server_light.sun_color.g = r.ReadF32();
                    server_light.sun_color.b = r.ReadF32();
                    server_light.sun_intensity_mul = r.ReadF32();
                    server_light.sky_intensity_mul = r.ReadF32();
                    server_light.fog_density_mul = r.ReadF32();
                    server_light.fog_color.r = r.ReadF32();
                    server_light.fog_color.g = r.ReadF32();
                    server_light.fog_color.b = r.ReadF32();
                    server_light.volumetrics_default = r.ReadBool();
                }
                if (!r.Done()) {
                    server_char_readability.shadowLift = r.ReadF32();
                    server_char_readability.rimStrength = r.ReadF32();
                    server_char_readability.rimExponent = r.ReadF32();
                    server_char_readability.minNdotL = r.ReadF32();
                    server_char_readability.ambientBoost = r.ReadF32();
                }
                if (!r.Done()) {
                    server_scene_look.iblIntensity = r.ReadF32();
                    server_scene_look.skyIntensity = r.ReadF32();
                    server_scene_look.worldShadowLift = r.ReadF32();
                    server_scene_look.directScale = r.ReadF32();
                    server_scene_look.ambientScale = r.ReadF32();
                    server_scene_look.flatAmbient = r.ReadF32();
                    server_scene_look.worldMinNdotL = r.ReadF32();
                    server_scene_look.albedoMinLuma = r.ReadF32();
                    server_scene_look.albedoLiftStrength = r.ReadF32();
                    server_scene_look.specularScale = r.ReadF32();
                    server_scene_look.exposureFactor = r.ReadF32();
                    server_scene_look.sunIntensity = r.ReadF32();
                }
                if (!r.Done()) {
                    server_render_color.contrast = r.ReadF32();
                    server_render_color.saturation = r.ReadF32();
                    server_render_color.vibrance = r.ReadF32();
                    server_render_color.black_point = r.ReadF32();
                    server_render_color.vignette_strength = r.ReadF32();
                    server_render_color.vignette_softness = r.ReadF32();
                }
                if (!r.Done()) {
                    server_terrain_tuning.tiling_mul = r.ReadF32();
                    server_terrain_tuning.macro_strength_mul = r.ReadF32();
                    server_terrain_tuning.height_blend_slop = r.ReadF32();
                }
                if (!r.OK()) {
                    std::fprintf(stderr, "[area-config] malformed payload ignored\n");
                    break;
                }

                if (glm::dot(server_light.sun_dir, server_light.sun_dir) > 0.0001f) {
                    server_light.sun_dir = glm::normalize(server_light.sun_dir);
                } else {
                    server_light.sun_dir = glm::normalize(glm::vec3(0.18f, 0.96f, 0.20f));
                }
                server_light.sun_color.r = glm::clamp(server_light.sun_color.r, 0.0f, 2.0f);
                server_light.sun_color.g = glm::clamp(server_light.sun_color.g, 0.0f, 2.0f);
                server_light.sun_color.b = glm::clamp(server_light.sun_color.b, 0.0f, 2.0f);
                server_light.sun_intensity_mul = glm::clamp(server_light.sun_intensity_mul, 0.0f, 2.0f);
                server_light.sky_intensity_mul = glm::clamp(server_light.sky_intensity_mul, 0.0f, 2.0f);
                server_light.fog_density_mul = glm::clamp(server_light.fog_density_mul, 0.0f, 2.0f);
                server_light.fog_color.r = glm::clamp(server_light.fog_color.r, 0.0f, 2.0f);
                server_light.fog_color.g = glm::clamp(server_light.fog_color.g, 0.0f, 2.0f);
                server_light.fog_color.b = glm::clamp(server_light.fog_color.b, 0.0f, 2.0f);

                server_char_readability.shadowLift =
                    glm::clamp(server_char_readability.shadowLift, 0.0f, 1.0f);
                server_char_readability.rimStrength =
                    glm::clamp(server_char_readability.rimStrength, 0.0f, 1.0f);
                server_char_readability.rimExponent =
                    glm::clamp(server_char_readability.rimExponent, 1.0f, 6.0f);
                server_char_readability.minNdotL =
                    glm::clamp(server_char_readability.minNdotL, 0.0f, 0.5f);
                server_char_readability.ambientBoost =
                    glm::clamp(server_char_readability.ambientBoost, 0.0f, 0.5f);

                server_scene_look.iblIntensity = glm::clamp(server_scene_look.iblIntensity, 0.00f, 2.00f);
                server_scene_look.skyIntensity = glm::clamp(server_scene_look.skyIntensity, 0.00f, 2.00f);
                server_scene_look.worldShadowLift = glm::clamp(server_scene_look.worldShadowLift, 0.00f, 0.95f);
                server_scene_look.directScale = glm::clamp(server_scene_look.directScale, 0.00f, 2.00f);
                server_scene_look.ambientScale = glm::clamp(server_scene_look.ambientScale, 0.00f, 3.00f);
                server_scene_look.flatAmbient = glm::clamp(server_scene_look.flatAmbient, 0.00f, 2.00f);
                server_scene_look.worldMinNdotL = glm::clamp(server_scene_look.worldMinNdotL, 0.00f, 1.00f);
                server_scene_look.albedoMinLuma = glm::clamp(server_scene_look.albedoMinLuma, 0.00f, 1.00f);
                server_scene_look.albedoLiftStrength = glm::clamp(server_scene_look.albedoLiftStrength, 0.00f, 1.00f);
                server_scene_look.specularScale = glm::clamp(server_scene_look.specularScale, 0.00f, 2.00f);
                server_scene_look.exposureFactor = glm::clamp(server_scene_look.exposureFactor, 0.05f, 2.00f);
                server_scene_look.sunIntensity = glm::clamp(server_scene_look.sunIntensity, 0.00f, 2.00f);

                server_render_color.contrast = glm::clamp(server_render_color.contrast, 0.80f, 1.35f);
                server_render_color.saturation = glm::clamp(server_render_color.saturation, 0.80f, 1.40f);
                server_render_color.vibrance = glm::clamp(server_render_color.vibrance, -0.30f, 0.60f);
                server_render_color.black_point = glm::clamp(server_render_color.black_point, 0.00f, 0.06f);
                server_render_color.vignette_strength =
                    glm::clamp(server_render_color.vignette_strength, 0.00f, 0.20f);
                server_render_color.vignette_softness =
                    glm::clamp(server_render_color.vignette_softness, 0.00f, 1.00f);
                server_terrain_tuning.tiling_mul =
                    glm::clamp(server_terrain_tuning.tiling_mul, 0.50f, 2.50f);
                server_terrain_tuning.macro_strength_mul =
                    glm::clamp(server_terrain_tuning.macro_strength_mul, 0.00f, 3.00f);
                server_terrain_tuning.height_blend_slop =
                    glm::clamp(server_terrain_tuning.height_blend_slop, 0.02f, 0.70f);

                active_character_readability_tuning = server_char_readability;
                active_scene_look_tuning = server_scene_look;
                active_render_color_profile = server_render_color;
                active_terrain_render_tuning = server_terrain_tuning;
                active_area_lighting = server_light;
                area_environment_pending_apply = true;
                pending_area_skybox_hdr = skybox_hdr;
                std::fprintf(
                    stderr,
                    "[area-config] area=\"%s\" skybox=\"%s\" sun=(%.2f,%.2f,%.2f) sun_mul=%.2f sky_mul=%.2f fog_mul=%.2f volumetrics=%d exposure=%.2f contrast=%.2f terrain_tiling=%.2f terrain_macro=%.2f terrain_slop=%.2f\n",
                    player.areaName.c_str(),
                    skybox_hdr.c_str(),
                    active_area_lighting.sun_dir.x,
                    active_area_lighting.sun_dir.y,
                    active_area_lighting.sun_dir.z,
                    active_area_lighting.sun_intensity_mul,
                    active_area_lighting.sky_intensity_mul,
                    active_area_lighting.fog_density_mul,
                    active_area_lighting.volumetrics_default ? 1 : 0,
                    active_scene_look_tuning.exposureFactor,
                    active_render_color_profile.contrast,
                    active_terrain_render_tuning.tiling_mul,
                    active_terrain_render_tuning.macro_strength_mul,
                    active_terrain_render_tuning.height_blend_slop);
                if (renderer_ready) {
                    const std::string path = ResolveIblPathFromAreaConfig(pending_area_skybox_hdr);
                    const auto env_t0 = std::chrono::steady_clock::now();
                    engine.LoadEnvironment(path);
                    const auto env_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - env_t0).count());
                    std::string skipped_path;
                    if (engine.ConsumeLoadEnvironmentSkipped(&skipped_path)) {
                        perf_entry.LogLoadEnvironmentSkipped(
                            "area_config_immediate",
                            skipped_path.empty() ? path : skipped_path);
                    } else {
                        perf_entry.LogLoadEnvironment("area_config_immediate", path, env_us);
                    }
                    pending_area_skybox_hdr.clear();
                }
                break;
            }

            case rco::net::kPCreateEmitter: {
                uint8_t  type = r.ReadU8();
                float    ex   = r.ReadF32(), ey = r.ReadF32(), ez = r.ReadF32();
                uint16_t dur  = r.ReadU16();
                if (r.OK() && renderer_ready)
                    particles.SpawnEmitter(
                        static_cast<rco::renderer::EmitterType>(type),
                        {ex, ey, ez},
                        static_cast<float>(glfwGetTime()),
                        dur > 0 ? dur / 1000.f : 0.f);
                break;
            }

            case rco::net::kPSound: {
                uint8_t id  = r.ReadU8();
                uint8_t vol = r.ReadU8();
                if (r.OK()) audio.PlaySfx(id, vol / 255.f);
                break;
            }

            case rco::net::kPMusic: {
                uint8_t track = r.ReadU8();
                uint8_t vol   = r.ReadU8();
                if (!r.OK()) break;
                if (track == 0) audio.StopMusic();
                else            audio.PlayMusic(track, vol / 255.f);
                break;
            }

            default:
                break;
        }
    };

    // -----------------------------------------------------------------------
    // UI screens
    // -----------------------------------------------------------------------
    rco::ui::LoginScreen login_screen({
        .OnLogin = [&](const std::string& user, const std::string& pass) {
            if (!conn.IsConnected()) {
                if (!conn.Connect("127.0.0.1", 7777)) {
                    login_error = "Could not connect to server.";
                    return;
                }
            }
            rco::net::Writer w;
            w.WriteString(user);
            w.WriteString(pass);
            conn.SendPacket(rco::net::kPVerifyAccount, w);
        },
        .OnRegister = [&](const std::string& user,
                          const std::string& pass,
                          const std::string& email) {
            if (!conn.IsConnected()) {
                if (!conn.Connect("127.0.0.1", 7777)) {
                    login_error = "Could not connect to server.";
                    return;
                }
            }
            rco::net::Writer w;
            w.WriteString(user);
            w.WriteString(pass);
            w.WriteString(email);
            conn.SendPacket(rco::net::kPCreateAccount, w);
        }
    });

    rco::ui::CharSelect char_select({
        .OnSelect = [&](int slot) {
            for (auto& ch : characters) {
                if (ch.slot == slot) {
                    player.name      = ch.name;
                    player.race      = ch.race;
                    player.charClass = ch.charClass;
                    break;
                }
            }
            rco::net::Writer w;
            w.WriteU8(static_cast<uint8_t>(slot));
            conn.SendPacket(rco::net::kPStartGame, w);
            perf_entry.StartEntry(slot);
            world_enter_pending = true;
            world_enter_logged  = false;
            world_enter_start_tp = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[perf-world-enter] phase=startgame_sent\n");
        },
        .OnCreate = [&](int slot,
                        const std::string& name,
                        uint16_t actor_def_id,
                        int gender) {
            rco::net::Writer w;
            w.WriteU8(static_cast<uint8_t>(slot));
            w.WriteString(name);
            w.WriteU16(actor_def_id);
            w.WriteU8(static_cast<uint8_t>(gender));
            conn.SendPacket(rco::net::kPCreateCharacter, w);
        },
        .OnDelete = [&](int slot) {
            rco::net::Writer w;
            w.WriteU8(static_cast<uint8_t>(slot));
            conn.SendPacket(rco::net::kPDeleteCharacter, w);
        },
        .OnLogout = [&]() {
            perf_entry.Finalize("logout");
            conn.Disconnect();
            state          = rco::GameState::Login;
            renderer_ready = false;
            characters.clear();
            world_actors.clear();
            world_corpses.clear();
            world_static_objects.clear();
            world_entry_loading = false;
            world_entry_world_objects_received = false;
            world_entry_core_indices.clear();
            world_entry_core_cursor = 0;
            static_model_prewarm_queue.clear();
            static_model_prewarm_cursor = 0;
            quest_log.Clear();
            quest_log.journal_visible = false;
            party_panel.Clear();
            rco::gameplay::MutablePlayerSkillState().Clear();
            rco::gameplay::MutableActiveKitPool().Clear();
            skill_loadout_screen.SetOpen(false);
            dodge_roll_active = false;
            dodge_roll_pending = false;
            special_parry_telegraphs.clear();
            parry_judgements.clear();
            login_error.clear();
        }
    });

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
    audio.Init();

    double last_time = glfwGetTime();

    // ---------------------------------------------------------------------------
    // Persistent mouse state — shared between the camera-orbit section and the
    // LMB-targeting section inside the game loop.
    // ---------------------------------------------------------------------------
    bool   ms_rmb_prev    = false;  // RMB state last frame
    bool   ms_lmb_prev    = false;  // LMB state last frame
    bool   ms_lmb_click   = false;  // true for exactly one frame: LMB press
    double ms_prev_x      = 0.0, ms_prev_y = 0.0;
    bool   ms_rmb_defense_candidate = false; // tap RMB => defense skill (T&L-like)
    double ms_rmb_down_time = 0.0;
    double ms_rmb_down_x = 0.0, ms_rmb_down_y = 0.0;
    int    cursor_mode_last = GLFW_CURSOR_NORMAL;
    bool   cursor_toggle_state = false;
    bool   cursor_key_was_pressed = false;
    uint64_t frame_index = 0;

    while (!window.ShouldClose()) {
        ++frame_index;

        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - last_time);
        last_time  = now;
        bool action_cursor_unlocked = false;

        // ---- Network poll ----
        {
            rco::net::InboundPacket pkt;
            while (conn.Poll(pkt)) handle_packet(pkt);
        }

        // ---- Begin frame (clears buffers, polls events) ----
        window.BeginFrame();

        // Cursor policy: action-only gameplay keeps mouse captured; menus/UI stay normal.
        {
            const bool force_cursor_unlock =
                (state == rco::GameState::InGame && skill_loadout_screen.IsOpen());
            if (state == rco::GameState::InGame) {
                const bool key_now_pressed =
                    glfwGetKey(window.Handle(), input_config.cursor_release_key_glfw) == GLFW_PRESS;

                if (input_config.cursor_release_mode == CursorReleaseMode::Hold) {
                    action_cursor_unlocked = key_now_pressed;
                } else {
                    if (key_now_pressed && !cursor_key_was_pressed) {
                        cursor_toggle_state = !cursor_toggle_state;
                    }
                    action_cursor_unlocked = cursor_toggle_state;
                }
                cursor_key_was_pressed = key_now_pressed;
            } else {
                cursor_toggle_state = false;
                cursor_key_was_pressed = false;
                action_cursor_unlocked = false;
            }
            const int desired_cursor_mode =
                (state == rco::GameState::InGame && !(action_cursor_unlocked || force_cursor_unlock))
                    ? GLFW_CURSOR_DISABLED
                    : GLFW_CURSOR_NORMAL;
            if (cursor_mode_last != desired_cursor_mode) {
                glfwSetInputMode(window.Handle(), GLFW_CURSOR, desired_cursor_mode);
                cursor_mode_last = desired_cursor_mode;
                if (desired_cursor_mode == GLFW_CURSOR_DISABLED) {
                    glfwGetCursorPos(window.Handle(), &ms_prev_x, &ms_prev_y);
                }
            }
        }

        // ---- ImGui frame start (must happen before any ImGui input checks/draw lists) ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ---- 3D world (rendered before ImGui so HUD draws on top) ----
        if (state == rco::GameState::InGame) {

            // Lazy-init renderer on first InGame frame
            if (!renderer_ready) {
                // --- New Engine/Pipeline (running in parallel to the old renderer during phase 4) ---
                rco::renderer::EngineConfig ecfg{};
                ecfg.width      = window.Width();
                ecfg.height     = window.Height();
                ecfg.shader_dir = "shaders/";
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    engine.Init(ecfg);
                    const auto dur_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count());
                    perf_entry.LogLazyStage("engine_init", dur_us, true);
                }
                {
                    const std::string path = "assets/ibl/default.hdr";
                    const auto t0 = std::chrono::steady_clock::now();
                    engine.LoadEnvironment(path);
                    const auto dur_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count());
                    perf_entry.LogLazyStage("load_environment_initial", dur_us, true);
                    std::string skipped_path;
                    if (engine.ConsumeLoadEnvironmentSkipped(&skipped_path)) {
                        perf_entry.LogLoadEnvironmentSkipped(
                            "init_default",
                            skipped_path.empty() ? path : skipped_path);
                    } else {
                        perf_entry.LogLoadEnvironment("init_default", path, dur_us);
                    }
                }
                pipeline = std::make_unique<rco::renderer::Pipeline>(engine);
                pipeline->SetCharacterReadability(active_character_readability_tuning);
                pipeline->SetSceneLook(active_scene_look_tuning);
                pipeline->SetColorGrading(
                    active_render_color_profile.contrast,
                    active_render_color_profile.saturation,
                    active_render_color_profile.vibrance,
                    active_render_color_profile.black_point,
                    active_render_color_profile.vignette_strength,
                    active_render_color_profile.vignette_softness);
                std::fprintf(stderr,
                    "[render-look] ibl=%.2f sky=%.2f shadow_lift=%.2f direct=%.2f ambient=%.2f flat=%.2f min_ndotl=%.2f albedo_min=%.2f albedo_lift=%.2f spec=%.2f exposure=%.2f sun=%.2f\n",
                    active_scene_look_tuning.iblIntensity,
                    active_scene_look_tuning.skyIntensity,
                    active_scene_look_tuning.worldShadowLift,
                    active_scene_look_tuning.directScale,
                    active_scene_look_tuning.ambientScale,
                    active_scene_look_tuning.flatAmbient,
                    active_scene_look_tuning.worldMinNdotL,
                    active_scene_look_tuning.albedoMinLuma,
                    active_scene_look_tuning.albedoLiftStrength,
                    active_scene_look_tuning.specularScale,
                    active_scene_look_tuning.exposureFactor,
                    active_scene_look_tuning.sunIntensity);
                std::fprintf(stderr,
                    "[render-color] contrast=%.2f saturation=%.2f vibrance=%.2f black_point=%.3f vignette=%.2f softness=%.2f\n",
                    active_render_color_profile.contrast,
                    active_render_color_profile.saturation,
                    active_render_color_profile.vibrance,
                    active_render_color_profile.black_point,
                    active_render_color_profile.vignette_strength,
                    active_render_color_profile.vignette_softness);
                std::fprintf(stderr, "[engine] init ok\n");

                bool terrain_ok = false;
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    terrain_ok = terrain.Init();
                    const auto dur_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count());
                    perf_entry.LogLazyStage("terrain_init", dur_us, terrain_ok);
                }
                if (terrain_ok) {
                    terrain.SetRenderTuning(active_terrain_render_tuning);
                    // Use the appearance from the actor def if already received,
                    // otherwise fall back to the default placeholder model.
                    const char* player_model = !player_meshes.empty()
                        ? player_meshes[0].model_path.c_str()
                        : "assets/models/player.glb";
                    const bool player_cache_hit = static_cast<bool>(
                        rco::renderer::ModelCachePeek(player_model));
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        ModelCacheContextScope mctx("lazy_player_init");
                        player_actor.Init("shaders", player_model, &engine.materials());
                        const auto dur_us = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count());
                        perf_entry.LogLazyStage("player_actor_init", dur_us, true);
                        perf_entry.LogActorInit(
                            "lazy_player_init", player_model, player_cache_hit,
                            false, dur_us, frame_index);
                    }
                    player_actor.SetReadabilityProfile(
                        rco::renderer::Actor::ReadabilityProfile::Character);
                    if (!player_meshes.empty()) {
                        const WorldMesh& body = player_meshes[0];
                        player_actor.scale      = body.scale > 0.f ? body.scale : 1.f;
                        player_actor.yaw_offset = player_yaw_offset;
                        player_actor.y_offset   = player_y_offset;
                        if (player_actor.IsLoaded() && !body.material_map.empty()) {
                            std::unordered_map<std::string,
                                rco::renderer::Model::MaterialPaths> by_name;
                            for (const auto& am : body.material_map) {
                                rco::renderer::Model::MaterialPaths mp;
                                mp.albedo = am.albedo;
                                mp.normal = am.normal;
                                mp.orm    = am.orm;
                                by_name[am.ai_name] = std::move(mp);
                            }
                            player_actor.ApplyMaterialsByName(engine.materials(), by_name);
                        }
                    }
                    for (auto& wa : player_anims) {
                        if (!wa.source_path.empty()) {
                            player_actor.LoadAnim(wa.source_path.c_str(),
                                                  wa.action.c_str());
                            player_anim_ctrl.SetClipDuration(
                                wa.action, player_actor.ClipDuration(wa.action));
                        }
                    }
                    for (auto& wa : player_anims) {
                        if (!wa.clip_override.empty())
                            player_actor.AliasClip(wa.clip_override, wa.action);
                    }
                    for (auto& wa : player_anims) {
                        player_anim_ctrl.SetClipDuration(
                            wa.action, player_actor.ClipDuration(wa.action));
                    }
                    engine.MarkMaterialsDirty();
                    initial_material_rebuild_pending_log = true;
                    camera.SetActorHeight(player_actor.ModelHeight());
                    particles.Init();
                    renderer_ready = true;
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        terrain.LoadFromEditor(player.areaName);
                        const auto dur_us = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count());
                        perf_entry.LogLazyStage("terrain_load_from_editor", dur_us, true);
                    }
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        col_data = rco::renderer::LoadColData(player.areaName);
                        const auto dur_us = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count());
                        perf_entry.LogLazyStage("coldata_load", dur_us, true);
                    }
                    player.y = terrain.SampleHeight(player.x, player.z);
                    player_ctrl.Reset();
                    camera.SnapTarget({player.x, player.y, player.z});
                    if (world_enter_pending && !world_enter_logged) {
                        auto now_tp = std::chrono::steady_clock::now();
                        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now_tp - world_enter_start_tp).count();
                        auto client_init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now_tp - world_enter_pstart_tp).count();
                        std::fprintf(stderr,
                            "[perf-world-enter] phase=renderer_ready total_ms=%lld client_init_ms=%lld\n",
                            static_cast<long long>(total_ms),
                            static_cast<long long>(client_init_ms));
                        rco::net::Writer pwe;
                        pwe.WriteU32(static_cast<uint32_t>(std::max<long long>(0, total_ms)));
                        pwe.WriteU32(static_cast<uint32_t>(std::max<long long>(0, client_init_ms)));
                        conn.SendPacket(rco::net::kPClientWorldReady, pwe);
                        perf_entry.LogClientWorldReady(
                            static_cast<uint32_t>(std::max<long long>(0, total_ms)),
                            static_cast<uint32_t>(std::max<long long>(0, client_init_ms)));
                        world_enter_logged = true;
                        world_enter_pending = false;
                    }
                } else {
                    std::fprintf(stderr, "[renderer] Failed to load shaders — check shaders/ directory\n");
                }
            }

            if (renderer_ready) {
                if (area_environment_pending_apply) {
                    auto cfg = pipeline->Features();
                    cfg.volumetrics = active_area_lighting.volumetrics_default;
                    pipeline->SetFeatures(cfg);
                    pipeline->SetCharacterReadability(active_character_readability_tuning);
                    terrain.SetRenderTuning(active_terrain_render_tuning);
                    pipeline->SetColorGrading(
                        active_render_color_profile.contrast,
                        active_render_color_profile.saturation,
                        active_render_color_profile.vibrance,
                        active_render_color_profile.black_point,
                        active_render_color_profile.vignette_strength,
                        active_render_color_profile.vignette_softness);
                    const std::string path = ResolveIblPathFromAreaConfig(pending_area_skybox_hdr);
                    const auto env_t0 = std::chrono::steady_clock::now();
                    engine.LoadEnvironment(path);
                    const auto env_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - env_t0).count());
                    std::string skipped_path;
                    if (engine.ConsumeLoadEnvironmentSkipped(&skipped_path)) {
                        perf_entry.LogLoadEnvironmentSkipped(
                            "area_apply_pending",
                            skipped_path.empty() ? path : skipped_path);
                    } else {
                        perf_entry.LogLoadEnvironment("area_apply_pending", path, env_us);
                    }
                    pending_area_skybox_hdr.clear();
                    if (SceneDebugLogsEnabled()) {
                        std::fprintf(stderr,
                            "[area-light] area='%s' preset='%s' sun_mul=%.2f sky_mul=%.2f fog_mul=%.2f volumetrics=%d exposure=%.2f contrast=%.2f terrain_tiling=%.2f terrain_macro=%.2f terrain_slop=%.2f\n",
                            player.areaName.c_str(),
                            active_area_lighting.preset_name.c_str(),
                            active_area_lighting.sun_intensity_mul,
                            active_area_lighting.sky_intensity_mul,
                            active_area_lighting.fog_density_mul,
                            active_area_lighting.volumetrics_default ? 1 : 0,
                            active_scene_look_tuning.exposureFactor,
                            active_render_color_profile.contrast,
                            active_terrain_render_tuning.tiling_mul,
                            active_terrain_render_tuning.macro_strength_mul,
                            active_terrain_render_tuning.height_blend_slop);
                    }
                    area_environment_pending_apply = false;
                }

                // Character-entry loading gate:
                // pre-load a core radius around player spawn before gameplay.
                if (world_entry_loading) {
                    const float kCorePreloadRadius = loading_preset.core_preload_radius;
                    const int kCorePreloadMaxObjects = loading_preset.core_preload_max_objects;
                    const double kCorePreloadTimeoutMs = loading_preset.core_preload_timeout_ms;
                    const int kCoreInitPerFrame = loading_preset.core_init_per_frame;
                    const double kCoreInitBudgetMs = loading_preset.core_init_budget_ms;
                    const int kCoreColdLoadsPerFrame = loading_preset.core_cold_loads_per_frame;
                    const int kLoadingExitPendingMax = loading_preset.loading_exit_pending_max;

                    if (world_entry_core_indices.empty()) {
                        const float r2 = kCorePreloadRadius * kCorePreloadRadius;
                        std::vector<std::pair<float, std::size_t>> core_sorted;
                        core_sorted.reserve(world_static_objects.size());
                        for (std::size_t i = 0; i < world_static_objects.size(); ++i) {
                            auto& obj = world_static_objects[i];
                            if (obj.actor || obj.model_path.empty()) continue;
                            float dx = obj.x - player.x;
                            float dz = obj.z - player.z;
                            float d2 = dx * dx + dz * dz;
                            if (d2 <= r2) core_sorted.emplace_back(d2, i);
                        }
                        std::sort(core_sorted.begin(), core_sorted.end(),
                            [](const auto& a, const auto& b) { return a.first < b.first; });
                        if (kCorePreloadMaxObjects > 0 &&
                            core_sorted.size() > static_cast<std::size_t>(kCorePreloadMaxObjects)) {
                            core_sorted.resize(static_cast<std::size_t>(kCorePreloadMaxObjects));
                        }
                        world_entry_core_indices.reserve(core_sorted.size());
                        for (const auto& it : core_sorted) world_entry_core_indices.push_back(it.second);
                        world_entry_core_cursor = 0;
                    }

                    int initialized = 0;
                    int cold_loads = 0;
                    const auto core_start_tp = std::chrono::steady_clock::now();
                    while (world_entry_core_cursor < world_entry_core_indices.size() &&
                           initialized < kCoreInitPerFrame) {
                        auto elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - core_start_tp).count() / 1000.0;
                        if (elapsed_ms >= kCoreInitBudgetMs) break;

                        auto& obj = world_static_objects[world_entry_core_indices[world_entry_core_cursor++]];
                        if (obj.actor || obj.model_path.empty()) continue;
                        const bool warm_cached =
                            static_cast<bool>(rco::renderer::ModelCachePeek(obj.model_path));
                        if (!warm_cached && cold_loads >= kCoreColdLoadsPerFrame) continue;

                        obj.actor = std::make_unique<rco::renderer::Actor>();
                        const auto init_t0 = std::chrono::steady_clock::now();
                        {
                            ModelCacheContextScope mctx("core_preload");
                            obj.actor->Init("shaders", obj.model_path.c_str(), &engine.materials());
                        }
                        const auto init_us = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - init_t0).count());
                        perf_entry.LogActorInit(
                            "core_preload", obj.model_path, warm_cached, false, init_us, frame_index);
                        obj.actor->position = {obj.x, obj.y, obj.z};
                        obj.actor->yaw      = obj.yaw;
                        obj.actor->scale    = obj.scale;
                        if (!warm_cached) ++cold_loads;
                        ++initialized;
                    }
                    if (initialized > 0) {
                        engine.MarkMaterialsDirty();
                    }

                    // After core is done, continue loading the most relevant global
                    // objects while still inside the loading screen so gameplay
                    // starts with a denser world.
                    if (world_entry_core_cursor >= world_entry_core_indices.size()) {
                        const int kGlobalInitPerFrame = loading_preset.global_init_per_frame_after_core;
                        const double kGlobalInitBudgetMs = loading_preset.global_init_budget_ms_after_core;
                        const auto global_start_tp = std::chrono::steady_clock::now();
                        int global_inits = 0;
                        int cold_loads = 0;

                        std::vector<std::pair<float, std::size_t>> candidates;
                        candidates.reserve(world_static_objects.size());
                        const float camYawRad = glm::radians(camera.GetYaw());
                        const glm::vec2 camForward(std::sin(camYawRad), std::cos(camYawRad));
                        for (std::size_t i = 0; i < world_static_objects.size(); ++i) {
                            auto& obj = world_static_objects[i];
                            if (obj.actor || obj.model_path.empty()) continue;
                            const bool warm_cached =
                                static_cast<bool>(rco::renderer::ModelCachePeek(obj.model_path));
                            const glm::vec2 toObj(obj.x - player.x, obj.z - player.z);
                            const float dist2 = glm::dot(toObj, toObj);
                            const float frontDot = glm::dot(glm::normalize(toObj + glm::vec2(1e-6f, 1e-6f)), camForward);
                            const float score = dist2
                                              + (frontDot < 0.f ? 1e8f : 0.f)
                                              + (warm_cached ? 0.f : 5e7f);
                            candidates.emplace_back(score, i);
                        }

                        if (!candidates.empty()) {
                            const std::size_t take = std::min<std::size_t>(
                                static_cast<std::size_t>(kGlobalInitPerFrame), candidates.size());
                            std::partial_sort(
                                candidates.begin(), candidates.begin() + take, candidates.end(),
                                [](const auto& a, const auto& b) { return a.first < b.first; });

                            for (std::size_t n = 0; n < take; ++n) {
                                auto elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - global_start_tp).count() / 1000.0;
                                if (elapsed_ms >= kGlobalInitBudgetMs) break;
                                auto& obj = world_static_objects[candidates[n].second];
                                if (obj.actor || obj.model_path.empty()) continue;
                                const bool warm_cached =
                                    static_cast<bool>(rco::renderer::ModelCachePeek(obj.model_path));
                                if (!warm_cached && cold_loads >= kCoreColdLoadsPerFrame) continue;
                                obj.actor = std::make_unique<rco::renderer::Actor>();
                                const auto init_t0 = std::chrono::steady_clock::now();
                                {
                                    ModelCacheContextScope mctx("loading_global");
                                    obj.actor->Init("shaders", obj.model_path.c_str(), &engine.materials());
                                }
                                const auto init_us = static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - init_t0).count());
                                perf_entry.LogActorInit(
                                    "loading_global", obj.model_path, warm_cached, false, init_us, frame_index);
                                obj.actor->position = {obj.x, obj.y, obj.z};
                                obj.actor->yaw      = obj.yaw;
                                obj.actor->scale    = obj.scale;
                                if (!warm_cached) ++cold_loads;
                                ++global_inits;
                            }
                        }

                        if (global_inits > 0) {
                            engine.MarkMaterialsDirty();
                        }
                    }

                    const double loading_elapsed_ms =
                        (glfwGetTime() - world_entry_loading_start) * 1000.0;
                    constexpr double kGateAbsoluteTimeoutMs = 5000.0;
                    const bool world_objects_ready = world_entry_world_objects_received;
                    const bool core_done =
                        world_objects_ready &&
                        world_entry_core_cursor >= world_entry_core_indices.size();
                    int pending_total = 0;
                    for (const auto& obj : world_static_objects) {
                        if (!obj.actor && !obj.model_path.empty()) ++pending_total;
                    }
                    if (core_done) {
                        perf_entry.LogCoreDone(
                            world_entry_core_indices.size(),
                            (std::min)(world_entry_core_cursor, world_entry_core_indices.size()),
                            pending_total);
                    }
                    const bool pending_gate_pass =
                        (kLoadingExitPendingMax < 0) ||
                        (pending_total <= kLoadingExitPendingMax);
                    const bool ready_enough =
                        world_objects_ready && core_done && pending_gate_pass;
                    const bool soft_timeout =
                        world_objects_ready && (loading_elapsed_ms >= kCorePreloadTimeoutMs);
                    const bool absolute_timeout = loading_elapsed_ms >= kGateAbsoluteTimeoutMs;
                    const bool timeout = soft_timeout || absolute_timeout;
                    if (ready_enough || timeout) {
                        const char* gate_reason = ready_enough ? "all_done"
                            : (!world_objects_ready ? "timeout_wait_worldobjects"
                                                    : (absolute_timeout ? "absolute_timeout" : "timeout"));
                        if (SceneDebugLogsEnabled()) {
                            std::fprintf(stderr,
                                "[perf-world-enter] loading-exit reason=%s core_total=%zu core_done=%zu pending=%d elapsed_ms=%.0f preset=%s world_objects_ready=%d\n",
                                gate_reason,
                                world_entry_core_indices.size(),
                                (std::min)(world_entry_core_cursor, world_entry_core_indices.size()),
                                pending_total,
                                loading_elapsed_ms,
                                loading_preset.name,
                                world_objects_ready ? 1 : 0);
                        }
                        perf_entry.LogGateRelease(
                            gate_reason,
                            world_entry_core_indices.size(),
                            (std::min)(world_entry_core_cursor, world_entry_core_indices.size()),
                            pending_total,
                            loading_elapsed_ms);
                        world_entry_loading = false;
                    }
                }

                // Incremental model prewarm:
                // Warm unique static-object model paths in tiny slices so the
                // later Actor::Init path hits ModelCache more often.
                {
                    constexpr int kPrewarmMaxPerFrame = 1;
                    constexpr double kPrewarmBudgetMs = 1.0;
                    const double frame_ms = dt * 1000.0;
                    if (frame_ms <= 22.0 &&
                        static_model_prewarm_cursor < static_model_prewarm_queue.size()) {
                        const auto prewarm_start_tp = std::chrono::steady_clock::now();
                        int warmed = 0;
                        while (warmed < kPrewarmMaxPerFrame &&
                               static_model_prewarm_cursor < static_model_prewarm_queue.size()) {
                            auto elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - prewarm_start_tp).count() / 1000.0;
                            if (elapsed_ms >= kPrewarmBudgetMs) break;

                            const std::string& path =
                                static_model_prewarm_queue[static_model_prewarm_cursor++];
                            if (!rco::renderer::ModelCachePeek(path)) {
                                ModelCacheContextScope mctx("static_prewarm");
                                (void)rco::renderer::ModelCacheGet(path, &engine.materials());
                            }
                            ++warmed;
                        }
                    }
                }

                // Progressive static world-object init: keep world-enter snappy.
                // We load only a small batch per frame to avoid a long blocking stall
                // when an area sends hundreds of objects.
                if (!world_entry_loading) {
                    const int kStaticInitPerFrame = loading_preset.static_init_per_frame;
                    const double kStaticInitBudgetMs = loading_preset.static_init_budget_ms;
                    constexpr double kFrameSoftLimitMs = 16.0; // ~60 FPS
                    constexpr double kFrameHardLimitMs = 22.0; // below ~45 FPS: pause streaming
                    int initialized = 0;
                    int pending = 0;
                    bool miss_used_this_frame = false;
                    const double frame_ms = dt * 1000.0;
                    int max_inits_this_frame = kStaticInitPerFrame;
                    double init_budget_ms = kStaticInitBudgetMs;
                    if (frame_ms > kFrameHardLimitMs) {
                        max_inits_this_frame = 0;
                        init_budget_ms = 0.0;
                    } else if (frame_ms > kFrameSoftLimitMs) {
                        max_inits_this_frame = 1;
                        init_budget_ms = 1.0;
                    }
                    // Calibrate throughput from real data: use rolling p99 of
                    // cache hits only, so rare cold-load misses do not throttle
                    // the steady-state streaming budget.
                    const double static_hit_init_p99_us = estimate_static_hit_init_p99_us();
                    if (frame_ms <= kFrameSoftLimitMs &&
                        max_inits_this_frame > 0 &&
                        init_budget_ms > 0.0 &&
                        static_hit_init_p99_us > 0.0) {
                        const int budget_cap = static_cast<int>(
                            std::floor((init_budget_ms * 1000.0) / static_hit_init_p99_us));
                        const int adaptive_cap = std::clamp(budget_cap, 4, 128);
                        max_inits_this_frame = (std::min)(kStaticInitPerFrame, adaptive_cap);
                    }
                    double budget_ms_remaining = init_budget_ms;

                    // Camera-aware streaming:
                    // 1) prefer objects in front of camera
                    // 2) within that, prefer nearest by distance
                    std::vector<std::pair<float, std::size_t>> candidates;
                    candidates.reserve(world_static_objects.size());
                    const float camYawRad = glm::radians(camera.GetYaw());
                    const glm::vec2 camForward(std::sin(camYawRad), std::cos(camYawRad));
                    for (std::size_t i = 0; i < world_static_objects.size(); ++i) {
                        auto& obj = world_static_objects[i];
                        if (obj.actor || obj.model_path.empty()) continue;
                        const bool warm_cached =
                            static_cast<bool>(rco::renderer::ModelCachePeek(obj.model_path));
                        const glm::vec2 toObj(obj.x - player.x, obj.z - player.z);
                        const float dist2 = glm::dot(toObj, toObj);
                        const float frontDot = glm::dot(glm::normalize(toObj + glm::vec2(1e-6f, 1e-6f)), camForward);
                        // Strong penalty for behind-camera objects so visible space
                        // fills first, while still eventually loading everything.
                        // Prefer warm-cached models to reduce stutter from first-time loads.
                        const float score = dist2
                                          + (frontDot < 0.f ? 1e8f : 0.f)
                                          + (warm_cached ? 0.f : 5e7f);
                        candidates.emplace_back(score, i);
                    }
                    pending = static_cast<int>(candidates.size());

                    if (max_inits_this_frame > 0 && !candidates.empty()) {
                        std::sort(candidates.begin(), candidates.end(),
                            [](const auto& a, const auto& b) { return a.first < b.first; });

                        for (std::size_t n = 0; n < candidates.size(); ++n) {
                            if (initialized >= max_inits_this_frame) break;
                            if (budget_ms_remaining <= 0.0) break;
                            auto& obj = world_static_objects[candidates[n].second];
                            const bool warm_cached =
                                static_cast<bool>(rco::renderer::ModelCachePeek(obj.model_path));
                            if (!warm_cached && miss_used_this_frame) break;
                            obj.actor = std::make_unique<rco::renderer::Actor>();
                            const auto init_t0 = std::chrono::steady_clock::now();
                            {
                                ModelCacheContextScope mctx("static_stream_post_loading");
                                obj.actor->Init("shaders", obj.model_path.c_str(), &engine.materials());
                            }
                            const auto init_us = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - init_t0).count());
                            perf_entry.LogActorInit(
                                "static_stream_post_loading",
                                obj.model_path,
                                warm_cached,
                                true,
                                init_us,
                                frame_index);
                            record_static_init_sample(init_us, warm_cached);
                            obj.actor->position = {obj.x, obj.y, obj.z};
                            obj.actor->yaw      = obj.yaw;
                            obj.actor->scale    = obj.scale;
                            if (!warm_cached) miss_used_this_frame = true;
                            budget_ms_remaining -= static_cast<double>(init_us) / 1000.0;
                            ++initialized;
                        }
                    }

                    if (initialized > 0) {
                        engine.MarkMaterialsDirty();
                        static double s_last_static_log = 0.0;
                        if (now - s_last_static_log > 1.0) {
                            std::fprintf(stderr,
                                "[perf-world-enter] static_stream init=%d pending=%d total=%zu\n",
                                initialized, pending, world_static_objects.size());
                            s_last_static_log = now;
                        }
                    }
                }

                GLFWwindow* w = window.Handle();
                const bool skill_loadout_open = skill_loadout_screen.IsOpen();

                // ---- Mouse orbit (Action mode only) ----
                {
                    bool cur_rmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                    bool cur_lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS;
                    const ImGuiIO& io_state = ImGui::GetIO();

                    // T&L-like defense skill flow:
                    // quick RMB tap -> defense skill.
                    // if moving while tapping, treat as dodge; otherwise guard window.
                    if (cur_rmb && !ms_rmb_prev) {
                        glfwGetCursorPos(w, &ms_rmb_down_x, &ms_rmb_down_y);
                        ms_rmb_down_time = now;
                        ms_rmb_defense_candidate = conn.IsConnected() && !player_dead &&
                                                   !skill_loadout_open &&
                                                   !io_state.WantCaptureMouse &&
                                                   !io_state.WantTextInput;
                    } else if (!cur_rmb && ms_rmb_prev) {
                        if (ms_rmb_defense_candidate && !skill_loadout_open && !io_state.WantCaptureMouse) {
                            double rx, ry;
                            glfwGetCursorPos(w, &rx, &ry);
                            const float moved = std::hypot(
                                static_cast<float>(rx - ms_rmb_down_x),
                                static_cast<float>(ry - ms_rmb_down_y));
                            const double held_s = now - ms_rmb_down_time;
                            if (held_s <= 0.22 && moved <= 6.f) {
                                auto resolve_dodge_dir = [&]() -> glm::vec2 {
                                    const float yr = glm::radians(player.yaw);
                                    const glm::vec2 fdir{-std::sin(yr), -std::cos(yr)};
                                    const glm::vec2 rdir{std::cos(yr), -std::sin(yr)};

                                    glm::vec2 dir(0.f);
                                    if (glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS) dir += fdir;
                                    if (glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS) dir -= fdir;
                                    if (glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_Q) == GLFW_PRESS) dir -= rdir;
                                    if (glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_E) == GLFW_PRESS) dir += rdir;

                                    if (glm::dot(dir, dir) > 0.0001f) {
                                        return glm::normalize(dir);
                                    }
                                    if (player_ctrl.HasMoveTarget()) {
                                        glm::vec3 mt = player_ctrl.MoveTarget();
                                        glm::vec2 to_target{mt.x - player.x, mt.z - player.z};
                                        if (glm::dot(to_target, to_target) > 0.0001f) {
                                            return glm::normalize(to_target);
                                        }
                                    }
                                    if (player_ctrl.IsAutoRunning()) {
                                        return glm::normalize(fdir);
                                    }
                                    return glm::normalize(fdir);
                                };

                                bool moving_input =
                                    glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS ||
                                    glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS ||
                                    glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS ||
                                    glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS ||
                                    glfwGetKey(w, GLFW_KEY_Q) == GLFW_PRESS ||
                                    glfwGetKey(w, GLFW_KEY_E) == GLFW_PRESS ||
                                    player_ctrl.IsAutoRunning() ||
                                    player_ctrl.HasMoveTarget();
                                if (moving_input) {
                                    dodge_roll_pending = true;
                                    dodge_roll_pending_dir = resolve_dodge_dir();
                                    dodge_roll_pending_until = now + 0.45;
                                    send_combat_action(rco::net::kCombatActionDodge, 0);
                                } else {
                                    send_combat_action(rco::net::kCombatActionGuardStart, 0);
                                }
                            }
                        }
                        ms_rmb_defense_candidate = false;
                    }

                    // Always rotate camera while cursor is captured; pause look when UI cursor is unlocked.
                    double cx, cy;
                    glfwGetCursorPos(w, &cx, &cy);
                    if (!action_cursor_unlocked && !io_state.WantCaptureMouse) {
                        camera.ApplyMouseDelta((float)(cx - ms_prev_x),
                                               (float)(cy - ms_prev_y));
                        player.yaw = camera.GetYaw();
                    }
                    ms_prev_x = cx;
                    ms_prev_y = cy;

                    // LMB down = instant click event.
                    if (cur_lmb && !ms_lmb_prev) ms_lmb_click = true;

                    ms_rmb_prev = cur_rmb;
                    ms_lmb_prev = cur_lmb;
                }

                // ---- Player movement (keyboard, click-to-move, gravity, slope, jump) ----
                if (!ImGui::GetIO().WantCaptureKeyboard && !skill_loadout_open) {
                    bool rmb_held = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                    bool lmb_held = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS;

                    player_ctrl.Update(w, dt, player_dead, player, terrain, rmb_held, lmb_held);
                }

                // Directional dodge roll impulse (authoritative start from PCombatEvent).
                if (!player_dead && dodge_roll_active) {
                    const double roll_total = dodge_roll_end - dodge_roll_start;
                    if (now >= dodge_roll_end || roll_total <= 0.0001 ||
                        glm::dot(dodge_roll_dir, dodge_roll_dir) < 0.0001f) {
                        dodge_roll_active = false;
                    } else {
                        const float t = std::clamp(
                            static_cast<float>((now - dodge_roll_start) / roll_total),
                            0.f, 1.f);
                        const float speed =
                            kDodgeRollSpeedStart + (kDodgeRollSpeedEnd - kDodgeRollSpeedStart) * t;
                        player.x += dodge_roll_dir.x * speed * dt;
                        player.z += dodge_roll_dir.y * speed * dt;
                    }
                }

                // Resolve player position against collision volumes (boxes + spheres).
                // Only re-snap y when grounded — controller owns y while airborne.
                if (col_data.loaded) {
                    col_data.Resolve(player.x, player.z, player.y, player.z);
                    if (player_ctrl.IsOnGround())
                        player.y = terrain.SampleHeight(player.x, player.z);
                }

                // Cancel movement on death.
                if (player_dead) {
                    player_ctrl.CancelMoveTarget();
                    pending_interact = 0;
                    dodge_roll_active = false;
                    dodge_roll_pending = false;
                }

                // Auto-interact: fire kPRightClick once close enough.
                if (pending_interact != 0 && !player_dead) {
                    auto pit = world_actors.find(pending_interact);
                    if (pit == world_actors.end()) {
                        pending_interact = 0; // NPC gone
                    } else {
                        float dnx = pit->second.x - player.x;
                        float dnz = pit->second.z - player.z;
                        if (dnx*dnx + dnz*dnz <= kInteractRange * kInteractRange) {
                            rco::net::Writer iw;
                            iw.WriteU32(pending_interact);
                            conn.SendPacket(rco::net::kPRightClick, iw);
                            pending_interact = 0;
                            player_ctrl.CancelMoveTarget();
                        }
                    }
                }

                // Send position to server at 10 Hz
                static double last_move_send = 0.0;
                if (!player_dead && now - last_move_send >= 0.1 && conn.IsConnected()) {
                    rco::net::Writer mw;
                    mw.WriteF32(player.x);
                    mw.WriteF32(player.y);
                    mw.WriteF32(player.z);
                    mw.WriteF32(player.yaw);
                    mw.WriteU8(0); // flags
                    conn.SendPacket(rco::net::kPStandardUpdate, mw);
                    last_move_send = now;
                }

                // Camera follows player — anchor at the model's visual feet, not
                // raw terrain y, so the pivot stays at shoulder regardless of y_offset.
                camera.action_mode = true;
                camera.SetTarget({player.x, player.y + player_actor.FeetOffset(), player.z});
                camera.Update(dt);

                // Terrain collision: march from pivot to ideal camera position and
                // find the furthest safe distance so the lens never clips into hills.
                {
                    float yr  = glm::radians(camera.GetYaw());
                    float pr  = glm::radians(camera.GetPitch());
                    glm::vec3 dir = { std::cos(pr) * std::sin(yr),
                                      std::sin(pr),
                                      std::cos(pr) * std::cos(yr) };
                    float lookat_y  = player.y + player_actor.FeetOffset()
                                      + player_actor.ModelHeight() * 0.85f;
                    glm::vec3 pivot = glm::vec3(player.x, lookat_y, player.z);
                    constexpr float kCollisionDist = 35.f;
                    constexpr int   kSteps         = 16;
                    constexpr float kMargin        = 0.4f;   // stay this far above terrain
                    float safe = kCollisionDist;
                    for (int i = 1; i <= kSteps; ++i) {
                        float t      = kCollisionDist * (float)i / (float)kSteps;
                        glm::vec3 p  = pivot + dir * t;
                        float     th = terrain.SampleHeight(p.x, p.z) + kMargin;
                        if (p.y < th) { safe = t - kCollisionDist / (float)kSteps; break; }
                    }
                    camera.SetCollisionDist((safe > 2.5f ? safe : 2.5f));
                }

                float aspect = window.Width() / static_cast<float>(window.Height());
                glm::mat4 view = camera.View();
                glm::mat4 proj = camera.Projection(aspect);
                view_mat = view;
                proj_mat = proj;
                const auto area_light = active_area_lighting;
                glm::vec3 sun  = area_light.sun_dir;



                // -- F8 cycles debug viz (0=full,1=albedo,2=normal,3=depth,4=AO,
                // 5=shadow,6=irradiance,7=NoL,8=albedo*NoL,9=envDiffuse,10=direct).
                {
                    static bool f8_prev = false;
                    bool f8_cur = glfwGetKey(w, GLFW_KEY_F8) == GLFW_PRESS;
                    if (f8_cur && !f8_prev) {
                        int m = (pipeline->DebugMode() + 1) % 17;
                        pipeline->SetDebugMode(m);
                        const char* names[] = {
                            "0 FULL", "1 albedo", "2 normal", "3 depth", "4 AO",
                            "5 shadow", "6 irradiance", "7 NoL", "8 albedo*NoL",
                            "9 envDiffuse", "10 direct (no shadow)", "11 shadowmap sample",
                            "12 ambient final", "13 direct shadowed", "14 lighting no sky",
                            "15 flat fill only", "16 lifted albedo"
                        };
                        std::fprintf(stderr, "[debug] gPhongGlobal mode = %s\n", names[m]);
                    }
                    f8_prev = f8_cur;
                }

                // Volumetric fog stays always enabled (design decision for atmosphere).

                // --- New pipeline: begin frame, submit all scene geometry, end writes to framebuffer 0 ---
                {
                    uint64_t rb_us = 0;
                    if (engine.FlushMaterialsBufferIfDirty(&rb_us)) {
                        const bool post_loading = !world_entry_loading;
                        perf_entry.LogRebuildMaterials(
                            post_loading ? "coalesced_frame_post_loading"
                                         : "coalesced_frame_loading",
                            post_loading, rb_us, frame_index);
                        if (initial_material_rebuild_pending_log) {
                            perf_entry.LogLazyStage("rebuild_materials_initial", rb_us, true);
                            perf_entry.LogRebuildMaterials(
                                "rebuild_materials_initial", false, rb_us, frame_index);
                            initial_material_rebuild_pending_log = false;
                        }
                    }
                }
                pipeline->Begin(view, proj, camera.Position(), static_cast<float>(dt));
                auto area_scene_look = active_scene_look_tuning;
                area_scene_look.sunIntensity =
                    glm::clamp(active_scene_look_tuning.sunIntensity * area_light.sun_intensity_mul, 0.0f, 2.0f);
                area_scene_look.skyIntensity =
                    glm::clamp(active_scene_look_tuning.skyIntensity * area_light.sky_intensity_mul, 0.0f, 2.0f);
                pipeline->SetSceneLook(area_scene_look);
                pipeline->SetAtmosphereFog(area_light.fog_color, area_light.fog_density_mul);
                pipeline->SetSun(-sun, area_light.sun_color * area_scene_look.sunIntensity);
                terrain.Submit(*pipeline, camera.Position());

                // Render local player
                {
                    bool moving = glm::length(glm::vec2(player.x - last_player_pos.x,
                                                        player.z - last_player_pos.y)) > 0.02f;
                    float player_speed = glm::length(glm::vec2(player.x - last_player_pos.x,
                                                               player.z - last_player_pos.y)) / dt;

                    // One-shot diagnostic: log anim state on the first rendered frame
                    {
                        static bool s_anim_logged = false;
                        if (!s_anim_logged) {
                            s_anim_logged = true;
                            if (SceneDebugLogsEnabled()) {
                                std::fprintf(stderr,
                                    "[anim-debug] IsReady=%d action='%s' time=%.3f "
                                    "speed=%.2f clips=%d\n",
                                    player_anim_ctrl.IsReady() ? 1 : 0,
                                    player_anim_ctrl.IsReady()
                                        ? player_anim_ctrl.CurrentAction().c_str() : "(none)",
                                    player_anim_ctrl.IsReady()
                                        ? player_anim_ctrl.CurrentTime() : 0.f,
                                    player_speed,
                                    player_actor.model().ClipCount());
                            }
                        }
                    }

                    if (player_anim_ctrl.IsReady()) {
                        // AnimController handles Idle/Walk/Run transitions via Update()
                        if (player_dead) {
                            player_anim_ctrl.RequestStateByName("Death");
                        }
                        player_anim_ctrl.Update(dt, player_dead ? 0.f : player_speed);
                    } else {
                        // Legacy fallback: drive player_actor directly and advance time
                        const std::string& cur = player_actor.CurrentAnim();
                        if (player_dead) {
                            if (cur != "Death") player_actor.PlayAnim("Death", false);
                        } else if (moving) {
                            if (cur != "Walk")  player_actor.PlayAnim("Walk",  true);
                        } else {
                            if (cur != "Idle")  player_actor.PlayAnim("Idle",  true);
                        }
                        player_actor.Update(dt);
                    }
                    player_actor.position = {player.x, player.y, player.z};
                    player_actor.yaw      = player.yaw;
                    player_anim_ctrl.Submit(player_actor, *pipeline);
                }

                // ── Frustum culling setup (Gribb-Hartmann) ──────────────────────────
                // Extract 6 planes from proj*view. GLM is col-major: row r =
                // (m[0][r], m[1][r], m[2][r], m[3][r]).
                // Each plane stored as (normal n, distance d): dot(n,P)+d >= 0 → inside.
                struct FrustumPlane { glm::vec3 n; float d; };
                FrustumPlane fc_planes[6];
                {
                    const glm::mat4 vp = proj * view;
                    auto row = [&](int r) {
                        return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]);
                    };
                    auto make_plane = [](glm::vec4 p) -> FrustumPlane {
                        float len = glm::length(glm::vec3(p));
                        if (len > 1e-5f) p /= len;
                        return { glm::vec3(p), p.w };
                    };
                    glm::vec4 r0=row(0), r1=row(1), r2=row(2), r3=row(3);
                    fc_planes[0] = make_plane(r3 + r0); // left
                    fc_planes[1] = make_plane(r3 - r0); // right
                    fc_planes[2] = make_plane(r3 + r1); // bottom
                    fc_planes[3] = make_plane(r3 - r1); // top
                    fc_planes[4] = make_plane(r3 + r2); // near
                    fc_planes[5] = make_plane(r3 - r2); // far
                }
                int fc_vis = 0, fc_culled = 0;
                std::unordered_map<std::string, int> fc_model_counts;

                // Render all other actors (NPCs + other players)
                for (auto& [rid, e] : world_actors) {
                    // Compute velocity for auto-locomotion from position delta
                    // (we only have position updates from PStandardUpdate, so
                    // approximate as a very small value when standing still)
                    float dx = e.x - e.prev_x;
                    float dz = e.z - e.prev_z;
                    // NPC positions jump at ~10Hz; use binary moved/stopped so
                    // AnimController auto-locomotion stays stable between updates.
                    float actor_vel = (dx*dx + dz*dz > 0.0001f) ? 5.0f : 0.f;
                    e.prev_x = e.x;
                    e.prev_z = e.z;
                    // Update AnimController and sync anim_name
                    if (e.anim_ctrl.IsReady()) {
                        e.anim_ctrl.Update(dt, actor_vel);
                        e.anim_name = e.anim_ctrl.CurrentAction();
                        e.anim_t    = e.anim_ctrl.CurrentTime();
                    } else {
                        e.anim_t += dt;
                    }

                    // Lazy init: build the per-NPC Actor now that renderer_ready
                    // is true and we have appearance data from PNewActor.
                    if (!e.actor && !e.meshes.empty()) {
                        const WorldMesh* body = nullptr;
                        for (auto& m : e.meshes) {
                            if (m.slot == 0) { body = &m; break; }
                        }
                        if (!body) body = &e.meshes.front();

                        const bool warm_cached = static_cast<bool>(
                            rco::renderer::ModelCachePeek(body->model_path));
                        auto a = std::make_unique<rco::renderer::Actor>();
                        const auto init_t0 = std::chrono::steady_clock::now();
                        {
                            ModelCacheContextScope mctx("npc_lazy_init");
                            a->Init("shaders", body->model_path.c_str(),
                                    &engine.materials());
                        }
                        const auto init_us = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - init_t0).count());
                        perf_entry.LogActorInit(
                            "npc_lazy_init",
                            body->model_path,
                            warm_cached,
                            !world_entry_loading,
                            init_us,
                            frame_index);
                        a->SetReadabilityProfile(
                            rco::renderer::Actor::ReadabilityProfile::Character);
                        a->scale      = body->scale > 0.f ? body->scale : 1.f;
                        a->yaw_offset = e.yaw_offset;
                        a->y_offset   = e.y_offset;

                        // Per-aiMaterial mapping FIRST — paints multi-material
                        // meshes (Substance imports etc.) where each submesh
                        // names a different material.
                        if (a->IsLoaded() && !body->material_map.empty()) {
                            std::unordered_map<std::string,
                                rco::renderer::Model::MaterialPaths> by_name;
                            for (const auto& am : body->material_map) {
                                rco::renderer::Model::MaterialPaths mp;
                                mp.albedo = am.albedo;
                                mp.normal = am.normal;
                                mp.orm    = am.orm;
                                by_name[am.ai_name] = std::move(mp);
                            }
                            a->ApplyMaterialsByName(engine.materials(), by_name);
                        }

                        // Per-slot global override (from Body.material_id) goes
                        // ON TOP of the per-aiMaterial map — same precedence as
                        // the GUE Media preview / Zone editor.
                        const bool has_material =
                            !body->albedo.empty() ||
                            !body->normal.empty() ||
                            !body->orm.empty();
                        if (has_material ||
                            body->roughness > 0.f || body->metallic > 0.f ||
                            body->ar > 0.f || body->ag > 0.f || body->ab > 0.f) {
                            a->OverrideMaterial(body->albedo, body->normal, body->orm,
                                                body->ar, body->ag, body->ab,
                                                body->roughness, body->metallic);
                        }
                        engine.MarkMaterialsDirty();

                        for (auto& an : e.anims) {
                            if (!an.source_path.empty())
                                a->LoadAnim(an.source_path.c_str(), an.action.c_str());
                        }
                        for (auto& an : e.anims) {
                            if (!an.clip_override.empty())
                                a->AliasClip(an.clip_override, an.action);
                        }
                        a->PlayAnim("Idle", true);
                        if (SceneDebugLogsEnabled()) {
                            std::fprintf(stderr,
                                "[Actor init] rid=%u model=%s scale=%.2f "
                                "mat=albedo:'%s' normal:'%s' orm:'%s' "
                                "anim_bindings=%zu clips_loaded=%d\n",
                                rid, body->model_path.c_str(), a->scale,
                                body->albedo.c_str(), body->normal.c_str(), body->orm.c_str(),
                                e.anims.size(), a->model().ClipCount());
                        }
                        e.actor = std::move(a);
                    }

                    // Y is server-authoritative — the GUE places NPCs on
                    // terrain and writes the hit-point Y to npc_spawns.y,
                    // which reaches us via PNewActor. Resampling here would
                    // hide that and leave NPCs floating when terrain has
                    // been edited without re-broadcasting spawns.
                    glm::vec3 pos = {e.x, e.y, e.z};

                    // ── Distance cull — skip actors beyond max render distance ────────
                    static constexpr float kActorDrawDist = 150.f;
                    {
                        float dx = e.x - player.x;
                        float dz = e.z - player.z;
                        if (dx*dx + dz*dz > kActorDrawDist * kActorDrawDist) {
                            ++fc_culled;
                            continue;
                        }
                    }

                    // ── Frustum cull — AABB 2×2×2 centered at (x, y+1, z) ───────────
                    // p-vertex test: r = |n.x|*hx + |n.y|*hy + |n.z|*hz (hx=hy=hz=1)
                    {
                        const glm::vec3 center = pos + glm::vec3(0.f, 1.f, 0.f);
                        bool inside = true;
                        for (const auto& p : fc_planes) {
                            float dist = glm::dot(p.n, center) + p.d
                                       + std::abs(p.n.x) + std::abs(p.n.y) + std::abs(p.n.z);
                            if (dist < 0.f) { inside = false; break; }
                        }
                        if (!inside) { ++fc_culled; continue; }
                    }
                    ++fc_vis;
                    // Track per-model visible counts for perf diagnostics
                    if (e.actor && !e.meshes.empty())
                        ++fc_model_counts[e.meshes.front().model_path];

                    // Use AnimController state if available, otherwise fall back
                    // to the legacy anim_name/anim_t fields
                    std::string submit_anim = e.anim_name;
                    float       submit_time = e.anim_t;
                    bool loop_flag = true;
                    if (e.anim_ctrl.IsReady()) {
                        submit_anim = e.anim_ctrl.CurrentAction();
                        submit_time = e.anim_ctrl.CurrentTime();
                        // One-shot actions should not loop
                        loop_flag = (submit_anim != "Attack" && submit_anim != "Death"
                                     && submit_anim != "Jump" && submit_anim != "Cast");
                    } else {
                        loop_flag = (e.anim_name != "Attack" && e.anim_name != "Death");
                    }

                    if (e.actor) {
                        e.actor->position = pos;
                        e.actor->yaw      = e.yaw;
                        e.anim_ctrl.Submit(*e.actor, *pipeline);
                    } else {
                        player_actor.position = pos;
                        player_actor.yaw      = e.yaw;
                        player_anim_ctrl.Submit(player_actor, *pipeline);
                    }
                }

                // Render corpses: keep death pose briefly, then sink into ground.
                if (!world_corpses.empty()) {
                    for (auto it = world_corpses.begin(); it != world_corpses.end();) {
                        if (now >= it->remove_time) {
                            it = world_corpses.erase(it);
                            continue;
                        }
                        auto& e = it->actor;
                        if (!e.actor) {
                            ++it;
                            continue;
                        }

                        float sink_t = 0.f;
                        if (now > it->sink_start_time && it->remove_time > it->sink_start_time) {
                            sink_t = static_cast<float>((now - it->sink_start_time) /
                                (it->remove_time - it->sink_start_time));
                            sink_t = std::clamp(sink_t, 0.f, 1.f);
                        }
                        const float sink_eased = sink_t * sink_t * (3.f - 2.f * sink_t); // smoothstep
                        e.y = it->base_y - (it->sink_depth * sink_eased);

                        if (e.anim_ctrl.IsReady()) {
                            e.anim_ctrl.Update(dt, 0.f);
                            e.anim_name = e.anim_ctrl.CurrentAction();
                            e.anim_t    = e.anim_ctrl.CurrentTime();
                        } else {
                            e.anim_t += dt;
                            e.actor->Update(dt);
                        }

                        glm::vec3 pos = {e.x, e.y, e.z};
                        static constexpr float kActorDrawDist = 150.f;
                        {
                            float ddx = e.x - player.x;
                            float ddz = e.z - player.z;
                            if (ddx * ddx + ddz * ddz > kActorDrawDist * kActorDrawDist) {
                                ++it;
                                continue;
                            }
                        }
                        {
                            const glm::vec3 center = pos + glm::vec3(0.f, 0.9f, 0.f);
                            bool inside = true;
                            for (const auto& p : fc_planes) {
                                float dist = glm::dot(p.n, center) + p.d
                                           + std::abs(p.n.x) + std::abs(p.n.y) + std::abs(p.n.z);
                                if (dist < 0.f) { inside = false; break; }
                            }
                            if (!inside) {
                                ++it;
                                continue;
                            }
                        }

                        e.actor->position = pos;
                        e.actor->yaw      = e.yaw;
                        if (e.anim_ctrl.IsReady()) {
                            e.anim_ctrl.Submit(*e.actor, *pipeline);
                        } else {
                            e.actor->Submit(*pipeline);
                        }
                        ++it;
                    }
                }

                // Static world objects
                for (auto& obj : world_static_objects) {
                    if (obj.actor && obj.actor->IsLoaded()) {
                        obj.actor->Submit(*pipeline);
                    }
                }

                // Frustum culling stats — throttled to ~1 Hz
                {
                    static float s_perf_acc = 0.f;
                    s_perf_acc += static_cast<float>(dt);
                    if (s_perf_acc >= 1.f) {
                        s_perf_acc = 0.f;
                        if (SceneDebugLogsEnabled()) {
                            std::fprintf(stderr,
                                "[perf] actors_vis=%d actors_culled=%d actors_tot=%d\n",
                                fc_vis, fc_culled, fc_vis + fc_culled);
                            for (const auto& [path, count] : fc_model_counts) {
                                // Extract just the filename for brevity
                                auto slash = path.find_last_of("/\\");
                                std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
                                std::fprintf(stderr, "[perf]   model='%s' instances=%d\n",
                                             fname.c_str(), count);
                            }
                        }
                    }
                }

                // All scene submissions done. Step particles forward (sim) then run the
                // deferred pipeline, letting particles render inside its forward pass
                // so they benefit from depth coherence + tonemap + FXAA.
                particles.Update(static_cast<float>(now), dt);
                pipeline->End([&]() {
                    particles.Render(view, proj);
                });

                last_player_pos = {player.x, player.z};

                // ---- Target indicator: ring on ground under combat_target ----
                if (combat_target != 0) {
                    auto tit = world_actors.find(combat_target);
                    if (tit != world_actors.end()) {
                        float tx = tit->second.x, tz = tit->second.z;
                        float ty = terrain.SampleHeight(tx, tz) + 0.05f;
                        float sw = (float)window.Width(), sh = (float)window.Height();
                        auto* ol = ImGui::GetForegroundDrawList();
                        constexpr int   kSegs = 32;
                        constexpr float kRad  = 1.1f;
                        ImVec2 pts[kSegs]; bool all_ok = true;
                        for (int i = 0; i < kSegs; ++i) {
                            float a  = (float)i / kSegs * 6.2831853f;
                            glm::vec4 c = proj * view *
                                glm::vec4(tx + std::cos(a)*kRad, ty, tz + std::sin(a)*kRad, 1.f);
                            if (c.w > 0.f)
                                pts[i] = { (c.x/c.w*0.5f+0.5f)*sw, (1.f-c.y/c.w*0.5f-0.5f)*sh };
                            else { all_ok = false; pts[i] = {-9999.f,-9999.f}; }
                        }
                        if (all_ok)
                            ol->AddPolyline(pts, kSegs, IM_COL32(255,60,60,220),
                                            ImDrawFlags_Closed, 2.5f);
                    }
                }

                // NPC special attack telegraph and parry judgement feedback.
                // The circle is billboarded (screen-facing) and anchored in air.
                if (!special_parry_telegraphs.empty() || !parry_judgements.empty()) {
                    auto* ol = ImGui::GetForegroundDrawList();
                    float sw = static_cast<float>(window.Width());
                    float sh = static_cast<float>(window.Height());
                    std::vector<uint32_t> expired;
                    auto project_to_screen = [&](float wx, float wy, float wz, ImVec2& out) -> bool {
                        glm::vec4 c = proj * view * glm::vec4(wx, wy, wz, 1.f);
                        if (c.w <= 0.f) return false;
                        out = {(c.x / c.w * 0.5f + 0.5f) * sw,
                               (1.f - c.y / c.w * 0.5f - 0.5f) * sh};
                        return true;
                    };
                    auto MulAlpha = [](ImU32 col, float mul) -> ImU32 {
                        const int r = static_cast<int>((col >> IM_COL32_R_SHIFT) & 0xFF);
                        const int g = static_cast<int>((col >> IM_COL32_G_SHIFT) & 0xFF);
                        const int b = static_cast<int>((col >> IM_COL32_B_SHIFT) & 0xFF);
                        int a = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFF);
                        a = std::clamp(static_cast<int>(static_cast<float>(a) * mul), 0, 255);
                        return IM_COL32(r, g, b, a);
                    };
                    auto LerpColor = [](ImU32 a, ImU32 b, float t) -> ImU32 {
                        t = std::clamp(t, 0.f, 1.f);
                        auto lerp_ch = [t](int x, int y) -> int {
                            return static_cast<int>(static_cast<float>(x) +
                                                    (static_cast<float>(y - x) * t) + 0.5f);
                        };
                        const int ar = static_cast<int>((a >> IM_COL32_R_SHIFT) & 0xFF);
                        const int ag = static_cast<int>((a >> IM_COL32_G_SHIFT) & 0xFF);
                        const int ab = static_cast<int>((a >> IM_COL32_B_SHIFT) & 0xFF);
                        const int aa = static_cast<int>((a >> IM_COL32_A_SHIFT) & 0xFF);
                        const int br = static_cast<int>((b >> IM_COL32_R_SHIFT) & 0xFF);
                        const int bg = static_cast<int>((b >> IM_COL32_G_SHIFT) & 0xFF);
                        const int bb = static_cast<int>((b >> IM_COL32_B_SHIFT) & 0xFF);
                        const int ba = static_cast<int>((b >> IM_COL32_A_SHIFT) & 0xFF);
                        return IM_COL32(
                            std::clamp(lerp_ch(ar, br), 0, 255),
                            std::clamp(lerp_ch(ag, bg), 0, 255),
                            std::clamp(lerp_ch(ab, bb), 0, 255),
                            std::clamp(lerp_ch(aa, ba), 0, 255));
                    };

                    for (const auto& [source_rid, tg] : special_parry_telegraphs) {
                        auto sit = world_actors.find(source_rid);
                        if (sit == world_actors.end()) {
                            expired.push_back(source_rid);
                            continue;
                        }
                        if (now > tg.end_time + 0.35) {
                            expired.push_back(source_rid);
                            continue;
                        }

                        const float tx = sit->second.x;
                        const float tz = sit->second.z;
                        const float model_h = (sit->second.actor && sit->second.actor->IsLoaded())
                            ? sit->second.actor->ModelHeight()
                            : 1.8f;
                        const float ty = sit->second.y + (std::max)(1.6f, model_h * 1.05f);
                        ImVec2 center{};
                        if (!project_to_screen(tx, ty, tz, center)) {
                            continue;
                        }

                        double duration = tg.end_time - tg.start_time;
                        if (duration < 0.0001) duration = 0.0001;
                        const float duration_s = static_cast<float>(duration);
                        const float remaining_s = (std::max)(0.f, static_cast<float>(tg.end_time - now));
                        const float t = std::clamp(remaining_s / duration_s, 0.f, 1.f);
                        const float urgency = 1.f - t;
                        const float outer_world = (tg.radius > 0.2f) ? tg.radius : 1.45f;
                        const float dx = tx - player.x;
                        const float dy = ty - player.y;
                        const float dz = tz - player.z;
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const float outer_px = std::clamp(
                            (outer_world * 160.f) / (distance > 0.8f ? distance : 0.8f),
                            24.f, 120.f);

                        float parry_window_s =
                            (tg.parry_window_ms > 0) ? (static_cast<float>(tg.parry_window_ms) / 1000.f) : 0.22f;
                        parry_window_s = std::clamp(parry_window_s, 0.08f, duration_s * 0.80f);
                        const float parry_window_ratio =
                            std::clamp(parry_window_s / duration_s, 0.08f, 0.75f);
                        const bool parry_now = (remaining_s <= parry_window_s);

                        const float pulse = 0.85f + 0.15f *
                            std::sin(static_cast<float>((now - tg.start_time) * 14.0));
                        const ImU32 outer_col = MulAlpha(
                            tg.outer_color,
                            std::clamp((0.55f + urgency * 0.65f) * pulse, 0.30f, 1.45f));
                        const ImU32 guide_col = parry_now
                            ? IM_COL32(95, 255, 165, 245)
                            : IM_COL32(255, 225, 110, 190);
                        const ImU32 inner_hot = parry_now ? IM_COL32(120, 255, 170, 245)
                                                          : IM_COL32(255, 255, 255, 245);
                        const ImU32 inner_mix = LerpColor(tg.inner_color, inner_hot, urgency * 0.80f);
                        const ImU32 inner_col = MulAlpha(
                            inner_mix, std::clamp(0.75f + urgency * 0.45f, 0.45f, 1.35f));

                        const float guide_px = outer_px * parry_window_ratio;
                        const float inner_px = (std::max)(outer_px * 0.10f, outer_px * t);
                        ol->AddCircle(center, outer_px, outer_col, 64, 2.4f);
                        ol->AddCircle(center, guide_px, MulAlpha(guide_col, 0.90f), 64, 2.2f);
                        ol->AddCircle(center, inner_px, inner_col, 64, 3.0f);

                        if (tg.target_rid == player.runtimeId) {
                            const char* lbl = parry_now ? "PARRY AGORA!" : "PARRY";
                            ImVec2 ts = ImGui::CalcTextSize(lbl);
                            ImU32 lbl_col = parry_now ? IM_COL32(120, 255, 175, 252)
                                                      : IM_COL32(255, 240, 100, 245);
                            ol->AddText({center.x - ts.x * 0.5f, center.y - outer_px - ts.y - 8.f},
                                        lbl_col, lbl);

                            char timer_text[24];
                            std::snprintf(timer_text, sizeof(timer_text), "%.2fs", remaining_s);
                            ImVec2 tts = ImGui::CalcTextSize(timer_text);
                            ol->AddText({center.x - tts.x * 0.5f, center.y + outer_px + 4.f},
                                        IM_COL32(255, 255, 255, 220), timer_text);
                        }
                    }
                    for (uint32_t rid : expired) {
                        special_parry_telegraphs.erase(rid);
                    }

                    for (auto it = parry_judgements.begin(); it != parry_judgements.end();) {
                        if (now > it->end_time) {
                            it = parry_judgements.erase(it);
                            continue;
                        }
                        const float life = static_cast<float>(it->end_time - it->start_time);
                        const float elapsed = static_cast<float>(now - it->start_time);
                        const float p = (life > 0.0001f) ? std::clamp(elapsed / life, 0.f, 1.f) : 1.f;
                        const float alpha_mul = 1.f - p;

                        ImVec2 center{};
                        bool has_center = false;
                        auto src = world_actors.find(it->source_rid);
                        if (src != world_actors.end()) {
                            const float src_h = (src->second.actor && src->second.actor->IsLoaded())
                                ? src->second.actor->ModelHeight()
                                : 1.8f;
                            ImVec2 projected{};
                            if (project_to_screen(
                                    src->second.x,
                                    src->second.y + (std::max)(1.6f, src_h * 1.10f),
                                    src->second.z, projected)) {
                                center = projected;
                                has_center = true;
                            }
                        }
                        if (!has_center) {
                            ++it;
                            continue;
                        }

                        const ImU32 burst_col = it->success
                            ? MulAlpha(IM_COL32(90, 255, 165, 245), alpha_mul)
                            : MulAlpha(IM_COL32(255, 110, 110, 245), alpha_mul);
                        const float burst_r = 22.f + p * 80.f;
                        ol->AddCircle(center, burst_r, burst_col, 56, 3.0f);

                        const char* lbl = it->success ? "PARRY PERFEITO!" : "PARRY FALHOU!";
                        ImVec2 ts = ImGui::CalcTextSize(lbl);
                        ImU32 lbl_col = it->success
                            ? MulAlpha(IM_COL32(120, 255, 180, 255), alpha_mul)
                            : MulAlpha(IM_COL32(255, 135, 135, 255), alpha_mul);
                        ol->AddText({center.x - ts.x * 0.5f, center.y - burst_r - ts.y - 8.f},
                                    lbl_col, lbl);

                        char sub[48];
                        if (it->success) {
                            std::snprintf(sub, sizeof(sub), "timing: %dms", static_cast<int>(it->metric));
                        } else {
                            std::snprintf(sub, sizeof(sub), "special hit: %d", static_cast<int>(it->metric));
                        }
                        ImVec2 ss = ImGui::CalcTextSize(sub);
                        ol->AddText({center.x - ss.x * 0.5f, center.y + burst_r + 4.f},
                                    MulAlpha(IM_COL32(255, 255, 255, 220), alpha_mul), sub);
                        ++it;
                    }
                }

                // Particles render inside pipeline->End() forward-pass callback above.
                // HDRI skybox is now drawn by pipeline->skyboxPass_().

                // Left-click: select closest actor within 55 px of cursor.
                // ms_lmb_click is set by the orbit section above: true for one
                // frame when LMB is pressed.
                {
                    // Ground-AoE targeting: resolve mouse ray to world XZ plane.
                    float ground_cursor_x = 0.f, ground_cursor_z = 0.f;
                    if (spellbar.pending_ground_spell != 0) {
                        double mx, my;
                        glfwGetCursorPos(w, &mx, &my);
                        float sw = static_cast<float>(window.Width());
                        float sh = static_cast<float>(window.Height());
                        float ndcX =  (static_cast<float>(mx) / sw) * 2.f - 1.f;
                        float ndcY = -(static_cast<float>(my) / sh) * 2.f + 1.f;
                        glm::mat4 invVP = glm::inverse(proj * view);
                        glm::vec4 near4 = invVP * glm::vec4(ndcX, ndcY, -1.f, 1.f);
                        glm::vec4 far4  = invVP * glm::vec4(ndcX, ndcY,  1.f, 1.f);
                        glm::vec3 rNear = glm::vec3(near4) / near4.w;
                        glm::vec3 rFar  = glm::vec3(far4)  / far4.w;
                        float t = (rFar.y != rNear.y)
                                  ? -rNear.y / (rFar.y - rNear.y)
                                  : 0.f;
                        ground_cursor_x = rNear.x + t * (rFar.x - rNear.x);
                        ground_cursor_z = rNear.z + t * (rFar.z - rNear.z);

                        // Draw targeting reticle via ImGui overlay.
                        auto* ol = ImGui::GetForegroundDrawList();
                        // Project circle points back to screen.
                        constexpr int kSegs = 32;
                        float gcy = renderer_ready
                            ? terrain.SampleHeight(ground_cursor_x, ground_cursor_z) + 0.05f
                            : 0.f;
                        // Find AoE radius for the pending spell.
                        float rad = 5.f;
                        for (auto& sl : spellbar.slots)
                            if (sl.id == spellbar.pending_ground_spell) { rad = sl.aoe_radius; break; }
                        ImVec2 pts[kSegs];
                        for (int i = 0; i < kSegs; ++i) {
                            float a  = (float)i / kSegs * 6.2831853f;
                            float wx = ground_cursor_x + std::cos(a) * rad;
                            float wz = ground_cursor_z + std::sin(a) * rad;
                            glm::vec4 c = proj * view * glm::vec4(wx, gcy, wz, 1.f);
                            if (c.w > 0.f) {
                                pts[i] = { (c.x/c.w*0.5f+0.5f)*sw,
                                           (1.f-c.y/c.w*0.5f-0.5f)*sh };
                            } else {
                                pts[i] = {-9999.f, -9999.f};
                            }
                        }
                        ol->AddPolyline(pts, kSegs, IM_COL32(255, 220, 40, 200), ImDrawFlags_Closed, 2.f);
                        ol->AddCircleFilled({(ndcX*0.5f+0.5f)*sw, (0.5f-ndcY*0.5f)*sh},
                                            5.f, IM_COL32(255, 220, 40, 200));
                    }

                    // Ground-AoE click: consume LMB click and fire the spell.
                    if (ms_lmb_click && spellbar.pending_ground_spell != 0
                        && !skill_loadout_open
                        && !ImGui::GetIO().WantCaptureMouse) {
                        if (spellbar.on_cast_ground)
                            spellbar.on_cast_ground(spellbar.pending_ground_spell,
                                                    ground_cursor_x, ground_cursor_z);
                        spellbar.pending_ground_spell = 0;
                        ms_lmb_click = false;
                    } else
                    if (ms_lmb_click && !player_dead && !skill_loadout_open && !ImGui::GetIO().WantCaptureMouse) {
                        double mx, my;
                        glfwGetCursorPos(w, &mx, &my);
                        float best_dist2 = 55.f * 55.f;
                        uint32_t best_id = 0;
                        float sw = static_cast<float>(window.Width());
                        float sh = static_cast<float>(window.Height());
                        for (auto& [rid, e] : world_actors) {
                            // Hit-test at the actor's actual Y + 1 (chest height).
                            // Using terrain sample would miss NPCs placed off-ground.
                            float ey = e.y + 1.f;
                            glm::vec4 clip = proj * view * glm::vec4(e.x, ey, e.z, 1.f);
                            if (clip.w <= 0.f) continue;
                            float sx2 = (clip.x / clip.w + 1.f) * 0.5f * sw;
                            float sy2 = (1.f - clip.y / clip.w) * 0.5f * sh;
                            float dx = sx2 - static_cast<float>(mx);
                            float dy = sy2 - static_cast<float>(my);
                            float d2 = dx * dx + dy * dy;
                            if (d2 < best_dist2) { best_dist2 = d2; best_id = rid; }
                        }
                        if (best_id != 0) {
                            auto& clicked = world_actors[best_id];
                            if (clicked.actor_type == 2 && conn.IsConnected()) {
                                float dnx = clicked.x - player.x;
                                float dnz = clicked.z - player.z;
                                if (dnx*dnx + dnz*dnz <= kInteractRange * kInteractRange) {
                                    // Close enough — interact now.
                                    rco::net::Writer iw;
                                    iw.WriteU32(best_id);
                                    conn.SendPacket(rco::net::kPRightClick, iw);
                                } else {
                                    // Too far — walk toward NPC then interact.
                                    float ny = renderer_ready
                                        ? terrain.SampleHeight(clicked.x, clicked.z)
                                        : clicked.y;
                                    player_ctrl.SetMoveTarget({clicked.x, ny, clicked.z});
                                    pending_interact  = best_id;
                                }
                            } else {
                                combat_target    = best_id;
                                pending_interact = 0;
                            }
                        } else {
                            combat_target    = 0;
                            pending_interact = 0;
                        }
                    }
                    ms_lmb_click = false;  // consumed; reset for next frame
                }

                // ---- Tab: cycle to nearest hostile actor ----
                {
                    static bool tab_prev = false;
                    bool tab_cur = glfwGetKey(w, GLFW_KEY_TAB) == GLFW_PRESS;
                    if (tab_cur && !tab_prev && !player_dead && !skill_loadout_open) {
                        // Build list sorted by screen-space distance to centre.
                        float sw = (float)window.Width(), sh = (float)window.Height();
                        float best = 1e9f; uint32_t best_id = 0;
                        bool  found_after_current = false;
                        // Two-pass: prefer actor after current target in list order,
                        // fall back to globally closest.
                        bool  past_current = (combat_target == 0);
                        for (auto& [rid, e] : world_actors) {
                            if (e.actor_type == 2) continue; // skip dialog-only NPCs
                            glm::vec4 clip = proj * view * glm::vec4(e.x, e.y+1.f, e.z, 1.f);
                            if (clip.w <= 0.f) continue;
                            float sx = (clip.x/clip.w*0.5f+0.5f)*sw - sw*0.5f;
                            float sy = (1.f-clip.y/clip.w*0.5f-0.5f)*sh - sh*0.5f;
                            float d  = sx*sx + sy*sy;
                            if (rid == combat_target) { past_current = true; continue; }
                            if (past_current && d < best) {
                                best = d; best_id = rid; found_after_current = true;
                            }
                        }
                        if (!found_after_current) { // wrap-around
                            for (auto& [rid, e] : world_actors) {
                                if (e.actor_type == 2) continue;
                                glm::vec4 clip = proj * view * glm::vec4(e.x, e.y+1.f, e.z, 1.f);
                                if (clip.w <= 0.f || rid == combat_target) continue;
                                float sx = (clip.x/clip.w*0.5f+0.5f)*sw - sw*0.5f;
                                float sy = (1.f-clip.y/clip.w*0.5f-0.5f)*sh - sh*0.5f;
                                float d  = sx*sx + sy*sy;
                                if (d < best) { best = d; best_id = rid; }
                            }
                        }
                        if (best_id) combat_target = best_id;
                    }
                    tab_prev = tab_cur;
                }

                // Auto-attack: send PAttackActor every ~0.85 s while target is selected.
                static constexpr double kAutoAttackInterval = 0.85;
                if (combat_target && !player_dead && conn.IsConnected()
                    && !skill_loadout_open
                    && now - last_attack_sent >= kAutoAttackInterval) {
                    rco::net::Writer aw;
                    aw.WriteU32(combat_target);
                    conn.SendPacket(rco::net::kPAttackActor, aw);
                    last_attack_sent = now;
                }
            }
        }

        if (perf_entry.EntryActive() &&
            state == rco::GameState::InGame &&
            renderer_ready) {
            int pending_static = 0;
            for (const auto& obj : world_static_objects) {
                if (!obj.actor && !obj.model_path.empty()) ++pending_static;
            }
            int pending_npc = 0;
            for (const auto& [rid, e] : world_actors) {
                (void)rid;
                if (!e.actor && !e.meshes.empty()) ++pending_npc;
            }
            perf_entry.MaybeAutoFinalize(world_entry_loading, pending_static, pending_npc);
        }

        switch (state) {

            case rco::GameState::Login:
                login_screen.SetError(login_error);
                login_screen.Render();
                break;

            case rco::GameState::CharacterSelect:
                char_select.SetCharacters(characters);
                char_select.SetPlayableDefs(playable_defs);
                char_select.SetError(login_error);
                char_select.Render(window.Width(), window.Height());
                break;

            case rco::GameState::InGame: {
                if (world_entry_loading) {
                    const bool world_objects_ready = world_entry_world_objects_received;
                    const int core_total = static_cast<int>(world_entry_core_indices.size());
                    const int core_done  = static_cast<int>(
                        (std::min)(world_entry_core_cursor, world_entry_core_indices.size()));
                    const float p = core_total > 0
                        ? static_cast<float>(core_done) / static_cast<float>(core_total)
                        : 0.f;
                    const double elapsed_ms = (glfwGetTime() - world_entry_loading_start) * 1000.0;

                    ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Always);
                    ImGui::SetNextWindowSize(
                        {static_cast<float>(window.Width()), static_cast<float>(window.Height())},
                        ImGuiCond_Always);
                    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.05f, 0.07f, 0.98f));
                    ImGui::Begin("##world_loading", nullptr,
                        ImGuiWindowFlags_NoDecoration |
                        ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings);
                    ImGui::Dummy({0.f, window.Height() * 0.35f});
                    ImGui::SetCursorPosX(window.Width() * 0.5f - 180.f);
                    ImGui::Text("Entrando em %s...", player.areaName.c_str());
                    ImGui::SetCursorPosX(window.Width() * 0.5f - 180.f);
                    ImGui::ProgressBar(p, {360.f, 18.f});
                    ImGui::SetCursorPosX(window.Width() * 0.5f - 180.f);
                    ImGui::Text("Preset: %s", loading_preset.name);
                    ImGui::SetCursorPosX(window.Width() * 0.5f - 180.f);
                    if (!world_objects_ready) {
                        ImGui::Text("Aguardando dados de objetos do mundo (PWorldObjects)...");
                    } else {
                        ImGui::Text("Objetos do mundo recebidos.");
                    }
                    ImGui::SetCursorPosX(window.Width() * 0.5f - 180.f);
                    ImGui::Text("Preload core (raio spawn): %d / %d  (%.1fs)", core_done, core_total,
                                static_cast<float>(elapsed_ms / 1000.0));
                    ImGui::End();
                    ImGui::PopStyleColor();
                    break;
                }

                // HUD
                ImGui::SetNextWindowPos({10.f, 10.f}, ImGuiCond_Always);
                ImGui::SetNextWindowSize({300.f, 142.f}, ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.55f);
                ImGui::Begin("##hud", nullptr,
                    ImGuiWindowFlags_NoDecoration  |
                    ImGuiWindowFlags_NoInputs       |
                    ImGuiWindowFlags_NoNav          |
                    ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("Area: %s  |  Lv %d", player.areaName.c_str(), (int)player.level);
                ImGui::Text("HP: %d / %d    MP: %d / %d    SP: %d / %d",
                    player.health, player.healthMax,
                    player.mana, player.manaMax,
                    player.stamina, player.staminaMax);
                // XP bar
                {
                    float xp_ratio = player.xp_next > 0
                        ? static_cast<float>(player.xp) / player.xp_next
                        : 1.f;
                    char xp_lbl[32];
                    snprintf(xp_lbl, sizeof(xp_lbl), "XP %u / %u", player.xp, player.xp_next);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 1.f, 1.f));
                    ImGui::ProgressBar(xp_ratio, {-1.f, 10.f}, xp_lbl);
                    ImGui::PopStyleColor();
                }
                ImGui::Text("Gold: %u    Pos: %.1f, %.1f, %.1f    [ACTION]",
                    player_gold, player.x, player.y, player.z);
                ImGui::TextDisabled("[Action] Mouse=cam  WASD=move  AD=strafe  Shift=sprint  NumLk=autorun");
                {
                    const char* key_label = CursorReleaseKeyLabel(input_config.cursor_release_key_glfw);
                    if (input_config.cursor_release_mode == CursorReleaseMode::Hold) {
                        ImGui::TextDisabled("Hold %s to unlock cursor for UI.", key_label);
                    } else {
                        ImGui::TextDisabled("Press %s to toggle cursor lock.", key_label);
                    }
                }
                ImGui::TextDisabled("Combat: Tap RMB moving = Dodge Roll (W/A/S/D direction), still = Guard");
                ImGui::End();

                // Action-mode crosshair.
                {
                    auto* dl = ImGui::GetForegroundDrawList();
                    float cx = window.Width()  * 0.5f;
                    float cy = window.Height() * 0.5f;
                    constexpr float kArm   = 10.f;
                    constexpr float kGap   = 3.f;
                    constexpr float kThick = 1.5f;
                    ImU32 col = IM_COL32(255, 255, 255, 210);
                    dl->AddLine({cx - kArm, cy}, {cx - kGap, cy}, col, kThick);
                    dl->AddLine({cx + kGap, cy}, {cx + kArm, cy}, col, kThick);
                    dl->AddLine({cx, cy - kArm}, {cx, cy - kGap}, col, kThick);
                    dl->AddLine({cx, cy + kGap}, {cx, cy + kArm}, col, kThick);
                }

                // Skill hotbar cast (1-9): client sends slot index, server resolves ability
                // authoritatively from active loadout.
                const bool skill_loadout_open_hotbar = skill_loadout_screen.IsOpen();
                if (!player_dead && !skill_loadout_open_hotbar && !ImGui::GetIO().WantTextInput) {
                    const auto& skill_state = rco::gameplay::PlayerSkillState();
                    int hotbar_key_slots = 4;
                    for (const auto& ab : skill_state.abilities()) {
                        hotbar_key_slots = (std::max)(hotbar_key_slots, static_cast<int>(ab.slot_index) + 1);
                    }
                    hotbar_key_slots = std::clamp(hotbar_key_slots, 1, 9);

                    for (int i = 0; i < hotbar_key_slots; ++i) {
                        const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_1 + i);
                        if (!ImGui::IsKeyPressed(key, false)) continue;

                        bool has_ability = false;
                        for (const auto& ab : skill_state.abilities()) {
                            if (static_cast<int>(ab.slot_index) == i) {
                                has_ability = true;
                                break;
                            }
                        }
                        if (!has_ability) continue;

                        if (combat_target == 0) {
                            static double s_last_no_target_log_at = -9999.0;
                            if (now - s_last_no_target_log_at >= 0.6) {
                                std::fprintf(stderr, "[cast] no target selected\n");
                                std::fflush(stderr);
                                s_last_no_target_log_at = now;
                            }
                            continue;
                        }

                        send_cast_skill_slot(static_cast<uint8_t>(i), combat_target);
                    }
                }

                // Toggle bag with I, character sheet with C, skill loadout with K,
                // controls/help with F1, quest journal with J, party panel with P.
                if (ImGui::IsKeyPressed(ImGuiKey_I) && !player_dead && !ImGui::GetIO().WantTextInput)
                    inventory.bag_visible = !inventory.bag_visible;
                if (ImGui::IsKeyPressed(ImGuiKey_C) && !player_dead && !ImGui::GetIO().WantTextInput)
                    inventory.char_visible = !inventory.char_visible;
                if (ImGui::IsKeyPressed(ImGuiKey_K) && !player_dead && !ImGui::GetIO().WantTextInput)
                    skill_loadout_screen.Toggle();
                if (ImGui::IsKeyPressed(ImGuiKey_F1) && !ImGui::GetIO().WantTextInput)
                    controls_ui.Toggle();
                if (ImGui::IsKeyPressed(ImGuiKey_J) && !ImGui::GetIO().WantTextInput)
                    quest_log.journal_visible = !quest_log.journal_visible;
                if (ImGui::IsKeyPressed(ImGuiKey_P) && !ImGui::GetIO().WantTextInput)
                    party_panel.visible = !party_panel.visible;

                // Close shop and helper windows with Escape
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    shop.open = false;
                    controls_ui.SetVisible(false);
                    skill_loadout_screen.SetOpen(false);
                    quest_log.journal_visible = false;
                }

                // F key — pick up nearby dropped item
                if (ImGui::IsKeyPressed(ImGuiKey_F) && !player_dead && !ImGui::GetIO().WantTextInput
                    && conn.IsConnected()) {
                    for (auto& wi : world_items) {
                        float dx = player.x - wi.x, dz = player.z - wi.z;
                        if (dx*dx + dz*dz <= 25.f) { // 5 unit radius
                            rco::net::Writer w;
                            w.WriteU32(wi.rid);
                            conn.SendPacket(rco::net::kPPickupItem, w);
                            audio.PlaySfx(rco::audio::SfxId::PickupItem);
                            break;
                        }
                    }
                }

                // Chat
                chat.Render(window.Width(), window.Height(),
                            static_cast<float>(now));
                {
                    std::string msg;
                    if (chat.PollSend(msg) && conn.IsConnected()) {
                        bool handled_ingame_cmd = false;
                        if (!msg.empty() && msg[0] == '/') {
                            std::string lower = msg;
                            std::transform(lower.begin(), lower.end(), lower.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            auto trim_copy = [](std::string value) {
                                const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
                                while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
                                    value.erase(value.begin());
                                while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
                                    value.pop_back();
                                return value;
                            };

                            auto send_party_action = [&](uint8_t action, const std::string& target_name) {
                                rco::net::Writer w;
                                w.WriteU8(action);
                                w.WriteString(target_name);
                                conn.SendPacket(rco::net::kPPartyAction, w);
                            };

                            if (lower == "/party accept") {
                                send_party_action(rco::net::kPartyActionAccept, "");
                                handled_ingame_cmd = true;
                            } else if (lower == "/party decline") {
                                send_party_action(rco::net::kPartyActionDecline, "");
                                handled_ingame_cmd = true;
                            } else if (lower == "/party leave") {
                                send_party_action(rco::net::kPartyActionLeave, "");
                                handled_ingame_cmd = true;
                            } else if (lower.rfind("/party invite ", 0) == 0 && msg.size() > 14) {
                                std::string target = trim_copy(msg.substr(14));
                                if (!target.empty()) {
                                    send_party_action(rco::net::kPartyActionInvite, target);
                                    handled_ingame_cmd = true;
                                }
                            } else if (lower.rfind("/party kick ", 0) == 0 && msg.size() > 12) {
                                std::string target = trim_copy(msg.substr(12));
                                if (!target.empty()) {
                                    send_party_action(rco::net::kPartyActionKick, target);
                                    handled_ingame_cmd = true;
                                }
                            } else if (lower.rfind("/party lead ", 0) == 0 && msg.size() > 12) {
                                std::string target = trim_copy(msg.substr(12));
                                if (!target.empty()) {
                                    send_party_action(rco::net::kPartyActionTransferLead, target);
                                    handled_ingame_cmd = true;
                                }
                            } else if (lower == "/party help") {
                                chat.AddMessage("",
                                    "Party commands: /party invite <name>, /party accept, /party decline, /party leave, /party kick <name>, /party lead <name>.");
                                handled_ingame_cmd = true;
                            } else if (lower == "/combat dodge") {
                                {
                                    const float yr = glm::radians(player.yaw);
                                    dodge_roll_pending = true;
                                    dodge_roll_pending_dir = {-std::sin(yr), -std::cos(yr)};
                                    dodge_roll_pending_until = glfwGetTime() + 0.45;
                                }
                                send_combat_action(rco::net::kCombatActionDodge, 0);
                                handled_ingame_cmd = true;
                            } else if (lower == "/combat guard on") {
                                send_combat_action(rco::net::kCombatActionGuardStart, 0);
                                handled_ingame_cmd = true;
                            } else if (lower == "/combat parry") {
                                send_combat_action(rco::net::kCombatActionParryStart, 0);
                                handled_ingame_cmd = true;
                            } else if (lower == "/combat help") {
                                chat.AddMessage("",
                                    "Combat commands: /combat dodge, /combat guard on, /combat parry. Dodge roll follows W/A/S/D direction.");
                                handled_ingame_cmd = true;
                            }
                        }

                        if (!handled_ingame_cmd) {
                            rco::net::Writer w;
                            w.WriteU8(0); // channel: say
                            w.WriteString(msg);
                            conn.SendPacket(rco::net::kPChatMessage, w);
                        }
                    }
                }

                inventory.Render(window.Width(), window.Height());
                controls_ui.Draw(player.name);
                quest_log.Render(window.Width(), window.Height());
                party_panel.Render(window.Width(), window.Height());
                skill_loadout_screen.Render(window.Width(), window.Height());

                // Death overlay.
                if (player_dead) {
                    auto* dl = ImGui::GetForegroundDrawList();
                    dl->AddRectFilled({0.f, 0.f},
                        {static_cast<float>(window.Width()), static_cast<float>(window.Height())},
                        IM_COL32(0, 0, 0, 160));

                    constexpr float kBtnW = 200.f, kBtnH = 40.f;
                    float cx = window.Width()  * 0.5f;
                    float cy = window.Height() * 0.5f;

                    ImGui::SetNextWindowPos({cx - kBtnW * 0.5f - 10.f, cy - 60.f},
                                           ImGuiCond_Always);
                    ImGui::SetNextWindowSize({kBtnW + 20.f, 120.f}, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.f);
                    ImGui::Begin("##death", nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoSavedSettings);

                    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("You have died.").x) * 0.5f);
                    ImGui::TextColored({1.f, 0.2f, 0.2f, 1.f}, "You have died.");
                    ImGui::Spacing();
                    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - kBtnW) * 0.5f);
                    if (ImGui::Button("Respawn", {kBtnW, kBtnH}) && conn.IsConnected()) {
                        rco::net::Writer w;
                        conn.SendPacket(rco::net::kPRespawnPlayer, w);
                    }
                    ImGui::End();
                }

                // Target indicator + HP bar over selected actor.
                if (combat_target) {
                    auto it = world_actors.find(combat_target);
                    if (it != world_actors.end()) {
                        auto& e = it->second;
                        float ey = e.y + 1.9f;
                        glm::vec4 clip = proj_mat * view_mat * glm::vec4(e.x, ey, e.z, 1.f);
                        if (clip.w > 0.f) {
                            float sw2 = static_cast<float>(window.Width());
                            float sh2 = static_cast<float>(window.Height());
                            float sx = (clip.x / clip.w + 1.f) * 0.5f * sw2;
                            float sy = (1.f - clip.y / clip.w) * 0.5f * sh2;
                            auto* dl = ImGui::GetForegroundDrawList();
                            // Name
                            ImVec2 ts = ImGui::CalcTextSize(e.name.c_str());
                            dl->AddText({sx - ts.x * 0.5f, sy - ts.y - 12.f},
                                        IM_COL32(255, 255, 0, 220), e.name.c_str());
                            // HP bar
                            constexpr float kBarW = 80.f, kBarH = 6.f;
                            float ratio = e.health_max > 0
                                ? static_cast<float>(e.health) / e.health_max
                                : 0.f;
                            if (ratio < 0.f) ratio = 0.f;
                            dl->AddRectFilled({sx - kBarW * 0.5f, sy - kBarH},
                                              {sx + kBarW * 0.5f, sy},
                                              IM_COL32(60, 0, 0, 200));
                            dl->AddRectFilled({sx - kBarW * 0.5f, sy - kBarH},
                                              {sx - kBarW * 0.5f + kBarW * ratio, sy},
                                              IM_COL32(0, 200, 0, 200));
                        }
                    } else {
                        combat_target = 0; // actor gone
                    }
                }

                // Floating damage/heal numbers.
                float_nums.Render(window.Width(), window.Height(),
                                  view_mat, proj_mat,
                                  static_cast<float>(ImGui::GetTime()));

                // Spell bar — compute distance to target for range checks.
                float target_dist_for_spells = 0.f;
                if (combat_target != 0) {
                    auto tit2 = world_actors.find(combat_target);
                    if (tit2 != world_actors.end()) {
                        float ddx = tit2->second.x - player.x;
                        float ddz = tit2->second.z - player.z;
                        target_dist_for_spells = std::sqrt(ddx*ddx + ddz*ddz);
                    }
                }
                spellbar.Render(window.Width(), window.Height(),
                                combat_target, static_cast<float>(now),
                                player_dead || skill_loadout_screen.IsOpen(),
                                player.mana, target_dist_for_spells);
                skill_hotbar.Render(window.Width(), window.Height());

                // Range / AoE preview circles (drawn when hovering a spell slot).
                if (renderer_ready && spellbar.hovered_range > 0.f) {
                    float sw = (float)window.Width(), sh = (float)window.Height();
                    auto* ol = ImGui::GetForegroundDrawList();
                    auto DrawWorldCircle = [&](float cx, float cz, float cy, float rad, ImU32 col, float thick) {
                        constexpr int kSeg = 48;
                        ImVec2 pts[kSeg]; bool ok = true;
                        for (int ii = 0; ii < kSeg; ++ii) {
                            float a = (float)ii / kSeg * 6.2831853f;
                            glm::vec4 c = proj_mat * view_mat *
                                glm::vec4(cx + std::cos(a)*rad, cy + 0.05f, cz + std::sin(a)*rad, 1.f);
                            if (c.w > 0.f)
                                pts[ii] = { (c.x/c.w*0.5f+0.5f)*sw, (1.f-c.y/c.w*0.5f-0.5f)*sh };
                            else { ok = false; pts[ii] = {-9999.f,-9999.f}; }
                        }
                        if (ok) ol->AddPolyline(pts, kSeg, col, ImDrawFlags_Closed, thick);
                    };
                    // Range circle around player (white, faint).
                    float py = terrain.SampleHeight(player.x, player.z);
                    DrawWorldCircle(player.x, player.z, py,
                                    spellbar.hovered_range,
                                    IM_COL32(220, 220, 255, 120), 1.5f);
                    // AoE preview around target (yellow).
                    if (spellbar.hovered_aoe_radius > 0.f &&
                        spellbar.hovered_aoe_type == 1 && combat_target != 0) {
                        auto tit3 = world_actors.find(combat_target);
                        if (tit3 != world_actors.end()) {
                            float ty = terrain.SampleHeight(tit3->second.x, tit3->second.z);
                            DrawWorldCircle(tit3->second.x, tit3->second.z, ty,
                                            spellbar.hovered_aoe_radius,
                                            IM_COL32(255, 220, 40, 180), 2.f);
                        }
                    }
                }

                // Spell visual effects.
                spell_fx.Render(window.Width(), window.Height(),
                                view_mat, proj_mat, static_cast<float>(now));

                // Chat bubbles.
                chat_bubbles.Render(window.Width(), window.Height(),
                                    view_mat, proj_mat, static_cast<float>(now));

                // Dialog window.
                if (dialog.open && !player_dead) {
                    constexpr float kDW = 440.f;
                    float dh = 130.f + static_cast<float>(dialog.options.size()) * 36.f;
                    float cx = static_cast<float>(window.Width())  * 0.5f;
                    float cy = static_cast<float>(window.Height()) * 0.5f;
                    ImGui::SetNextWindowPos({cx - kDW * 0.5f, cy - dh * 0.5f}, ImGuiCond_Always);
                    ImGui::SetNextWindowSize({kDW, dh}, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.88f);
                    ImGui::Begin("##dialog", nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings);

                    ImGui::TextColored({1.f, 0.85f, 0.3f, 1.f}, "%s", dialog.npc_name.c_str());
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", dialog.text.c_str());
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    float btnW = kDW - ImGui::GetStyle().WindowPadding.x * 2.f;
                    for (int i = 0; i < static_cast<int>(dialog.options.size()); ++i) {
                        std::string lbl = std::to_string(i + 1) + ". " + dialog.options[i];
                        if (ImGui::Button(lbl.c_str(), {btnW, 28.f}) && conn.IsConnected()) {
                            rco::net::Writer w;
                            w.WriteU8(static_cast<uint8_t>(i + 1));
                            conn.SendPacket(rco::net::kPDialogChoice, w);
                            dialog.open = false;
                        }
                    }
                    if (ImGui::Button("[Close]", {btnW, 28.f})) {
                        rco::net::Writer w;
                        w.WriteU8(0);
                        conn.SendPacket(rco::net::kPDialogChoice, w);
                        dialog.open = false;
                    }
                    ImGui::End();
                }

                // World item labels (dropped loot)
                if (!world_items.empty()) {
                    auto* dl = ImGui::GetForegroundDrawList();
                    float sw2 = static_cast<float>(window.Width());
                    float sh2 = static_cast<float>(window.Height());
                    float t   = static_cast<float>(ImGui::GetTime());

                    // Find nearest item for pickup prompt
                    const WorldItemEntry* nearest = nullptr;
                    float nearestDist = 25.f;
                    for (auto& wi : world_items) {
                        float dx = player.x - wi.x, dz = player.z - wi.z;
                        float d = dx*dx + dz*dz;
                        if (d <= nearestDist) { nearestDist = d; nearest = &wi; }
                    }

                    for (const auto& wi : world_items) {
                        glm::vec4 c = proj_mat * view_mat * glm::vec4(wi.x, wi.y + 0.6f, wi.z, 1.f);
                        if (c.w <= 0.f) continue;
                        float sx = (c.x/c.w + 1.f) * 0.5f * sw2;
                        float sy = (1.f - c.y/c.w) * 0.5f * sh2;

                        // Pulsing gold dot
                        float pulse = 0.7f + 0.3f * sinf(t * 3.f);
                        uint8_t a = static_cast<uint8_t>(200 * pulse);
                        dl->AddCircleFilled({sx, sy}, 5.f, IM_COL32(255, 200, 50, a));

                        // Item name
                        char lbl[64];
                        if (wi.quantity > 1)
                            snprintf(lbl, sizeof(lbl), "%s x%d", wi.name.c_str(), (int)wi.quantity);
                        else
                            snprintf(lbl, sizeof(lbl), "%s", wi.name.c_str());
                        ImVec2 ts = ImGui::CalcTextSize(lbl);
                        dl->AddText({sx - ts.x*0.5f, sy - ts.y - 6.f},
                                    IM_COL32(255, 220, 80, 220), lbl);

                        // Pickup hint for nearest
                        if (nearest == &wi) {
                            const char* hint = "[F] Pegar";
                            ImVec2 hs = ImGui::CalcTextSize(hint);
                            dl->AddText({sx - hs.x*0.5f, sy + 4.f},
                                        IM_COL32(200, 255, 180, 200), hint);
                        }
                    }
                }

                // Shop window
                if (shop.open && !player_dead) {
                    constexpr float kSW = 480.f;
                    float sh_win = 420.f;
                    float cx = static_cast<float>(window.Width())  * 0.5f;
                    float cy = static_cast<float>(window.Height()) * 0.5f;
                    ImGui::SetNextWindowPos({cx - kSW*0.5f, cy - sh_win*0.5f}, ImGuiCond_Always);
                    ImGui::SetNextWindowSize({kSW, sh_win}, ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.92f);
                    ImGui::Begin("Loja", &shop.open,
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoSavedSettings);

                    ImGui::Text("Ouro: %u", player_gold);
                    ImGui::Separator();

                    if (ImGui::BeginTabBar("##shop_tabs")) {
                        if (ImGui::BeginTabItem("Comprar")) {
                            ImGui::BeginChild("##buy_list", {0, sh_win - 100.f}, false);
                            for (const auto& it : shop.items) {
                                char row[128];
                                if (it.item_type == 0)
                                    snprintf(row, sizeof(row), "%s  [Dano %d]", it.name.c_str(), (int)it.weapon_damage);
                                else if (it.item_type == 1)
                                    snprintf(row, sizeof(row), "%s  [Arm %d]", it.name.c_str(), (int)it.armor_level);
                                else
                                    snprintf(row, sizeof(row), "%s", it.name.c_str());

                                ImGui::Text("%s", row);
                                ImGui::SameLine(kSW - 150.f);
                                ImGui::Text("%u g", it.buy_price);
                                ImGui::SameLine();
                                char btn[32];
                                snprintf(btn, sizeof(btn), "Comprar##b%d", (int)it.item_id);
                                if (ImGui::SmallButton(btn) && conn.IsConnected()) {
                                    rco::net::Writer w;
                                    w.WriteU8(0); // buy
                                    w.WriteU16(it.item_id);
                                    w.WriteU8(1);
                                    conn.SendPacket(rco::net::kPShopAction, w);
                                    audio.PlaySfx(rco::audio::SfxId::BuyItem);
                                }
                                ImGui::Separator();
                            }
                            ImGui::EndChild();
                            ImGui::EndTabItem();
                        }
                        if (ImGui::BeginTabItem("Vender")) {
                            ImGui::TextDisabled("Selecione um item da mochila para vender.");
                            ImGui::Spacing();
                            ImGui::BeginChild("##sell_list", {0, sh_win - 120.f}, false);
                            for (int s = rco::ui::Inventory::kBackpackStart;
                                 s < rco::ui::Inventory::kTotalSlots; ++s) {
                                const auto& inv_it = inventory.GetSlot(s);
                                if (inv_it.empty()) continue;
                                char row[128];
                                snprintf(row, sizeof(row), "%s", inv_it.name.c_str());
                                ImGui::Text("%s", row);
                                ImGui::SameLine(kSW - 100.f);
                                char btn[32];
                                snprintf(btn, sizeof(btn), "Vender##s%d", s);
                                if (ImGui::SmallButton(btn) && conn.IsConnected()) {
                                    rco::net::Writer w;
                                    w.WriteU8(1); // sell
                                    w.WriteU16(static_cast<uint16_t>(s));
                                    w.WriteU8(1);
                                    conn.SendPacket(rco::net::kPShopAction, w);
                                    audio.PlaySfx(rco::audio::SfxId::SellItem);
                                }
                                ImGui::Separator();
                            }
                            ImGui::EndChild();
                            ImGui::EndTabItem();
                        }
                        ImGui::EndTabBar();
                    }

                    ImGui::End();
                }

                // Portal markers.
                if (!area_portals.empty()) {
                    auto* dl = ImGui::GetForegroundDrawList();
                    float sw2 = static_cast<float>(window.Width());
                    float sh2 = static_cast<float>(window.Height());
                    float t   = static_cast<float>(ImGui::GetTime());
                    for (const auto& p : area_portals) {
                        // Animated pillar: sample a few points around the ring.
                        constexpr int kSegs = 16;
                        constexpr float kH  = 3.5f; // pillar height world units
                        ImVec2 prev_bot{}, prev_top{};
                        bool   first = true;
                        for (int s = 0; s <= kSegs; ++s) {
                            float a = (s % kSegs) * (2.f * 3.14159f / kSegs);
                            float rx = p.x + p.radius * cosf(a);
                            float rz = p.z + p.radius * sinf(a);
                            float ry_bot = 0.f;
                            float ry_top = kH;

                            auto project = [&](float wx, float wy, float wz) -> std::pair<ImVec2,bool> {
                                glm::vec4 c = proj_mat * view_mat * glm::vec4(wx, wy, wz, 1.f);
                                if (c.w <= 0.f) return {{}, false};
                                float sx = (c.x/c.w + 1.f) * 0.5f * sw2;
                                float sy = (1.f - c.y/c.w) * 0.5f * sh2;
                                return {{sx, sy}, true};
                            };

                            auto [bot, bok] = project(rx, ry_bot, rz);
                            auto [top, tok] = project(rx, ry_top, rz);

                            uint8_t alpha = static_cast<uint8_t>(180 + 60 * sinf(t * 2.f + a));
                            ImU32 col = IM_COL32(80, 160, 255, alpha);

                            if (!first && bok) {
                                dl->AddLine(prev_bot, bot, col, 1.5f);
                            }
                            if (bok && tok) {
                                dl->AddLine(bot, top, col, 1.f);
                            }
                            if (!first && tok) {
                                dl->AddLine(prev_top, top, col, 1.5f);
                            }
                            prev_bot = bot; prev_top = top;
                            first = false;
                        }

                        // Label above the portal.
                        auto [lp, lok] = [&]() -> std::pair<ImVec2,bool> {
                            glm::vec4 c = proj_mat * view_mat * glm::vec4(p.x, kH + 0.5f, p.z, 1.f);
                            if (c.w <= 0.f) return {{}, false};
                            return {ImVec2{(c.x/c.w+1.f)*0.5f*sw2, (1.f-c.y/c.w)*0.5f*sh2}, true};
                        }();
                        if (lok) {
                            char lbl[64];
                            snprintf(lbl, sizeof(lbl), "→ %s", p.target_area.c_str());
                            ImVec2 ts = ImGui::CalcTextSize(lbl);
                            dl->AddText({lp.x - ts.x*0.5f, lp.y},
                                        IM_COL32(120, 200, 255, 220), lbl);
                        }
                    }
                }

                // -- F6 performance overlay --
                {
                    static bool perf_open = false;
                    if (ImGui::IsKeyPressed(ImGuiKey_F6)) perf_open = !perf_open;
                    if (perf_open && pipeline) {
                        static float fps_buf[60] = {};
                        static int   fps_idx     = 0;
                        fps_buf[fps_idx % 60]    = (dt > 0.f) ? 1.f / dt : 0.f;
                        fps_idx++;
                        float avg_fps = 0.f;
                        for (float f : fps_buf) avg_fps += f;
                        avg_fps /= 60.f;

                        auto stats = pipeline->LastFrameStats();
                        ImGui::SetNextWindowPos({10, 50}, ImGuiCond_Once);
                        ImGui::SetNextWindowSize({230, 0}, ImGuiCond_Always);
                        ImGui::Begin("Perf [F6]", &perf_open, ImGuiWindowFlags_AlwaysAutoResize);
                        ImGui::Text("FPS        : %.1f  (%.2f ms)", avg_fps, dt * 1000.f);
                        ImGui::Text("Triangles  : %d", stats.triangles);
                        ImGui::Text("Draw calls : %d", stats.draw_calls);
                        ImGui::End();
                    }
                }

                // -- F7 animation debug overlay (must be inside ImGui frame) --
                {
                    static bool anim_debug_open = false;
                    if (ImGui::IsKeyPressed(ImGuiKey_F7)) anim_debug_open = !anim_debug_open;

                    if (anim_debug_open) {
                        ImGui::SetNextWindowPos({320, 10}, ImGuiCond_Once);
                        ImGui::SetNextWindowSize({420, 0}, ImGuiCond_Always);
                        ImGui::Begin("Animation Debug [F7]", &anim_debug_open,
                            ImGuiWindowFlags_AlwaysAutoResize);

                        ImGui::SeparatorText("Player");
                        if (player_anim_ctrl.IsReady()) {
                            const std::string& act = player_anim_ctrl.CurrentAction();
                            float t = player_anim_ctrl.CurrentTime();
                            ImGui::Text("Action : %s", act.c_str());
                            ImGui::Text("Time   : %.3f s", t);
                            ImGui::Text("Clips  : %d", player_actor.model().ClipCount());
                            int cidx = -1;
                            for (int ci = 0; ci < player_actor.model().ClipCount(); ++ci)
                                if (player_actor.model().ClipName(ci) == act) { cidx = ci; break; }
                            float dur = (cidx >= 0) ? player_actor.model().ClipDuration(cidx) : 0.f;
                            if (dur > 0.f)
                                ImGui::ProgressBar(std::fmod(t, dur) / dur, {-1, 0}, act.c_str());
                        } else {
                            ImGui::Text("AnimController not ready (no bindings)");
                            ImGui::Text("Legacy anim: %s  t=%.3f",
                                player_actor.CurrentAnim().c_str(), player_actor.AnimTime());
                        }
                        if (ImGui::TreeNode("All clips")) {
                            for (int ci = 0; ci < player_actor.model().ClipCount(); ++ci)
                                ImGui::Text("[%d] %s  (%.2fs)", ci,
                                    player_actor.model().ClipName(ci).c_str(),
                                    player_actor.model().ClipDuration(ci));
                            ImGui::TreePop();
                        }

                        ImGui::SeparatorText("NPCs");
                        int shown = 0;
                        for (auto& [rid, e] : world_actors) {
                            if (!e.actor) continue;
                            if (shown++ > 8) { ImGui::Text("... (+%d more)", (int)world_actors.size()-8); break; }
                            ImGui::PushID(rid);
                            const char* act = e.anim_ctrl.IsReady()
                                ? e.anim_ctrl.CurrentAction().c_str()
                                : e.anim_name.c_str();
                            float t = e.anim_ctrl.IsReady()
                                ? e.anim_ctrl.CurrentTime() : e.anim_t;
                            ImGui::Text("rid=%-4u  %-16s  t=%.2f  %s",
                                rid, e.name.c_str(), t, act);
                            ImGui::PopID();
                        }
                        ImGui::End();
                    }
                }

                break;
            }
        }

        // ---- Render ImGui draw data ----
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // ---- End frame ----
        window.EndFrame();
    }

    // -----------------------------------------------------------------------
    // Shutdown
    // -----------------------------------------------------------------------
    perf_entry.Finalize("main_loop_exit");
    rco::renderer::ModelCacheSetObserver(nullptr);
    g_entry_perf_logger = nullptr;

    pipeline.reset();
    engine.Shutdown();

    player_actor.Destroy();
    terrain.Destroy();
    particles.Shutdown();
    audio.Shutdown();
    conn.Disconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    return 0;
}

