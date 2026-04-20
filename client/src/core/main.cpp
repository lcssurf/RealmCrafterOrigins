#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>

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
#include "../ui/spell_effects.h"
#include "../ui/chat_bubbles.h"
#include "../renderer/camera.h"
#include "../renderer/skybox.h"
#include "../renderer/terrain/terrain.h"
#include "../renderer/actors/actor.h"

// Scroll callback routes wheel input to the camera.
static rco::renderer::Camera* g_camera = nullptr;
static void ScrollCallback(GLFWwindow*, double, double y) {
    if (g_camera) g_camera->ProcessScroll(static_cast<float>(y));
}

int main() {
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

    // -----------------------------------------------------------------------
    // Game state
    // -----------------------------------------------------------------------
    rco::GameState               state = rco::GameState::Login;
    rco::PlayerState             player{};
    std::vector<rco::CharacterInfo> characters;
    std::string                  login_error;

    struct WorldActorEntry {
        float x, y, z, yaw;
        std::string name;
        uint16_t level = 1;
        int32_t health = 100, health_max = 100;
        uint8_t actor_type = 0; // 0=player, 1=combat NPC, 2=dialog NPC
    };
    std::unordered_map<uint32_t, WorldActorEntry> world_actors;

    struct PortalEntry {
        float x, z, radius;
        std::string target_area;
    };
    std::vector<PortalEntry> area_portals;

    // -----------------------------------------------------------------------
    // Renderer
    // -----------------------------------------------------------------------
    rco::renderer::Camera  camera;
    rco::renderer::Skybox  skybox;
    rco::renderer::Terrain terrain;
    rco::renderer::Actor   player_actor;
    bool renderer_ready = false;

    rco::ui::Chat            chat;
    rco::ui::Inventory       inventory;
    rco::ui::FloatingNumbers float_nums;
    rco::ui::SpellBar        spellbar;
    rco::ui::SpellEffects    spell_fx;
    rco::ui::ChatBubbles     chat_bubbles;

    struct DialogState {
        bool                     open = false;
        std::string              npc_name;
        std::string              text;
        std::vector<std::string> options;
    } dialog;

    // Combat state
    uint32_t combat_target     = 0;
    double   last_attack_sent  = 0.0;
    bool     player_dead       = false;
    glm::mat4 view_mat{1.f}, proj_mat{1.f};

    // Click-to-move state
    glm::vec3 move_target{0.f};
    bool      has_move_target    = false;
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

    spellbar.on_cast = [&](uint16_t spell_id, uint32_t target_rid) {
        if (!conn.IsConnected()) return;
        rco::net::Writer w;
        w.WriteU16(spell_id);
        w.WriteU32(target_rid);
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
    };

    g_camera = &camera;
    glfwSetScrollCallback(window.Handle(), ScrollCallback);

    // -----------------------------------------------------------------------
    // Packet handler
    // -----------------------------------------------------------------------
    auto handle_packet = [&](const rco::net::InboundPacket& pkt) {
        rco::net::Reader r(pkt.payload.data(), pkt.payload.size());

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

            case rco::net::kPCharListResult: {
                characters.clear();
                uint8_t count = r.ReadU8();
                for (int i = 0; i < count && r.OK(); ++i) {
                    rco::CharacterInfo c;
                    c.slot      = static_cast<int>(r.ReadU8());
                    c.name      = r.ReadString();
                    c.race      = r.ReadString();
                    c.charClass = r.ReadString();
                    c.level     = r.ReadU16();
                    c.area      = r.ReadString();
                    c.health    = static_cast<int32_t>(r.ReadU32());
                    c.healthMax = static_cast<int32_t>(r.ReadU32());
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
                player.runtimeId = r.ReadU32();
                player.areaName  = r.ReadString();
                player.x         = r.ReadF32();
                player.y         = r.ReadF32();
                player.z         = r.ReadF32();
                player.yaw       = r.ReadF32();
                player.health    = static_cast<int32_t>(r.ReadU32());
                player.healthMax = static_cast<int32_t>(r.ReadU32());
                player.energy    = static_cast<int32_t>(r.ReadU32());
                player.energyMax = static_cast<int32_t>(r.ReadU32());
                if (!r.OK()) break;
                state = rco::GameState::InGame;
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
                world_actors.clear();
                area_portals.clear();
                combat_target = 0;
                spellbar.Clear();
                spell_fx.Clear();
                chat_bubbles.Clear();
                dialog.open = false;
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
                if (!r.OK()) break;
                WorldActorEntry e;
                e.x = x; e.y = y; e.z = z; e.yaw = yaw;
                e.name = name; e.level = level;
                e.health = hp; e.health_max = hpmax;
                e.actor_type = atype;
                world_actors[rid] = e;
                break;
            }

            case rco::net::kPActorGone: {
                uint32_t rid = r.ReadU32();
                if (!r.OK()) break;
                world_actors.erase(rid);
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
                            by = renderer_ready ? terrain.SampleHeight(e.x, e.z) : e.y;
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

            case rco::net::kPActorDead: {
                uint32_t dead_rid   = r.ReadU32();
                /*killer*/ r.ReadU32();
                if (!r.OK()) break;
                if (dead_rid == player.runtimeId) {
                    player_dead    = true;
                    player.health  = 0;
                    combat_target  = 0;
                } else {
                    world_actors.erase(dead_rid);
                    if (combat_target == dead_rid) combat_target = 0;
                }
                break;
            }

            case rco::net::kPRepositionActor: {
                uint32_t rid = r.ReadU32();
                float rx = r.ReadF32(), ry = r.ReadF32(),
                      rz = r.ReadF32(), ryaw = r.ReadF32();
                if (!r.OK()) break;
                if (rid == player.runtimeId) {
                    player.x = rx; player.y = ry;
                    player.z = rz; player.yaw = ryaw;
                    player_dead = false;
                } else {
                    auto it = world_actors.find(rid);
                    if (it != world_actors.end()) {
                        it->second.x = rx; it->second.y = ry;
                        it->second.z = rz; it->second.yaw = ryaw;
                    }
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
                        if      (attr == 0) { player.health    = val; if (val > 0) player_dead = false; }
                        else if (attr == 1) { player.healthMax = val; }
                        else if (attr == 2) { player.energy    = val; }
                        else if (attr == 3) { player.energyMax = val; }
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

            case rco::net::kPXPUpdate: {
                uint16_t lvl     = r.ReadU16();
                uint32_t xp      = r.ReadU32();
                uint32_t xp_next = r.ReadU32();
                if (!r.OK()) break;
                player.level   = lvl;
                player.xp      = xp;
                player.xp_next = xp_next;
                break;
            }

            case rco::net::kPKnownSpells: {
                spellbar.Clear();
                uint8_t count = r.ReadU8();
                for (uint8_t i = 0; i < count; ++i) {
                    uint16_t    spell_id    = r.ReadU16();
                    std::string name        = r.ReadString();
                    uint8_t     spell_type  = r.ReadU8();
                    uint16_t    ep_cost     = r.ReadU16();
                    uint32_t    cooldown_ms = r.ReadU32();
                    /*icon*/                 r.ReadU8();
                    if (!r.OK()) break;
                    spellbar.AddSpell(spell_id, name, spell_type, ep_cost, cooldown_ms);
                }
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

            case rco::net::kPPing: {
                rco::net::Writer w;
                conn.SendPacket(rco::net::kPPong, w);
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
                if (ch.slot == slot) { player.name = ch.name; break; }
            }
            rco::net::Writer w;
            w.WriteU8(static_cast<uint8_t>(slot));
            conn.SendPacket(rco::net::kPStartGame, w);
        },
        .OnCreate = [&](int slot,
                        const std::string& name,
                        const std::string& race,
                        const std::string& cls,
                        int gender) {
            rco::net::Writer w;
            w.WriteU8(static_cast<uint8_t>(slot));
            w.WriteString(name);
            w.WriteString(race);
            w.WriteString(cls);
            w.WriteU8(static_cast<uint8_t>(gender));
            conn.SendPacket(rco::net::kPCreateCharacter, w);
        },
        .OnDelete = [&](int slot) {
            rco::net::Writer w;
            w.WriteU8(static_cast<uint8_t>(slot));
            conn.SendPacket(rco::net::kPDeleteCharacter, w);
        },
        .OnLogout = [&]() {
            conn.Disconnect();
            state          = rco::GameState::Login;
            renderer_ready = false;
            characters.clear();
            world_actors.clear();
            login_error.clear();
        }
    });

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
    double last_time = glfwGetTime();

    while (!window.ShouldClose()) {

        double now = glfwGetTime();
        float  dt  = static_cast<float>(now - last_time);
        last_time  = now;

        // ---- Network poll ----
        {
            rco::net::InboundPacket pkt;
            while (conn.Poll(pkt)) handle_packet(pkt);
        }

        // ---- Begin frame (clears buffers, polls events) ----
        window.BeginFrame();

        // ---- 3D world (rendered before ImGui so HUD draws on top) ----
        if (state == rco::GameState::InGame) {

            // Lazy-init renderer on first InGame frame
            if (!renderer_ready) {
                if (skybox.Init("shaders") && terrain.Init("shaders")) {
                    player_actor.Init("shaders", "assets/models/player.glb");
                    renderer_ready = true;
                    player.y = terrain.SampleHeight(player.x, player.z);
                } else {
                    std::fprintf(stderr, "[renderer] Failed to load shaders — check shaders/ directory\n");
                }
            }

            if (renderer_ready) {
                GLFWwindow* w = window.Handle();

                // ---- Right-click: set move target via ray-terrain intersection ----
                {
                    static bool prev_rmb = false;
                    bool cur_rmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                    if (cur_rmb && !player_dead && !ImGui::GetIO().WantCaptureMouse) {
                        double mx, my;
                        glfwGetCursorPos(w, &mx, &my);
                        float sw2 = static_cast<float>(window.Width());
                        float sh2 = static_cast<float>(window.Height());
                        // Unproject mouse → world ray
                        float ndcX = (2.f * static_cast<float>(mx) / sw2) - 1.f;
                        float ndcY = 1.f - (2.f * static_cast<float>(my) / sh2);
                        glm::vec4 clipRay = {ndcX, ndcY, -1.f, 1.f};
                        glm::vec4 eyeRay  = glm::inverse(proj_mat) * clipRay;
                        eyeRay = {eyeRay.x, eyeRay.y, -1.f, 0.f};
                        glm::vec3 rayDir = glm::normalize(glm::vec3(glm::inverse(view_mat) * eyeRay));
                        glm::vec3 rayOri = camera.Position();
                        // Intersect with y=0 plane, then snap to terrain height
                        if (fabsf(rayDir.y) > 0.001f) {
                            float t = -rayOri.y / rayDir.y;
                            if (t > 0.f) {
                                float hx = rayOri.x + rayDir.x * t;
                                float hz = rayOri.z + rayDir.z * t;
                                move_target      = {hx, terrain.SampleHeight(hx, hz), hz};
                                has_move_target  = true;
                                pending_interact = 0; // manual move cancels pending interact
                            }
                        }
                    }
                    prev_rmb = cur_rmb;
                }

                // ---- Move player toward target ----
                if (has_move_target && !player_dead) {
                    constexpr float kSpeed = 8.f;
                    float dx = move_target.x - player.x;
                    float dz = move_target.z - player.z;
                    float d2 = dx * dx + dz * dz;
                    if (d2 > 0.08f * 0.08f) {
                        float dist = sqrtf(d2);
                        float step = kSpeed * dt < dist ? kSpeed * dt : dist;
                        player.x += (dx / dist) * step;
                        player.z += (dz / dist) * step;
                        player.y  = terrain.SampleHeight(player.x, player.z);
                        player.yaw = glm::degrees(std::atan2f(dx / dist, dz / dist));
                    } else {
                        has_move_target = false;
                    }
                }

                // Cancel movement on death.
                if (player_dead) { has_move_target = false; pending_interact = 0; }

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
                            has_move_target  = false;
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

                // Camera follows player
                camera.SetTarget({player.x, player.y, player.z});
                camera.Update(window.Handle(), dt);

                float aspect = window.Width() / static_cast<float>(window.Height());
                glm::mat4 view = camera.View();
                glm::mat4 proj = camera.Projection(aspect);
                view_mat = view;
                proj_mat = proj;
                glm::vec3 sun  = glm::normalize(glm::vec3(0.3f, 1.f, 0.5f));

                terrain.Render(view, proj, camera.Position(), sun);

                // Render local player
                player_actor.position = {player.x, player.y, player.z};
                player_actor.yaw      = player.yaw;
                player_actor.Render(view, proj, camera.Position(), sun);

                // Render all other actors (NPCs + other players)
                // Y is snapped to terrain height — server doesn't know client-side terrain yet.
                for (auto& [rid, e] : world_actors) {
                    player_actor.position = {e.x, terrain.SampleHeight(e.x, e.z), e.z};
                    player_actor.yaw      = e.yaw;
                    player_actor.Render(view, proj, camera.Position(), sun);
                }

                skybox.Render(view, proj);

                // Left-click: select closest actor within 55 px of cursor.
                {
                    static bool prev_lmb = false;
                    bool cur_lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    if (cur_lmb && !prev_lmb && !player_dead && !ImGui::GetIO().WantCaptureMouse) {
                        double mx, my;
                        glfwGetCursorPos(w, &mx, &my);
                        float best_dist2 = 55.f * 55.f;
                        uint32_t best_id = 0;
                        float sw = static_cast<float>(window.Width());
                        float sh = static_cast<float>(window.Height());
                        for (auto& [rid, e] : world_actors) {
                            float ey = terrain.SampleHeight(e.x, e.z) + 1.f;
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
                                    move_target       = {clicked.x, ny, clicked.z};
                                    has_move_target   = true;
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
                    prev_lmb = cur_lmb;
                }

                // Auto-attack: send PAttackActor every ~0.85 s while target is selected.
                static constexpr double kAutoAttackInterval = 0.85;
                if (combat_target && !player_dead && conn.IsConnected()
                    && now - last_attack_sent >= kAutoAttackInterval) {
                    rco::net::Writer aw;
                    aw.WriteU32(combat_target);
                    conn.SendPacket(rco::net::kPAttackActor, aw);
                    last_attack_sent = now;
                }
            }
        }

        // ---- ImGui ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        switch (state) {

            case rco::GameState::Login:
                login_screen.SetError(login_error);
                login_screen.Render();
                break;

            case rco::GameState::CharacterSelect:
                char_select.SetCharacters(characters);
                char_select.SetError(login_error);
                char_select.Render(window.Width(), window.Height());
                break;

            case rco::GameState::InGame: {
                // HUD
                ImGui::SetNextWindowPos({10.f, 10.f}, ImGuiCond_Always);
                ImGui::SetNextWindowSize({300.f, 110.f}, ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.55f);
                ImGui::Begin("##hud", nullptr,
                    ImGuiWindowFlags_NoDecoration  |
                    ImGuiWindowFlags_NoInputs       |
                    ImGuiWindowFlags_NoNav          |
                    ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("Area: %s  |  Lv %d", player.areaName.c_str(), (int)player.level);
                ImGui::Text("HP: %d / %d    EP: %d / %d",
                    player.health, player.healthMax,
                    player.energy, player.energyMax);
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
                ImGui::Text("Pos: %.1f, %.1f, %.1f",
                    player.x, player.y, player.z);
                ImGui::TextDisabled("RMB move  Q/E rotate  Scroll zoom  LMB target/interact  I inventory");
                ImGui::End();

                // Toggle inventory with I (when chat input is not focused)
                if (ImGui::IsKeyPressed(ImGuiKey_I) && !player_dead && !ImGui::GetIO().WantTextInput)
                    inventory.visible = !inventory.visible;

                // Chat
                chat.Render(window.Width(), window.Height(),
                            static_cast<float>(now));
                {
                    std::string msg;
                    if (chat.PollSend(msg) && conn.IsConnected()) {
                        rco::net::Writer w;
                        w.WriteU8(0); // channel: say
                        w.WriteString(msg);
                        conn.SendPacket(rco::net::kPChatMessage, w);
                    }
                }

                inventory.Render(window.Width(), window.Height());

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

                // Spell bar.
                spellbar.Render(window.Width(), window.Height(),
                                combat_target, static_cast<float>(now), player_dead,
                                player.energy);

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
    player_actor.Destroy();
    terrain.Destroy();
    skybox.Destroy();
    conn.Disconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    return 0;
}
