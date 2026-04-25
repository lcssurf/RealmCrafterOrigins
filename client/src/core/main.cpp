#include <cstdio>
#include <cmath>
#include <memory>
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
#include "paths.h"
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
#include "../renderer/terrain/terrain.h"
#include "../renderer/actors/actor.h"
#include "../renderer/particles.h"
#include "../audio/audio.h"

#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"

// Scroll callback routes wheel input to the camera.
static rco::renderer::Camera* g_camera = nullptr;
static void ScrollCallback(GLFWwindow*, double, double y) {
    if (g_camera) g_camera->ProcessScroll(static_cast<float>(y));
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

    // -----------------------------------------------------------------------
    // Game state
    // -----------------------------------------------------------------------
    rco::GameState               state = rco::GameState::Login;
    rco::PlayerState             player{};
    std::vector<rco::CharacterInfo> characters;
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
    };

    struct WorldActorEntry {
        float x = 0, y = 0, z = 0, yaw = 0;
        std::string name;
        uint16_t level = 1;
        int32_t health = 100, health_max = 100;
        uint8_t     actor_type = 0; // 0=player, 1=combat NPC, 2=dialog NPC
        std::string anim_name  = "Idle";
        float       anim_t     = 0.f;

        // Appearance from the Media Actor Def bound to this NPC (if any).
        std::vector<WorldMesh> meshes;
        std::vector<WorldAnim> anims;

        // Per-actor rendering — lazily instantiated from `meshes` on first
        // render when renderer_ready is true. nullptr = fall back to the
        // shared player_actor model.
        std::unique_ptr<rco::renderer::Actor> actor;

        WorldActorEntry() = default;
        WorldActorEntry(WorldActorEntry&&) = default;
        WorldActorEntry& operator=(WorldActorEntry&&) = default;
        WorldActorEntry(const WorldActorEntry&) = delete;
        WorldActorEntry& operator=(const WorldActorEntry&) = delete;
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
    rco::renderer::Terrain terrain;
    rco::renderer::Actor   player_actor;
    bool renderer_ready = false;

    rco::renderer::Engine engine;
    std::unique_ptr<rco::renderer::Pipeline> pipeline;

    rco::ui::Chat            chat;
    rco::ui::Inventory       inventory;
    rco::ui::FloatingNumbers float_nums;
    rco::ui::SpellBar        spellbar;
    rco::ui::SpellEffects    spell_fx;
    rco::ui::ChatBubbles     chat_bubbles;

    rco::renderer::ParticleSystem particles;
    rco::audio::AudioSystem       audio;

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

    // Combat state
    uint32_t combat_target     = 0;
    double   last_attack_sent  = 0.0;
    bool     player_dead       = false;
    glm::mat4 view_mat{1.f}, proj_mat{1.f};

    // Click-to-move state
    glm::vec3 move_target{0.f};
    bool      has_move_target    = false;
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
                // Seed character sheet stats
                inventory.stat_name   = player.name;
                inventory.stat_race   = player.race;
                inventory.stat_class  = player.charClass;
                inventory.stat_hp     = player.health;
                inventory.stat_hp_max = player.healthMax;
                inventory.stat_ep     = player.energy;
                inventory.stat_ep_max = player.energyMax;
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
                world_items.clear();
                shop.open = false;
                combat_target = 0;
                spellbar.Clear();
                spell_fx.Clear();
                chat_bubbles.Clear();
                dialog.open = false;
                // Reload editor-painted terrain for the new area (no-op if not found)
                if (renderer_ready) terrain.LoadFromEditor(area);
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
                    std::string action, source_path, clip_override;
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
                uint8_t num_anims = r.ReadU8();
                std::vector<IncomingAnim> anims;
                anims.reserve(num_anims);
                for (uint8_t i = 0; i < num_anims && r.OK(); ++i) {
                    IncomingAnim a;
                    a.action        = r.ReadString();
                    a.source_path   = r.ReadString();
                    a.clip_override = r.ReadString();
                    anims.push_back(std::move(a));
                }
                if (!r.OK()) break;

                auto& e = world_actors[rid];  // in-place; avoids copy (actor is unique_ptr)
                e.x = x; e.y = y; e.z = z; e.yaw = yaw;
                e.name = name; e.level = level;
                e.health = hp; e.health_max = hpmax;
                e.actor_type = atype;
                e.anim_name = "Idle";
                e.anim_t    = 0.f;

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
                    e.anims.push_back({std::move(a.action),
                                       std::move(a.source_path),
                                       std::move(a.clip_override)});
                }
                // Drop any pre-existing Actor — it'll be rebuilt on next render
                // against the (possibly new) appearance data.
                e.actor.reset();

                std::fprintf(stderr,
                    "[PNewActor] rid=%u name=%s pos=(%.1f,%.1f,%.1f) "
                    "meshes=%u anims=%u\n",
                    rid, e.name.c_str(), x, y, z,
                    (unsigned)e.meshes.size(), (unsigned)e.anims.size());
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

            case rco::net::kPActorDead: {
                uint32_t dead_rid   = r.ReadU32();
                /*killer*/ r.ReadU32();
                if (!r.OK()) break;
                if (dead_rid == player.runtimeId) {
                    player_dead    = true;
                    player.health  = 0;
                    combat_target  = 0;
                    audio.PlaySfx(rco::audio::SfxId::PlayerDeath);
                } else {
                    world_actors.erase(dead_rid);
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

            case rco::net::kPAnimateActor: {
                uint32_t    rid  = r.ReadU32();
                std::string name = r.ReadString();
                if (!r.OK()) break;
                auto it = world_actors.find(rid);
                if (it != world_actors.end()) {
                    it->second.anim_name = name;
                    it->second.anim_t    = 0.f;
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
                        inventory.stat_hp     = player.health;
                        inventory.stat_hp_max = player.healthMax;
                        inventory.stat_ep     = player.energy;
                        inventory.stat_ep_max = player.energyMax;
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

            case rco::net::kPKnownSpells: {
                spellbar.Clear();
                uint8_t count = r.ReadU8();
                for (uint8_t i = 0; i < count; ++i) {
                    uint16_t    spell_id    = r.ReadU16();
                    std::string name        = r.ReadString();
                    uint8_t     spell_type  = r.ReadU8();
                    uint16_t    ep_cost     = r.ReadU16();
                    uint32_t    cooldown_ms = r.ReadU32();
                    float       range       = r.ReadF32();
                    /*icon*/                 r.ReadU8();
                    uint8_t     aoe_type    = r.ReadU8();
                    float       aoe_radius  = r.ReadF32();
                    if (!r.OK()) break;
                    spellbar.AddSpell(spell_id, name, spell_type, ep_cost, cooldown_ms,
                                      aoe_type, aoe_radius, range);
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

            case rco::net::kPPing: {
                rco::net::Writer w;
                conn.SendPacket(rco::net::kPPong, w);
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
    audio.Init();

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
                // --- New Engine/Pipeline (running in parallel to the old renderer during phase 4) ---
                rco::renderer::EngineConfig ecfg{};
                ecfg.width      = window.Width();
                ecfg.height     = window.Height();
                ecfg.shader_dir = "shaders/";
                engine.Init(ecfg);
                engine.LoadEnvironment("assets/ibl/default.hdr");
                pipeline = std::make_unique<rco::renderer::Pipeline>(engine);
                std::fprintf(stderr, "[engine] init ok\n");

                if (terrain.Init()) {
                    player_actor.Init("shaders", "assets/models/player.glb",
                                      &engine.materials());
                    engine.RebuildMaterialsBuffer();
                    particles.Init();
                    renderer_ready = true;
                    terrain.LoadFromEditor(player.areaName);
                    player.y = terrain.SampleHeight(player.x, player.z);
                    camera.SnapTarget({player.x, player.y, player.z});
                } else {
                    std::fprintf(stderr, "[renderer] Failed to load shaders — check shaders/ directory\n");
                }
            }

            if (renderer_ready) {
                GLFWwindow* w = window.Handle();

                // ---- RMB held: lock cursor + mouselook (RC 1.26 style) ----
                // Holding RMB decouples the camera from the character: the
                // mouse rotates CAM yaw/pitch freely, but the character keeps
                // facing wherever A/D pointed it. Releasing RMB does NOT
                // re-sync the camera — only W/S without RMB does that.
                {
                    static bool   rmb_prev = false;
                    static double prev_mx  = 0, prev_my = 0;
                    bool cur_rmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

                    if (cur_rmb && !rmb_prev) {
                        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                        glfwGetCursorPos(w, &prev_mx, &prev_my);
                    }
                    if (!cur_rmb && rmb_prev)
                        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

                    if (cur_rmb) {
                        double cx, cy;
                        glfwGetCursorPos(w, &cx, &cy);
                        camera.ApplyMouseDelta((float)(cx - prev_mx), (float)(cy - prev_my));
                        prev_mx = cx; prev_my = cy;
                        // NOTE: player.yaw intentionally NOT touched here —
                        // RC does not rotate the character during mouselook.
                    }
                    rmb_prev = cur_rmb;
                }

                // ---- RC 1.26 keyboard movement ----
                // W/S : walk forward/back along the CHARACTER's facing.
                //       Without RMB held, W/S also centre the camera behind
                //       the character (yaw=player.yaw, pitch=0) — this is
                //       the classic RC "snap follow" when you start walking.
                // A/D : rotate the character. Camera keeps its current
                //       relative offset (AddYaw by the same delta) so the
                //       chase angle stays consistent through turns.
                // Q/E : strafe (RCO extension — RC 1.26 has no strafe key).
                if (!player_dead && !ImGui::GetIO().WantCaptureKeyboard) {
                    bool rmb_held = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                    constexpr float kSpeed    = 8.f;
                    constexpr float kTurnRate = 150.f;

                    bool moving_fwd  = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS;
                    bool moving_back = glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS;

                    // A/D — turn the character. Camera follows 1:1 so the
                    // relative chase-cam offset stays fixed during turns.
                    float turn = 0.f;
                    if (glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS) turn += kTurnRate * dt;
                    if (glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS) turn -= kTurnRate * dt;
                    if (turn != 0.f) {
                        player.yaw += turn;
                        camera.AddYaw(turn);
                    }

                    // W/S without RMB → snap camera directly behind the player.
                    if ((moving_fwd || moving_back) && !rmb_held) {
                        camera.SetYaw(player.yaw);
                        camera.SetPitch(0.f);
                    }

                    float fwd = 0.f;
                    if (moving_fwd)  fwd =  kSpeed;
                    if (moving_back) fwd = -kSpeed * 0.65f;

                    float strafe = 0.f;
                    if (glfwGetKey(w, GLFW_KEY_Q) == GLFW_PRESS) strafe -= kSpeed;
                    if (glfwGetKey(w, GLFW_KEY_E) == GLFW_PRESS) strafe += kSpeed;

                    // Move in the CHARACTER's facing direction, not the camera's.
                    float yr = glm::radians(player.yaw);
                    glm::vec2 fdir = { -std::sin(yr), -std::cos(yr) };
                    glm::vec2 rdir = {  std::cos(yr), -std::sin(yr) };
                    glm::vec2 vel  = fdir * fwd + rdir * strafe;

                    if (vel.x*vel.x + vel.y*vel.y > 0.001f) {
                        player.x += vel.x * dt;
                        player.z += vel.y * dt;
                        player.y  = terrain.SampleHeight(player.x, player.z);
                        has_move_target = false;
                    }
                }

                // ---- Click-to-move: move player toward target ----
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
                camera.Update(dt);

                float aspect = window.Width() / static_cast<float>(window.Height());
                glm::mat4 view = camera.View();
                glm::mat4 proj = camera.Projection(aspect);
                view_mat = view;
                proj_mat = proj;
                glm::vec3 sun  = glm::normalize(glm::vec3(0.3f, 1.f, 0.5f));

                // -- F8 cycles debug viz (0=full,1=albedo,2=normal,3=depth,4=AO,
                // 5=shadow,6=irradiance,7=NoL,8=albedo*NoL,9=envDiffuse,10=direct).
                {
                    static bool f8_prev = false;
                    bool f8_cur = glfwGetKey(w, GLFW_KEY_F8) == GLFW_PRESS;
                    if (f8_cur && !f8_prev) {
                        int m = (pipeline->DebugMode() + 1) % 11;
                        pipeline->SetDebugMode(m);
                        const char* names[] = {
                            "0 FULL", "1 albedo", "2 normal", "3 depth", "4 AO",
                            "5 shadow", "6 irradiance", "7 NoL", "8 albedo*NoL",
                            "9 envDiffuse", "10 direct (no shadow)"
                        };
                        std::fprintf(stderr, "[debug] gPhongGlobal mode = %s\n", names[m]);
                    }
                    f8_prev = f8_cur;
                }

                // --- New pipeline: begin frame, submit all scene geometry, end writes to framebuffer 0 ---
                pipeline->Begin(view, proj, camera.Position(), static_cast<float>(dt));
                pipeline->SetSun(-sun, glm::vec3(1.0f, 0.95f, 0.80f));
                terrain.Submit(*pipeline);

                // Render local player
                {
                    bool moving = glm::length(glm::vec2(player.x - last_player_pos.x,
                                                        player.z - last_player_pos.y)) > 0.02f;
                    const std::string& cur = player_actor.CurrentAnim();
                    if (player_dead) {
                        if (cur != "Death") player_actor.PlayAnim("Death", false);
                    } else if (moving) {
                        if (cur != "Walk")  player_actor.PlayAnim("Walk",  true);
                    } else {
                        if (cur != "Idle")  player_actor.PlayAnim("Idle",  true);
                    }
                    player_actor.position = {player.x, player.y, player.z};
                    player_actor.yaw      = player.yaw;
                    player_actor.Update(dt);
                    player_actor.Submit(*pipeline);
                }

                // Render all other actors (NPCs + other players)
                for (auto& [rid, e] : world_actors) {
                    e.anim_t += dt;

                    // Lazy init: build the per-NPC Actor now that renderer_ready
                    // is true and we have appearance data from PNewActor.
                    if (!e.actor && !e.meshes.empty()) {
                        const WorldMesh* body = nullptr;
                        for (auto& m : e.meshes) {
                            if (m.slot == 0) { body = &m; break; }
                        }
                        if (!body) body = &e.meshes.front();

                        auto a = std::make_unique<rco::renderer::Actor>();
                        a->Init("shaders", body->model_path.c_str(),
                                &engine.materials());
                        a->scale = body->scale > 0.f ? body->scale : 1.f;

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
                                std::fprintf(stderr,
                                    "[matmap] rid=%u ai='%s' albedo='%s' normal='%s' orm='%s'\n",
                                    rid, am.ai_name.c_str(),
                                    am.albedo.c_str(), am.normal.c_str(), am.orm.c_str());
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
                        engine.RebuildMaterialsBuffer();

                        for (auto& an : e.anims) {
                            if (!an.source_path.empty()) {
                                a->LoadAnim(an.source_path.c_str(), an.action.c_str());
                            }
                        }
                        a->PlayAnim("Idle", true);
                        std::fprintf(stderr,
                            "[Actor init] rid=%u model=%s scale=%.2f "
                            "mat=albedo:'%s' normal:'%s' orm:'%s' extra_anims=%zu\n",
                            rid, body->model_path.c_str(), a->scale,
                            body->albedo.c_str(), body->normal.c_str(), body->orm.c_str(),
                            e.anims.size());
                        e.actor = std::move(a);
                    }

                    // Y is server-authoritative — the GUE places NPCs on
                    // terrain and writes the hit-point Y to npc_spawns.y,
                    // which reaches us via PNewActor. Resampling here would
                    // hide that and leave NPCs floating when terrain has
                    // been edited without re-broadcasting spawns.
                    glm::vec3 pos = {e.x, e.y, e.z};
                    const bool loop_flag = (e.anim_name != "Attack" && e.anim_name != "Death");

                    if (e.actor) {
                        e.actor->position = pos;
                        e.actor->yaw      = e.yaw;
                        e.actor->SubmitAs(e.anim_name, e.anim_t, loop_flag, *pipeline);
                    } else {
                        player_actor.position = pos;
                        player_actor.yaw      = e.yaw;
                        player_actor.SubmitAs(e.anim_name, e.anim_t, loop_flag, *pipeline);
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

                // Particles render inside pipeline->End() forward-pass callback above.
                // HDRI skybox is now drawn by pipeline->skyboxPass_().

                // Left-click: select closest actor within 55 px of cursor.
                {
                    static bool prev_lmb = false;
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

                    bool cur_lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    // Ground-AoE click: consume LMB and fire the spell.
                    if (cur_lmb && !prev_lmb && spellbar.pending_ground_spell != 0
                        && !ImGui::GetIO().WantCaptureMouse) {
                        if (spellbar.on_cast_ground)
                            spellbar.on_cast_ground(spellbar.pending_ground_spell,
                                                    ground_cursor_x, ground_cursor_z);
                        spellbar.pending_ground_spell = 0;
                        prev_lmb = cur_lmb;
                    } else
                    if (cur_lmb && !prev_lmb && !player_dead && !ImGui::GetIO().WantCaptureMouse) {
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

                // ---- Tab: cycle to nearest hostile actor ----
                {
                    static bool tab_prev = false;
                    bool tab_cur = glfwGetKey(w, GLFW_KEY_TAB) == GLFW_PRESS;
                    if (tab_cur && !tab_prev && !player_dead) {
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
                ImGui::SetNextWindowSize({300.f, 125.f}, ImGuiCond_Always);
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
                ImGui::Text("Gold: %u    Pos: %.1f, %.1f, %.1f",
                    player_gold, player.x, player.y, player.z);
                ImGui::TextDisabled("RMB move  Q/E rotate  Scroll zoom  LMB target  I bag  C character  F pickup");
                ImGui::End();

                // Toggle bag with I, character sheet with C
                if (ImGui::IsKeyPressed(ImGuiKey_I) && !player_dead && !ImGui::GetIO().WantTextInput)
                    inventory.bag_visible = !inventory.bag_visible;
                if (ImGui::IsKeyPressed(ImGuiKey_C) && !player_dead && !ImGui::GetIO().WantTextInput)
                    inventory.char_visible = !inventory.char_visible;

                // Close shop with Escape
                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                    shop.open = false;

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
                                combat_target, static_cast<float>(now), player_dead,
                                player.energy, target_dist_for_spells);

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
