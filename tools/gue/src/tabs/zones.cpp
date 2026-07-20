#include "zones.h"
#include "media.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <chrono>

namespace gue {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static void InputScript(const char* label, char* scriptBuf, int sbLen,
                        char* funcBuf, int fbLen,
                        const std::vector<std::string>& scripts) {
    // Script combo
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
    if (ImGui::BeginCombo(label, scriptBuf[0] ? scriptBuf : "(none)")) {
        if (ImGui::Selectable("(none)", !scriptBuf[0])) { scriptBuf[0] = 0; funcBuf[0] = 0; }
        for (auto& s : scripts) {
            bool sel = (s == scriptBuf);
            if (ImGui::Selectable(s.c_str(), sel)) {
                std::strncpy(scriptBuf, s.c_str(), sbLen-1);
                funcBuf[0] = 0;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    // Func field on same line
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    char funcLabel[64];
    std::snprintf(funcLabel, sizeof(funcLabel), "##f%s", label);
    ImGui::InputTextWithHint(funcLabel, "function", funcBuf, fbLen);
}

// ─── Script list ──────────────────────────────────────────────────────────────

bool ZonesTab::IsInSelection(int type, int id) const {
    if (type == kSelNone || id < 0) return false;
    if (!selectedRefs_.empty()) {
        bool hasPrimary = false;
        for (const auto& ref : selectedRefs_) {
            if (ref.type == type && ref.id == id) return true;
            if (ref.type == selectedType_ && ref.id == selectedID_) hasPrimary = true;
        }
        if (!hasPrimary) {
            return (selectedType_ == type && selectedID_ == id);
        }
        return false;
    }
    return (selectedType_ == type && selectedID_ == id);
}

void ZonesTab::ClearSelection() {
    selectedID_ = -1;
    selectedType_ = kSelNone;
    selectedRefs_.clear();
}

void ZonesTab::SelectSingle(int type, int id) {
    if (type == kSelNone || id < 0) {
        ClearSelection();
        return;
    }
    selectedType_ = type;
    selectedID_ = id;
    selectedRefs_.clear();
    selectedRefs_.push_back({type, id});
}

void ZonesTab::AddSelection(int type, int id, bool makePrimary) {
    if (type == kSelNone || id < 0) return;
    if (!selectedRefs_.empty()) {
        bool hasPrimary = false;
        for (const auto& ref : selectedRefs_) {
            if (ref.type == selectedType_ && ref.id == selectedID_) {
                hasPrimary = true;
                break;
            }
        }
        if (!hasPrimary) selectedRefs_.clear();
    }
    if (selectedRefs_.empty() &&
        selectedID_ >= 0 && selectedType_ != kSelNone &&
        !(selectedType_ == type && selectedID_ == id)) {
        selectedRefs_.push_back({selectedType_, selectedID_});
    }
    if (!IsInSelection(type, id)) {
        selectedRefs_.push_back({type, id});
    }
    if (makePrimary) {
        selectedType_ = type;
        selectedID_ = id;
    } else if (selectedID_ < 0 || selectedType_ == kSelNone) {
        selectedType_ = type;
        selectedID_ = id;
    }
}

void ZonesTab::RemoveSelection(int type, int id) {
    if (type == kSelNone || id < 0) return;
    if (!selectedRefs_.empty()) {
        bool hasPrimary = false;
        for (const auto& ref : selectedRefs_) {
            if (ref.type == selectedType_ && ref.id == selectedID_) {
                hasPrimary = true;
                break;
            }
        }
        if (!hasPrimary) selectedRefs_.clear();
    }
    selectedRefs_.erase(
        std::remove_if(selectedRefs_.begin(), selectedRefs_.end(),
            [&](const SelectionRef& ref) { return ref.type == type && ref.id == id; }),
        selectedRefs_.end());

    if (selectedRefs_.empty()) {
        if (selectedType_ == type && selectedID_ == id) {
            selectedType_ = kSelNone;
            selectedID_ = -1;
        }
        return;
    }

    if (selectedType_ == type && selectedID_ == id) {
        selectedType_ = selectedRefs_.back().type;
        selectedID_ = selectedRefs_.back().id;
    }
}

void ZonesTab::ToggleSelection(int type, int id) {
    if (IsInSelection(type, id)) {
        RemoveSelection(type, id);
    } else {
        AddSelection(type, id, true);
    }
}

std::vector<ZonesTab::SelectionRef> ZonesTab::ActiveSelection() const {
    if (!selectedRefs_.empty()) {
        bool hasPrimary = false;
        for (const auto& ref : selectedRefs_) {
            if (ref.type == selectedType_ && ref.id == selectedID_) {
                hasPrimary = true;
                break;
            }
        }
        if (hasPrimary) return selectedRefs_;
    }
    if (selectedID_ >= 0 && selectedType_ != kSelNone)
        return {SelectionRef{selectedType_, selectedID_}};
    return {};
}

void ZonesTab::CopySelected() {
    selectionClipboard_ = ActiveSelection();
    if (selectionClipboard_.empty()) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Copy skipped: nothing selected.");
        return;
    }
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Copied %d object(s).", (int)selectionClipboard_.size());
}

void ZonesTab::PasteSelected(sqlite3* db, MediaTab* media) {
    if (selectionClipboard_.empty()) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Paste skipped: clipboard is empty.");
        return;
    }

    const int prevSelectedID = selectedID_;
    const int prevSelectedType = selectedType_;
    const auto prevRefs = selectedRefs_;
    selectedRefs_.clear();

    std::vector<SelectionRef> pastedRefs;
    const auto refs = selectionClipboard_;
    for (const auto& ref : refs) {
        if (ref.id < 0 || ref.type == kSelNone) continue;
        selectedID_ = ref.id;
        selectedType_ = ref.type;
        const int beforeID = selectedID_;
        const int beforeType = selectedType_;
        DuplicateSelected(db, media);
        if (selectedID_ >= 0 && selectedType_ != kSelNone &&
            !(selectedID_ == beforeID && selectedType_ == beforeType)) {
            pastedRefs.push_back({selectedType_, selectedID_});
        }
    }

    if (pastedRefs.empty()) {
        selectedID_ = prevSelectedID;
        selectedType_ = prevSelectedType;
        selectedRefs_ = prevRefs;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Paste skipped: no object could be duplicated.");
        return;
    }

    selectedRefs_ = std::move(pastedRefs);
    selectedType_ = selectedRefs_.back().type;
    selectedID_ = selectedRefs_.back().id;
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Pasted %d object(s).", (int)selectedRefs_.size());
}

void ZonesTab::EnsureScriptList() {
    if (scriptListLoaded_) return;
    scriptListLoaded_ = true;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator("../server/scripts", ec)) {
        if (e.is_regular_file()) {
            std::string name = e.path().stem().string();
            scriptList_.push_back(name);
        }
    }
    std::sort(scriptList_.begin(), scriptList_.end());
}

// ─── Area list ────────────────────────────────────────────────────────────────

void ZonesTab::FetchAreaList(sqlite3* db) {
    areaList_.clear();
    ZoneScene::EnsureTables(db);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT name FROM area_config ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* t = (const char*)sqlite3_column_text(stmt, 0);
            if (t) areaList_.push_back(t);
        }
        sqlite3_finalize(stmt);
    }
    needFetchAreas_ = false;
}

void ZonesTab::LoadTerrainMats(sqlite3* db) {
    terrainMats_.clear();
    if (!db) { terrainMatsLoaded_ = true; return; }
    sqlite3_stmt* s = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, name, albedo_path, normal_path, orm_path, normal_strength "
        "FROM media_materials ORDER BY name",
        -1, &s, nullptr);
    if (rc != SQLITE_OK) { terrainMatsLoaded_ = true; return; }
    while (sqlite3_step(s) == SQLITE_ROW) {
        TerrainMediaMat m;
        m.id = sqlite3_column_int(s, 0);
        auto text = [&](int col) -> std::string {
            auto p = sqlite3_column_text(s, col);
            return p ? (const char*)p : "";
        };
        m.name            = text(1);
        m.albedo_path     = text(2);
        m.normal_path     = text(3);
        m.orm_path        = text(4);
        m.normal_strength = (float)sqlite3_column_double(s, 5);
        terrainMats_.push_back(std::move(m));
    }
    sqlite3_finalize(s);
    terrainMatsLoaded_ = true;
}

void ZonesTab::LoadZone(sqlite3* db, MediaTab* media, const std::string& name) {
    scene_.LoadFromDB(db, name);
    renderer_.LoadTerrain(name);

    // Apply terrain material IDs saved in materials.txt via DB lookup
    LoadTerrainMats(db);
    auto& ter = renderer_.terrain();
    for (int i = 0; i < ter.NumMaterials(); ++i) {
        int id = ter.materialId(i);
        if (id <= 0) continue;
        for (auto& tm : terrainMats_) {
            if (tm.id != id) continue;
            TerrainMatSpec spec;
            spec.media_id        = tm.id;
            spec.name            = tm.name;
            spec.albedo_path     = tm.albedo_path;
            spec.normal_path     = tm.normal_path;
            spec.roughness_path  = tm.orm_path;
            spec.tiling          = ter.tilings[i];
            spec.normal_strength = tm.normal_strength;
            ter.SetMaterialSlot(i, spec);
            break;
        }
    }

    // Centre of a 512×512 heightmap at cell_size=2 → 1024×1024 world units.
    cam_.pos   = {512.f, 120.f, 300.f};
    cam_.yaw   = 0.f;
    cam_.pitch = 30.f;
    ClearSelection();
    if (media) {
        media->EnsureTables(db);
        if (media->ActorDefs().empty()) media->FetchAll(db);
        SyncSceneryCache(media);
    }
    undoStack_.clear();
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Loaded zone: %s", name.c_str());
}

void ZonesTab::SaveZone(sqlite3* db) {
    scene_.SaveToDB(db);
    scene_.SaveColData(db, scene_.areaName);
    bool terrainOk = true;
    if (renderer_.terrain().Loaded())
        terrainOk = renderer_.terrain().SaveArea();
    if (terrainOk)
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved zone: %s", scene_.areaName.c_str());
    else
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved zone (terrain write failed): %s", scene_.areaName.c_str());
}

// ─── Raycast ──────────────────────────────────────────────────────────────────

glm::vec3 ZonesTab::RaycastScene(float vpX, float vpY) {
    // Convert viewport pixel to NDC
    float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
    float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
    float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
    glm::vec3 ray = cam_.NDCRay(ndcX, ndcY, aspect);

    // Hit the terrain surface first so placed objects sit on the ground
    // at the actual elevation — without this, NPCs are stored with y=0
    // while the client renders them at terrain height, creating a mismatch.
    glm::vec3 hit;
    if (renderer_.terrain().Loaded()
        && renderer_.terrain().Raycast(cam_.pos, ray, hit)) {
        return hit;
    }

    // Fallback: intersect the Y=0 plane (flat-map mode / off-map clicks).
    if (std::abs(ray.y) > 1e-5f) {
        float t = -cam_.pos.y / ray.y;
        if (t > 0.f && t < ZoneCamera::kFarZ)
            return cam_.pos + ray * t;
    }
    // Last resort: 20 units in front of camera
    return cam_.pos + cam_.Forward() * 20.f;
}

// ─── PlaceObject ──────────────────────────────────────────────────────────────

void ZonesTab::PlaceObject(const glm::vec3& wpos, sqlite3* db, MediaTab* media) {
    (void)media;

    switch (zoneMode_) {
    case kModePortal: {
        ZPortal p;
        p.pos    = {wpos.x, 0.f, wpos.z};
        p.radius = portalRadius_;
        p.name   = portalNameBuf_[0] ? portalNameBuf_ : "Portal";
        p.linkArea   = portalLinkAreaBuf_;
        p.linkPortal = portalLinkNameBuf_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO area_portals (area_name, x, z, radius, target_area, dest_x, dest_y, dest_z, dest_yaw)"
            " VALUES (?,?,?,?,?,0,0,0,0)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, p.pos.x);
            sqlite3_bind_double(stmt, 3, p.pos.z);
            sqlite3_bind_double(stmt, 4, p.radius);
            sqlite3_bind_text(stmt, 5, p.linkArea.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            p.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.portals.push_back(p);
            selectedID_ = p.id; selectedType_ = kSelPortal;
            PushUndo(kUndoCreate, kSelPortal, p.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed portal '%s'.", p.name.c_str());
        }
        break;
    }
    case kModeTrigger: {
        ZTrigger t;
        t.x = wpos.x; t.z = wpos.z;
        t.radius = trigRadius_;
        t.script = trigScriptBuf_;
        t.func   = trigFuncBuf_;
        t.once   = trigOnce_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO area_triggers (area_name, x, z, radius, script, func, trigger_once)"
            " VALUES (?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, t.x); sqlite3_bind_double(stmt, 3, t.z);
            sqlite3_bind_double(stmt, 4, t.radius);
            sqlite3_bind_text(stmt, 5, t.script.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, t.func.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, t.once ? 1 : 0);
            sqlite3_step(stmt);
            t.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.triggers.push_back(t);
            selectedID_ = t.id; selectedType_ = kSelTrigger;
            PushUndo(kUndoCreate, kSelTrigger, t.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed trigger.");
        }
        break;
    }
    case kModeSoundZone: {
        ZSoundZone s;
        s.x = wpos.x; s.z = wpos.z;
        s.radius    = sndRadius_;
        s.soundName = sndNameBuf_;
        s.volume    = sndVolume_;
        s.loopMs    = sndLoopMs_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO area_sound_zones (area_name, x, z, radius, sound_name, volume, loop_interval_ms)"
            " VALUES (?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, s.x); sqlite3_bind_double(stmt, 3, s.z);
            sqlite3_bind_double(stmt, 4, s.radius);
            sqlite3_bind_text(stmt, 5, s.soundName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 6, s.volume); sqlite3_bind_int(stmt, 7, s.loopMs);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.soundZones.push_back(s);
            selectedID_ = s.id; selectedType_ = kSelSoundZone;
            PushUndo(kUndoCreate, kSelSoundZone, s.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed sound zone.");
        }
        break;
    }
    case kModeColBox: {
        ZColBox c;
        c.pos   = wpos;
        c.scale = {cbScaleX_, cbScaleY_, cbScaleZ_};

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_colboxes (area_name, x, y, z, scale_x, scale_y, scale_z)"
            " VALUES (?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, c.pos.x); sqlite3_bind_double(stmt, 3, c.pos.y); sqlite3_bind_double(stmt, 4, c.pos.z);
            sqlite3_bind_double(stmt, 5, c.scale.x); sqlite3_bind_double(stmt, 6, c.scale.y); sqlite3_bind_double(stmt, 7, c.scale.z);
            sqlite3_step(stmt);
            c.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.colBoxes.push_back(c);
            selectedID_ = c.id; selectedType_ = kSelColBox;
            PushUndo(kUndoCreate, kSelColBox, c.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed collision box.");
        }
        break;
    }
    case kModeColSphere: {
        ZColSphere s;
        s.pos    = wpos;
        s.radius = csRadius_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_colspheres (area_name, x, y, z, radius)"
            " VALUES (?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, s.pos.x); sqlite3_bind_double(stmt, 3, s.pos.y); sqlite3_bind_double(stmt, 4, s.pos.z);
            sqlite3_bind_double(stmt, 5, s.radius);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.colSpheres.push_back(s);
            selectedID_ = s.id; selectedType_ = kSelColSphere;
            PushUndo(kUndoCreate, kSelColSphere, s.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed collision sphere.");
        }
        break;
    }
    case kModeWaypoint: {
        ZWaypoint w;
        w.pos = wpos;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_waypoints (area_name, x, y, z) VALUES (?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, w.pos.x); sqlite3_bind_double(stmt, 3, w.pos.y); sqlite3_bind_double(stmt, 4, w.pos.z);
            sqlite3_step(stmt);
            w.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.waypoints.push_back(w);
            selectedID_ = w.id; selectedType_ = kSelWaypoint;
            PushUndo(kUndoCreate, kSelWaypoint, w.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed waypoint #%d.", w.id);
        }
        break;
    }
    case kModeNPC: {
        ZNpcSpawn n;
        n.name    = npcNameBuf_[0] ? npcNameBuf_ : "NPC";
        n.race    = npcRaceBuf_;
        n.class_  = npcClassBuf_;
        n.level   = npcLevel_;
        n.pos     = wpos;
        n.yaw     = 0.f;
        n.aggressiveness = npcAgg_;
        n.aggroRange     = npcAggroRange_;
        n.attackRange    = npcAtkRange_;
        n.respawnDelayMs = npcRespawnMs_;
        n.actorDefId     = npcActorDefId_;
        n.spawnScript = npcSpawnScript_; n.spawnFunc = npcSpawnFunc_;
        n.clickScript = npcClickScript_; n.clickFunc = npcClickFunc_;
        n.deathScript = npcDeathScript_; n.deathFunc = npcDeathFunc_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO npc_spawns"
            " (name, race, class, level, area_name, x, y, z, yaw,"
            "  aggressiveness, aggressive_range, attack_range, respawn_delay_ms, actor_def_id,"
            "  spawn_script, spawn_func, click_script, click_func, death_script, death_func)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,  1, n.name.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,  2, n.race.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,  3, n.class_.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,   4, n.level);
            sqlite3_bind_text(stmt,  5, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 6, n.pos.x); sqlite3_bind_double(stmt, 7, n.pos.y); sqlite3_bind_double(stmt, 8, n.pos.z);
            sqlite3_bind_double(stmt, 9, n.yaw);
            sqlite3_bind_int(stmt,  10, n.aggressiveness);
            sqlite3_bind_double(stmt,11, n.aggroRange); sqlite3_bind_double(stmt,12, n.attackRange);
            sqlite3_bind_int(stmt,  13, n.respawnDelayMs); sqlite3_bind_int(stmt,14, n.actorDefId);
            sqlite3_bind_text(stmt, 15, n.spawnScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 16, n.spawnFunc.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 17, n.clickScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 18, n.clickFunc.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 19, n.deathScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 20, n.deathFunc.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            n.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.npcs.push_back(n);
            selectedID_ = n.id; selectedType_ = kSelNpc;
            PushUndo(kUndoCreate, kSelNpc, n.id);
            // Resync renderer caches so the new NPC's model loads + the
            // Actor instance is created. Without this, the NPC sits in
            // scene_.npcs but renders nothing until the next zone reload
            // or MediaRevision bump.
            if (media) SyncSceneryCache(media);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed NPC '%s'.", n.name.c_str());
        }
        break;
    }
    case kModeSpawnPoint: {
        ZSpawnPoint sp;
        sp.pos    = wpos;
        sp.radius = spawnPtRadius_;
        sp.name   = scene_.areaName + " Spawn";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO spawn_points (name, area_name, x, y, z, radius)"
            " VALUES (?,?,?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, sp.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, sp.pos.x);
            sqlite3_bind_double(stmt, 4, sp.pos.y);
            sqlite3_bind_double(stmt, 5, sp.pos.z);
            sqlite3_bind_double(stmt, 6, sp.radius);
            sqlite3_step(stmt);
            sp.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.spawnPoints.push_back(sp);
            selectedID_   = sp.id;
            selectedType_ = kSelSpawnPoint;
            spawnPtSelMob_ = -1;
            PushUndo(kUndoCreate, kSelSpawnPoint, sp.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                "Placed spawn point #%d at (%.0f, %.0f).", sp.id, wpos.x, wpos.z);
        }
        break;
    }
    case kModeScenery: {
        if (scnModelId_ == 0) {
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Select a model in the asset browser first.");
            break;
        }
        glm::vec3 p = wpos;
        // Grid snap (already applied on LMB path, but apply here too for
        // the RMB context-menu path to be consistent).
        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
            p.x = std::round(p.x / scnGridSize_) * scnGridSize_;
            p.z = std::round(p.z / scnGridSize_) * scnGridSize_;
        }
        // Ground snap: set Y to terrain height so the object sits on the ground.
        if (scnAlignGround_ && renderer_.terrain().Loaded()) {
            p.y = renderer_.terrain().heightmap().SampleWorld(p.x, p.z);
        }
        // Read the model's global scale from media_models — used as the
        // default instance scale so every placement starts at the right size.
        float defaultScale = 1.f;
        {
            sqlite3_stmt* sc = nullptr;
            sqlite3_prepare_v2(db,
                "SELECT scale FROM media_models WHERE id=?", -1, &sc, nullptr);
            sqlite3_bind_int(sc, 1, scnModelId_);
            if (sqlite3_step(sc) == SQLITE_ROW)
                defaultScale = (float)sqlite3_column_double(sc, 0);
            sqlite3_finalize(sc);
            if (defaultScale <= 0.f) defaultScale = 1.f;
        }

        ZScenery s;
        s.modelId    = scnModelId_;
        s.materialId = scnMaterialId_;
        s.pos        = p;
        s.scale      = {defaultScale, defaultScale, defaultScale};
        s.folder     = scnFolder_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_scenery (area_name, model_id, material_id, x, y, z, sx, sy, sz, folder)"
            " VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,2,s.modelId); sqlite3_bind_int(stmt,3,s.materialId);
            sqlite3_bind_double(stmt,4,s.pos.x); sqlite3_bind_double(stmt,5,s.pos.y); sqlite3_bind_double(stmt,6,s.pos.z);
            sqlite3_bind_double(stmt,7,defaultScale); sqlite3_bind_double(stmt,8,defaultScale); sqlite3_bind_double(stmt,9,defaultScale);
            sqlite3_bind_text(stmt,10,s.folder.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.scenery.push_back(s);
            if (media) SyncSceneryCache(media);
            selectedID_ = s.id; selectedType_ = kSelScenery;
            PushUndo(kUndoCreate, kSelScenery, s.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed scenery id=%d.", s.id);
        }
        break;
    }
    case kModeWater: {
        ZWater w;
        w.pos      = wpos;
        w.scale    = {wtrScaleX_, wtrScaleZ_};
        w.color    = wtrColor_;
        w.opacity  = wtrOpacity_;
        w.texPath  = wtrTexPath_;
        w.texScale = wtrTexScale_;
        w.damage   = wtrDamage_;
        w.waveSpeed = wtrWaveSpeed_;
        w.waveDirX  = std::cos(glm::radians(wtrWaveAngleDeg_));
        w.waveDirZ  = std::sin(glm::radians(wtrWaveAngleDeg_));
        w.waveScale = wtrWaveScale_;
        w.shallowColor      = wtrShallowColor_;
        w.deepColor         = wtrDeepColor_;
        w.depthFadeDistance = wtrDepthFadeDistance_;
        w.foamWidth         = wtrFoamWidth_;
        w.foamColor         = wtrFoamColor_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_water (area_name, x, y, z, scale_x, scale_z, color_r, color_g, color_b, opacity, tex_path, tex_scale, damage,"
            " wave_speed, wave_dir_x, wave_dir_z, wave_scale,"
            " shallow_r, shallow_g, shallow_b, deep_r, deep_g, deep_b, depth_fade_distance,"
            " foam_width, foam_r, foam_g, foam_b)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,2,w.pos.x); sqlite3_bind_double(stmt,3,w.pos.y); sqlite3_bind_double(stmt,4,w.pos.z);
            sqlite3_bind_double(stmt,5,w.scale.x); sqlite3_bind_double(stmt,6,w.scale.y);
            sqlite3_bind_int(stmt,7,(int)(w.color.r*255)); sqlite3_bind_int(stmt,8,(int)(w.color.g*255)); sqlite3_bind_int(stmt,9,(int)(w.color.b*255));
            sqlite3_bind_int(stmt,10,w.opacity);
            sqlite3_bind_text(stmt,11,w.texPath.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,12,w.texScale);
            sqlite3_bind_int(stmt,13,w.damage);
            sqlite3_bind_double(stmt,14,w.waveSpeed);
            sqlite3_bind_double(stmt,15,w.waveDirX);
            sqlite3_bind_double(stmt,16,w.waveDirZ);
            sqlite3_bind_double(stmt,17,w.waveScale);
            sqlite3_bind_int(stmt,18,(int)(w.shallowColor.r*255));
            sqlite3_bind_int(stmt,19,(int)(w.shallowColor.g*255));
            sqlite3_bind_int(stmt,20,(int)(w.shallowColor.b*255));
            sqlite3_bind_int(stmt,21,(int)(w.deepColor.r*255));
            sqlite3_bind_int(stmt,22,(int)(w.deepColor.g*255));
            sqlite3_bind_int(stmt,23,(int)(w.deepColor.b*255));
            sqlite3_bind_double(stmt,24,w.depthFadeDistance);
            sqlite3_bind_double(stmt,25,w.foamWidth);
            sqlite3_bind_int(stmt,26,(int)(w.foamColor.r*255));
            sqlite3_bind_int(stmt,27,(int)(w.foamColor.g*255));
            sqlite3_bind_int(stmt,28,(int)(w.foamColor.b*255));
            sqlite3_step(stmt);
            w.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            std::fprintf(stderr,
                "[water-panel] INSERT zone_water id=%d tex_path='%s' (from placement combo wtrTexPath_='%s')\n",
                w.id, w.texPath.c_str(), wtrTexPath_.c_str());
            scene_.water.push_back(w);
            selectedID_ = w.id; selectedType_ = kSelWater;
            PushUndo(kUndoCreate, kSelWater, w.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed water id=%d.", w.id);
        }
        break;
    }
    case kModePlayerSpawn: {
        ZPlayerSpawn ps;
        ps.pos  = wpos;
        ps.yaw  = 0.f;
        ps.name = scene_.areaName + " Spawn";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO player_spawns (name, area_name, x, y, z, yaw)"
            " VALUES (?,?,?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text  (stmt, 1, ps.name.c_str(),         -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 2, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, ps.pos.x);
            sqlite3_bind_double(stmt, 4, ps.pos.y);
            sqlite3_bind_double(stmt, 5, ps.pos.z);
            sqlite3_bind_double(stmt, 6, ps.yaw);
            sqlite3_step(stmt);
            ps.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.playerSpawns.push_back(ps);
            selectedID_   = ps.id;
            selectedType_ = kSelPlayerSpawn;
            PushUndo(kUndoCreate, kSelPlayerSpawn, ps.id);
            media->ReloadPlayerSpawns(db);
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                "Placed player spawn #%d at (%.0f, %.0f).", ps.id, wpos.x, wpos.z);
        }
        break;
    }
    case kModeEmitters: {
        ZEmitter e;
        e.pos        = wpos;
        e.configName = kEmitterNames[emtConfigIdx_];

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_emitters (area_name, config_name, x, y, z) VALUES (?,?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,e.configName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,3,e.pos.x); sqlite3_bind_double(stmt,4,e.pos.y); sqlite3_bind_double(stmt,5,e.pos.z);
            sqlite3_step(stmt);
            e.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.emitters.push_back(e);
            selectedID_ = e.id; selectedType_ = kSelEmitter;
            PushUndo(kUndoCreate, kSelEmitter, e.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed emitter '%s' id=%d.", e.configName.c_str(), e.id);
        }
        break;
    }
    case kModeLight: {
        ZLight l;
        l.pos       = wpos;
        l.name      = lightName_;
        l.color     = {lightColor_[0], lightColor_[1], lightColor_[2]};
        l.intensity = lightIntensity_;
        l.radius    = lightRadius_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_lights (area_name, name, x, y, z, color_r, color_g, color_b, intensity, radius)"
            " VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, l.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, l.pos.x); sqlite3_bind_double(stmt, 4, l.pos.y); sqlite3_bind_double(stmt, 5, l.pos.z);
            sqlite3_bind_double(stmt, 6, l.color.r); sqlite3_bind_double(stmt, 7, l.color.g); sqlite3_bind_double(stmt, 8, l.color.b);
            sqlite3_bind_double(stmt, 9, l.intensity);
            sqlite3_bind_double(stmt, 10, l.radius);
            sqlite3_step(stmt);
            l.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.lights.push_back(l);
            selectedID_ = l.id; selectedType_ = kSelLight;
            PushUndo(kUndoCreate, kSelLight, l.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed light id=%d.", l.id);
        }
        break;
    }
    default: break;
    }
}

// ─── Foliage brush ──────────────────────────────────────────────────────────
// Called every frame the LMB is held in Foliage mode (zones_viewport.cpp).
// Scatters new zone_scenery instances within foliageRadius_ of `hit` (erase=
// false), or removes existing ones whose model is in the current palette
// (erase=true, Shift+LMB). Painted instances are ordinary ZScenery rows —
// same table/renderer path as manually-placed scenery — so no new rendering
// or persistence plumbing was needed, just batched placement.
void ZonesTab::ApplyFoliageBrush(sqlite3* db, MediaTab* media, const glm::vec3& hit,
                                 float dt, bool erase) {
    if (erase) {
        // Three mask modes (foliageEraseMode_): Palette = only scenery whose
        // model is currently checked in the palette; Folder = ANY model
        // whose folder matches foliageFolder_, "/"-prefix included (e.g.
        // folder "Forest" also matches "Forest/Undergrowth"), ignoring the
        // palette entirely; Any = no mask, erase everything in radius. Any
        // mode exists because Palette mode silently erases nothing if the
        // palette happens to be empty or doesn't include the model you're
        // trying to clear — a common source of "erase doesn't work".
        if (foliageEraseMode_ == kFoliageErasePalette && foliageModelIds_.empty()) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                "Foliage erase: palette is empty, nothing to match. "
                "Check a model, or switch Erase mode to Folder/Any.");
            return;
        }
        const std::string eraseFolder = foliageFolder_;
        const std::string eraseFolderPrefix = eraseFolder + "/";
        std::vector<int> toRemove;
        for (auto& s : scene_.scenery) {
            bool matches;
            if (foliageEraseMode_ == kFoliageEraseAny) {
                matches = true;
            } else if (foliageEraseMode_ == kFoliageEraseFolder) {
                matches = eraseFolder.empty()
                    ? s.folder.empty()
                    : (s.folder == eraseFolder || s.folder.rfind(eraseFolderPrefix, 0) == 0);
            } else {
                matches = std::find(foliageModelIds_.begin(), foliageModelIds_.end(), s.modelId)
                          != foliageModelIds_.end();
            }
            if (!matches) continue;
            float dx = s.pos.x - hit.x, dz = s.pos.z - hit.z;
            if (dx * dx + dz * dz <= foliageRadius_ * foliageRadius_)
                toRemove.push_back(s.id);
        }
        if (toRemove.empty()) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                "Foliage erase: no matching scenery in radius (mode=%s).",
                foliageEraseMode_ == kFoliageEraseFolder ? "Folder" :
                foliageEraseMode_ == kFoliageEraseAny    ? "Any"    : "Palette");
            return;
        }

        sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM zone_scenery WHERE id=?", -1, &del, nullptr) == SQLITE_OK) {
            for (int id : toRemove) {
                sqlite3_bind_int(del, 1, id);
                sqlite3_step(del);
                sqlite3_reset(del);
            }
            sqlite3_finalize(del);
        }
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

        scene_.scenery.erase(std::remove_if(scene_.scenery.begin(), scene_.scenery.end(),
            [&](const ZScenery& s) {
                return std::find(toRemove.begin(), toRemove.end(), s.id) != toRemove.end();
            }), scene_.scenery.end());
        if (selectedType_ == kSelScenery &&
            std::find(toRemove.begin(), toRemove.end(), selectedID_) != toRemove.end()) {
            ClearSelection();
        }
        if (media) SyncSceneryCache(media);
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Foliage: erased %d instance(s).", (int)toRemove.size());
        return;
    }

    if (foliageModelIds_.empty() || !renderer_.terrain().Loaded()) return;

    // Frame-rate independent density: accumulate a fractional instance
    // "budget" from area × density × dt and only spend whole units, so a
    // slow frame and a fast frame scatter the same average density instead
    // of one placing more instances just because dt was larger.
    float area = 3.14159265f * foliageRadius_ * foliageRadius_;
    foliagePlaceAccum_ += area * foliageDensity_ * dt;
    int budget = (int)foliagePlaceAccum_;
    if (budget <= 0) return;
    foliagePlaceAccum_ -= (float)budget;
    // Hard cap per tick — a huge radius+density combo shouldn't spike a frame
    // with hundreds of inserts; the brush just needs more strokes/time instead.
    budget = std::min(budget, 12);
    // If any palette model has never appeared in the scene yet (this brush or
    // otherwise), this tick may be the one that pays its one-time load cost —
    // keep it to a single instance (see foliageWarmModelIds_ comment in zones.h).
    for (int mid : foliageModelIds_) {
        bool warm = foliageWarmModelIds_.count(mid) != 0;
        if (!warm) {
            for (auto& s : scene_.scenery) if (s.modelId == mid) { warm = true; break; }
        }
        if (!warm) { budget = std::min(budget, 1); break; }
    }

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO zone_scenery"
        " (area_name, model_id, material_id, x, y, z, pitch, yaw, roll, sx, sy, sz, folder)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &ins, nullptr) != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Foliage: insert prepare failed: %s", sqlite3_errmsg(db));
        return;
    }

    // Model default scale is looked up once per stroke-tick per distinct
    // model actually rolled, not per candidate point.
    std::unordered_map<int, float> defaultScaleCache;
    auto defaultScaleFor = [&](int modelId) -> float {
        auto it = defaultScaleCache.find(modelId);
        if (it != defaultScaleCache.end()) return it->second;
        float s = 1.f;
        sqlite3_stmt* sc = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT scale FROM media_models WHERE id=?", -1, &sc, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sc, 1, modelId);
            if (sqlite3_step(sc) == SQLITE_ROW) s = (float)sqlite3_column_double(sc, 0);
            sqlite3_finalize(sc);
        }
        if (s <= 0.f) s = 1.f;
        defaultScaleCache[modelId] = s;
        return s;
    };

    int placed = 0;
    const int maxAttempts = budget * 8;
    for (int attempt = 0; attempt < maxAttempts && placed < budget; ++attempt) {
        // Uniform random point inside the brush disk.
        float rr  = std::sqrt((float)std::rand() / (float)RAND_MAX) * foliageRadius_;
        float ang = ((float)std::rand() / (float)RAND_MAX) * 6.2831853f;
        glm::vec3 p = hit + glm::vec3(std::cos(ang) * rr, 0.f, std::sin(ang) * rr);

        bool tooClose = false;
        for (auto& s : scene_.scenery) {
            float dx = s.pos.x - p.x, dz = s.pos.z - p.z;
            if (dx * dx + dz * dz < foliageMinSpacing_ * foliageMinSpacing_) { tooClose = true; break; }
        }
        if (tooClose) continue;

        p.y = renderer_.terrain().heightmap().SampleWorld(p.x, p.z);

        int modelId = foliageModelIds_[(size_t)std::rand() % foliageModelIds_.size()];
        float jitter = foliageMinScale_ +
            ((float)std::rand() / (float)RAND_MAX) * (foliageMaxScale_ - foliageMinScale_);
        float finalScale = defaultScaleFor(modelId) * jitter;
        float yaw = foliageRandomYaw_ ? ((float)std::rand() / (float)RAND_MAX) * 360.f : 0.f;

        ZScenery s;
        s.modelId    = modelId;
        s.materialId = 0;
        s.pos        = p;
        s.rot        = {0.f, yaw, 0.f};
        s.scale      = {finalScale, finalScale, finalScale};
        s.folder     = foliageFolder_;

        sqlite3_bind_text(ins, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 2, s.modelId);
        sqlite3_bind_int(ins, 3, s.materialId);
        sqlite3_bind_double(ins, 4, s.pos.x);
        sqlite3_bind_double(ins, 5, s.pos.y);
        sqlite3_bind_double(ins, 6, s.pos.z);
        sqlite3_bind_double(ins, 7, s.rot.x);
        sqlite3_bind_double(ins, 8, s.rot.y);
        sqlite3_bind_double(ins, 9, s.rot.z);
        sqlite3_bind_double(ins, 10, s.scale.x);
        sqlite3_bind_double(ins, 11, s.scale.y);
        sqlite3_bind_double(ins, 12, s.scale.z);
        sqlite3_bind_text(ins, 13, s.folder.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        s.id = (int)sqlite3_last_insert_rowid(db);
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);

        scene_.scenery.push_back(s);
        foliageWarmModelIds_.insert(modelId);
        // Per-instance undo, same as any other placement. A stroke that
        // paints more than kMaxUndo (50) instances will only let you undo
        // the most recent 50 — acceptable for a scatter brush; Shift+LMB
        // erase is the practical way to clean up a bad stroke.
        PushUndo(kUndoCreate, kSelScenery, s.id);
        ++placed;
    }
    sqlite3_finalize(ins);

    if (placed > 0) {
        if (media) SyncSceneryCache(media);
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Foliage: painted %d instance(s).", placed);
    }
}

// ─── Scenery folders ────────────────────────────────────────────────────────
// Folder membership test shared by delete/rename: exact match or nested
// under it ("folder/..."). folder=="" (root/ungrouped) is intentionally not
// supported here — there's nothing to bulk-delete/rename about "no folder".

static bool InSceneryFolder(const std::string& objFolder, const std::string& folder) {
    if (objFolder == folder) return true;
    return objFolder.size() > folder.size() &&
           objFolder.compare(0, folder.size(), folder) == 0 &&
           objFolder[folder.size()] == '/';
}

void ZonesTab::CreateSceneryFolder(sqlite3* db, const std::string& name) {
    if (name.empty()) return;
    if (std::find(scene_.sceneryFolders.begin(), scene_.sceneryFolders.end(), name)
            != scene_.sceneryFolders.end()) {
        return;  // already registered — nothing to do
    }
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO zone_scenery_folders (area_name, name) VALUES (?,?)",
        -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    scene_.sceneryFolders.push_back(name);
    std::sort(scene_.sceneryFolders.begin(), scene_.sceneryFolders.end());
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Created folder '%s'.", name.c_str());
}

void ZonesTab::DeleteSceneryFolder(sqlite3* db, MediaTab* media, const std::string& folder) {
    if (folder.empty()) return;
    std::vector<int> toRemove;
    for (auto& s : scene_.scenery)
        if (InSceneryFolder(s.folder, folder)) toRemove.push_back(s.id);

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    if (!toRemove.empty()) {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM zone_scenery WHERE id=?", -1, &del, nullptr) == SQLITE_OK) {
            for (int id : toRemove) {
                sqlite3_bind_int(del, 1, id);
                sqlite3_step(del);
                sqlite3_reset(del);
            }
            sqlite3_finalize(del);
        }
    }
    // Purge the folder (and any nested sub-folders) from the registry too —
    // otherwise a now-empty folder would keep reappearing in the sidebar.
    sqlite3_stmt* delReg = nullptr;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM zone_scenery_folders WHERE area_name=? AND (name=? OR name LIKE ?)",
        -1, &delReg, nullptr) == SQLITE_OK) {
        std::string likePattern = folder + "/%";
        sqlite3_bind_text(delReg, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(delReg, 2, folder.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(delReg, 3, likePattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(delReg);
        sqlite3_finalize(delReg);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    scene_.scenery.erase(std::remove_if(scene_.scenery.begin(), scene_.scenery.end(),
        [&](const ZScenery& s) {
            return std::find(toRemove.begin(), toRemove.end(), s.id) != toRemove.end();
        }), scene_.scenery.end());
    scene_.sceneryFolders.erase(std::remove_if(scene_.sceneryFolders.begin(), scene_.sceneryFolders.end(),
        [&](const std::string& f) { return InSceneryFolder(f, folder); }), scene_.sceneryFolders.end());
    if (selectedType_ == kSelScenery &&
        std::find(toRemove.begin(), toRemove.end(), selectedID_) != toRemove.end()) {
        ClearSelection();
    }
    if (media) SyncSceneryCache(media);
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Deleted folder '%s' (%d instance(s)).", folder.c_str(), (int)toRemove.size());
}

void ZonesTab::RenameSceneryFolder(sqlite3* db, MediaTab* media,
                                   const std::string& folder, const std::string& newFolder) {
    if (folder.empty() || folder == newFolder) return;

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* upd = nullptr;
    bool prepared = sqlite3_prepare_v2(db, "UPDATE zone_scenery SET folder=? WHERE id=?",
                                       -1, &upd, nullptr) == SQLITE_OK;
    int renamed = 0;
    for (auto& s : scene_.scenery) {
        if (!InSceneryFolder(s.folder, folder)) continue;
        // Preserve the nested suffix: "Forest/Undergrowth" renaming
        // "Forest"->"Woods" becomes "Woods/Undergrowth"; an exact match just
        // becomes newFolder outright.
        std::string updated = (s.folder == folder)
            ? newFolder
            : newFolder + s.folder.substr(folder.size());
        s.folder = updated;
        if (prepared) {
            sqlite3_bind_text(upd, 1, updated.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upd, 2, s.id);
            sqlite3_step(upd);
            sqlite3_reset(upd);
            sqlite3_clear_bindings(upd);
        }
        ++renamed;
    }
    if (prepared) sqlite3_finalize(upd);

    // Rename matching registry rows too (covers folders with 0 scenery).
    sqlite3_stmt* updReg = nullptr;
    bool preparedReg = sqlite3_prepare_v2(db,
        "UPDATE OR IGNORE zone_scenery_folders SET name=? WHERE area_name=? AND name=?",
        -1, &updReg, nullptr) == SQLITE_OK;
    for (auto& f : scene_.sceneryFolders) {
        if (!InSceneryFolder(f, folder)) continue;
        std::string updated = (f == folder) ? newFolder : newFolder + f.substr(folder.size());
        if (preparedReg) {
            sqlite3_bind_text(updReg, 1, updated.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(updReg, 2, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(updReg, 3, f.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(updReg);
            sqlite3_reset(updReg);
            sqlite3_clear_bindings(updReg);
        }
        f = updated;
    }
    if (preparedReg) sqlite3_finalize(updReg);
    std::sort(scene_.sceneryFolders.begin(), scene_.sceneryFolders.end());
    scene_.sceneryFolders.erase(std::unique(scene_.sceneryFolders.begin(), scene_.sceneryFolders.end()),
                                scene_.sceneryFolders.end());

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    if (renamed > 0) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Renamed folder '%s' -> '%s' (%d instance(s)).",
                      folder.c_str(), newFolder.c_str(), renamed);
    } else {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Renamed folder '%s' -> '%s'.", folder.c_str(), newFolder.c_str());
    }
    (void)media;  // rename doesn't change model bindings; no re-sync needed
}

void ZonesTab::MoveSceneryToFolder(sqlite3* db, const std::vector<int>& ids, const std::string& newFolder) {
    if (ids.empty()) return;
    // Registering the destination means it survives even if every instance
    // in it later gets moved out or deleted individually.
    if (!newFolder.empty()) CreateSceneryFolder(db, newFolder);

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* upd = nullptr;
    bool prepared = sqlite3_prepare_v2(db, "UPDATE zone_scenery SET folder=? WHERE id=?",
                                       -1, &upd, nullptr) == SQLITE_OK;
    int moved = 0;
    for (int id : ids) {
        for (auto& s : scene_.scenery) {
            if (s.id != id) continue;
            s.folder = newFolder;
            if (prepared) {
                sqlite3_bind_text(upd, 1, newFolder.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(upd, 2, id);
                sqlite3_step(upd);
                sqlite3_reset(upd);
                sqlite3_clear_bindings(upd);
            }
            ++moved;
            break;
        }
    }
    if (prepared) sqlite3_finalize(upd);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    if (moved > 0) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Moved %d instance(s) to folder '%s'.",
                      moved, newFolder.empty() ? "(root)" : newFolder.c_str());
    }
}

std::vector<std::string> ZonesTab::DistinctSceneryFolders() const {
    std::vector<std::string> out = scene_.sceneryFolders;
    for (auto& s : scene_.scenery) {
        if (s.folder.empty()) continue;
        if (std::find(out.begin(), out.end(), s.folder) == out.end())
            out.push_back(s.folder);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ─── Undo ─────────────────────────────────────────────────────────────────────

void ZonesTab::PushUndo(UndoAction action, int type, int id, glm::vec3 prevVec) {
    UndoEntry e;
    e.action     = action;
    e.objectType = type;
    e.objectId   = id;
    e.prevVec    = prevVec;
    undoStack_.push_back(e);
    if ((int)undoStack_.size() > kMaxUndo)
        undoStack_.erase(undoStack_.begin());
}

void ZonesTab::Undo(sqlite3* db) {
    if (undoStack_.empty()) return;
    UndoEntry e = undoStack_.back();
    undoStack_.pop_back();

    switch (e.action) {
    case kUndoCreate: {
        // Undo a creation = delete the object
        if (selectedID_ == e.objectId && selectedType_ == e.objectType) {
            ClearSelection();
        }
        const char* table = nullptr;
        switch (e.objectType) {
        case kSelPortal:    table = "area_portals";     break;
        case kSelTrigger:   table = "area_triggers";    break;
        case kSelSoundZone: table = "area_sound_zones"; break;
        case kSelColBox:    table = "zone_colboxes";    break;
        case kSelColSphere: table = "zone_colspheres";  break;
        case kSelWaypoint:  table = "zone_waypoints";   break;
        case kSelNpc:       table = "npc_spawns";       break;
        case kSelEmitter:   table = "zone_emitters";    break;
        case kSelLight:     table = "zone_lights";      break;
        case kSelWater:       table = "zone_water";       break;
        case kSelScenery:     table = "zone_scenery";     break;
        case kSelPlayerSpawn: table = "player_spawns";    break;
        default: return;
        }
        char sql[128];
        std::snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id=?", table);
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, e.objectId);
            sqlite3_step(s); sqlite3_finalize(s);
        }
        auto rem = [&](auto& vec) {
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [&](auto& x){ return x.id == e.objectId; }), vec.end());
        };
        switch (e.objectType) {
        case kSelPortal:    rem(scene_.portals);    break;
        case kSelTrigger:   rem(scene_.triggers);   break;
        case kSelSoundZone: rem(scene_.soundZones); break;
        case kSelColBox:    rem(scene_.colBoxes);    break;
        case kSelColSphere: rem(scene_.colSpheres); break;
        case kSelWaypoint:  rem(scene_.waypoints);  break;
        case kSelNpc:         rem(scene_.npcs);         break;
        case kSelEmitter:     rem(scene_.emitters);     break;
        case kSelLight:       rem(scene_.lights);       break;
        case kSelWater:       rem(scene_.water);        break;
        case kSelScenery:     rem(scene_.scenery);      break;
        case kSelPlayerSpawn: rem(scene_.playerSpawns); needsSpawnReload_ = true; break;
        }
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Undo: removed id=%d.", e.objectId);
        break;
    }
    case kUndoMove: {
        // Restore previous position
        auto restorePos = [&](auto& vec, const char* table) {
            for (auto& obj : vec) {
                if (obj.id == e.objectId) {
                    obj.pos = e.prevVec;
                    sqlite3_stmt* s = nullptr;
                    char sql[128];
                    std::snprintf(sql, sizeof(sql), "UPDATE %s SET x=?,y=?,z=? WHERE id=?", table);
                    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK) {
                        sqlite3_bind_double(s,1,obj.pos.x); sqlite3_bind_double(s,2,obj.pos.y); sqlite3_bind_double(s,3,obj.pos.z);
                        sqlite3_bind_int(s,4,obj.id);
                        sqlite3_step(s); sqlite3_finalize(s);
                    }
                    return;
                }
            }
        };
        switch (e.objectType) {
        case kSelPortal:    restorePos(scene_.portals,    "area_portals");     break;
        case kSelTrigger: {
            for (auto& t : scene_.triggers) if (t.id == e.objectId) {
                t.x = e.prevVec.x; t.z = e.prevVec.z;
                sqlite3_stmt* s = nullptr;
                if (sqlite3_prepare_v2(db,"UPDATE area_triggers SET x=?,z=? WHERE id=?",-1,&s,nullptr)==SQLITE_OK) {
                    sqlite3_bind_double(s,1,t.x); sqlite3_bind_double(s,2,t.z); sqlite3_bind_int(s,3,t.id);
                    sqlite3_step(s); sqlite3_finalize(s);
                }
                break;
            }
            break;
        }
        case kSelColBox:    restorePos(scene_.colBoxes,   "zone_colboxes");    break;
        case kSelColSphere: restorePos(scene_.colSpheres, "zone_colspheres");  break;
        case kSelWaypoint:  restorePos(scene_.waypoints,  "zone_waypoints");   break;
        case kSelNpc:       restorePos(scene_.npcs,       "npc_spawns");       break;
        case kSelEmitter:   restorePos(scene_.emitters,   "zone_emitters");    break;
        case kSelLight:     restorePos(scene_.lights,     "zone_lights");      break;
        case kSelWater:     restorePos(scene_.water,      "zone_water");       break;
        case kSelScenery:   restorePos(scene_.scenery,    "zone_scenery");     break;
        }
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Undo: moved back id=%d.", e.objectId);
        break;
    }
    case kUndoRotate: {
        // Restore prior Euler rotation (only scenery carries full rot — NPCs
        // have yaw, others have nothing to rotate).
        if (e.objectType == kSelScenery) {
            for (auto& s : scene_.scenery) if (s.id == e.objectId) {
                s.rot = e.prevVec;
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                    "UPDATE zone_scenery SET pitch=?,yaw=?,roll=? WHERE id=?",
                    -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_double(st, 1, s.rot.x);
                    sqlite3_bind_double(st, 2, s.rot.y);
                    sqlite3_bind_double(st, 3, s.rot.z);
                    sqlite3_bind_int   (st, 4, s.id);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
                break;
            }
        } else if (e.objectType == kSelNpc) {
            for (auto& n : scene_.npcs) if (n.id == e.objectId) {
                n.yaw = e.prevVec.y;
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db, "UPDATE npc_spawns SET yaw=? WHERE id=?",
                    -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_double(st, 1, n.yaw);
                    sqlite3_bind_int   (st, 2, n.id);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
                break;
            }
        }
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Undo: rotated back id=%d.", e.objectId);
        break;
    }
    case kUndoScale: {
        if (e.objectType == kSelScenery) {
            for (auto& s : scene_.scenery) if (s.id == e.objectId) {
                s.scale = e.prevVec;
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                    "UPDATE zone_scenery SET sx=?,sy=?,sz=? WHERE id=?",
                    -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_double(st, 1, s.scale.x);
                    sqlite3_bind_double(st, 2, s.scale.y);
                    sqlite3_bind_double(st, 3, s.scale.z);
                    sqlite3_bind_int   (st, 4, s.id);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
                break;
            }
        } else if (e.objectType == kSelColBox) {
            for (auto& c : scene_.colBoxes) if (c.id == e.objectId) {
                c.scale = e.prevVec;
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                    "UPDATE zone_colboxes SET scale_x=?,scale_y=?,scale_z=? WHERE id=?",
                    -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_double(st, 1, c.scale.x);
                    sqlite3_bind_double(st, 2, c.scale.y);
                    sqlite3_bind_double(st, 3, c.scale.z);
                    sqlite3_bind_int   (st, 4, c.id);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
                break;
            }
        } else if (e.objectType == kSelWater) {
            for (auto& w : scene_.water) if (w.id == e.objectId) {
                w.scale.x = e.prevVec.x; w.scale.y = e.prevVec.z;
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                    "UPDATE zone_water SET scale_x=?,scale_z=? WHERE id=?",
                    -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_double(st, 1, w.scale.x);
                    sqlite3_bind_double(st, 2, w.scale.y);
                    sqlite3_bind_int   (st, 3, w.id);
                    sqlite3_step(st); sqlite3_finalize(st);
                }
                break;
            }
        }
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Undo: scaled back id=%d.", e.objectId);
        break;
    }
    default: break;
    }
}

// ─── DuplicateSelected ────────────────────────────────────────────────────────

void ZonesTab::DuplicateSelected(sqlite3* db, MediaTab* media) {
    const auto refs = ActiveSelection();
    if (refs.size() > 1) {
        std::vector<SelectionRef> duplicatedRefs;
        ClearSelection();
        for (const auto& ref : refs) {
            if (ref.id < 0 || ref.type == kSelNone) continue;
            selectedID_ = ref.id;
            selectedType_ = ref.type;
            const int beforeID = selectedID_;
            const int beforeType = selectedType_;
            DuplicateSelected(db, media);
            if (selectedID_ >= 0 && selectedType_ != kSelNone &&
                !(selectedID_ == beforeID && selectedType_ == beforeType)) {
                duplicatedRefs.push_back({selectedType_, selectedID_});
            }
        }
        if (!duplicatedRefs.empty()) {
            selectedRefs_ = std::move(duplicatedRefs);
            selectedType_ = selectedRefs_.back().type;
            selectedID_ = selectedRefs_.back().id;
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "Duplicated %d object(s).", (int)selectedRefs_.size());
        }
        return;
    }

    if (selectedID_ < 0) return;
    // Re-place the current object at pos + small offset
    // For scenery, portal, trigger etc. — just re-call PlaceObject with same settings
    // but offset by (2,0,0)
    glm::vec3 offset = {2.f, 0.f, 0.f};

    switch (selectedType_) {
    case kSelPortal: {
        auto it = std::find_if(scene_.portals.begin(), scene_.portals.end(),
                               [&](auto& p){ return p.id == selectedID_; });
        if (it == scene_.portals.end()) return;
        ZPortal p = *it; p.id = 0; p.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO area_portals (area_name,x,z,radius,target_area,dest_x,dest_y,dest_z,dest_yaw)"
            " VALUES (?,?,?,?,?,0,0,0,0)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,2,p.pos.x); sqlite3_bind_double(stmt,3,p.pos.z);
            sqlite3_bind_double(stmt,4,p.radius); sqlite3_bind_text(stmt,5,p.linkArea.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            p.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.portals.push_back(p);
            selectedID_ = p.id;
            PushUndo(kUndoCreate, kSelPortal, p.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated portal id=%d.", p.id);
        }
        break;
    }
    case kSelTrigger: {
        auto it = std::find_if(scene_.triggers.begin(), scene_.triggers.end(),
                               [&](auto& t){ return t.id == selectedID_; });
        if (it == scene_.triggers.end()) return;
        ZTrigger t = *it;
        t.id = 0;
        t.x += offset.x;
        t.z += offset.z;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO area_triggers (area_name, x, z, radius, script, func, trigger_once)"
            " VALUES (?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, t.x); sqlite3_bind_double(stmt, 3, t.z);
            sqlite3_bind_double(stmt, 4, t.radius);
            sqlite3_bind_text(stmt, 5, t.script.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, t.func.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, t.once ? 1 : 0);
            sqlite3_step(stmt);
            t.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.triggers.push_back(t);
            selectedID_ = t.id;
            PushUndo(kUndoCreate, kSelTrigger, t.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated trigger id=%d.", t.id);
        }
        break;
    }
    case kSelSoundZone: {
        auto it = std::find_if(scene_.soundZones.begin(), scene_.soundZones.end(),
                               [&](auto& s){ return s.id == selectedID_; });
        if (it == scene_.soundZones.end()) return;
        ZSoundZone s = *it;
        s.id = 0;
        s.x += offset.x;
        s.z += offset.z;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO area_sound_zones (area_name, x, z, radius, sound_name, volume, loop_interval_ms)"
            " VALUES (?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, s.x); sqlite3_bind_double(stmt, 3, s.z);
            sqlite3_bind_double(stmt, 4, s.radius);
            sqlite3_bind_text(stmt, 5, s.soundName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 6, s.volume); sqlite3_bind_int(stmt, 7, s.loopMs);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.soundZones.push_back(s);
            selectedID_ = s.id;
            PushUndo(kUndoCreate, kSelSoundZone, s.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated sound zone id=%d.", s.id);
        }
        break;
    }
    case kSelColBox: {
        auto it = std::find_if(scene_.colBoxes.begin(), scene_.colBoxes.end(),
                               [&](auto& c){ return c.id == selectedID_; });
        if (it == scene_.colBoxes.end()) return;
        ZColBox c = *it;
        c.id = 0;
        c.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_colboxes (area_name, x, y, z, scale_x, scale_y, scale_z)"
            " VALUES (?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, c.pos.x); sqlite3_bind_double(stmt, 3, c.pos.y); sqlite3_bind_double(stmt, 4, c.pos.z);
            sqlite3_bind_double(stmt, 5, c.scale.x); sqlite3_bind_double(stmt, 6, c.scale.y); sqlite3_bind_double(stmt, 7, c.scale.z);
            sqlite3_step(stmt);
            c.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.colBoxes.push_back(c);
            selectedID_ = c.id;
            PushUndo(kUndoCreate, kSelColBox, c.id);
            scene_.colVisDirty = true;
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated collision box id=%d.", c.id);
        }
        break;
    }
    case kSelColSphere: {
        auto it = std::find_if(scene_.colSpheres.begin(), scene_.colSpheres.end(),
                               [&](auto& s){ return s.id == selectedID_; });
        if (it == scene_.colSpheres.end()) return;
        ZColSphere s = *it;
        s.id = 0;
        s.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_colspheres (area_name, x, y, z, radius)"
            " VALUES (?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, s.pos.x); sqlite3_bind_double(stmt, 3, s.pos.y); sqlite3_bind_double(stmt, 4, s.pos.z);
            sqlite3_bind_double(stmt, 5, s.radius);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.colSpheres.push_back(s);
            selectedID_ = s.id;
            PushUndo(kUndoCreate, kSelColSphere, s.id);
            scene_.colVisDirty = true;
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated collision sphere id=%d.", s.id);
        }
        break;
    }
    case kSelWaypoint: {
        auto it = std::find_if(scene_.waypoints.begin(), scene_.waypoints.end(),
                               [&](auto& w){ return w.id == selectedID_; });
        if (it == scene_.waypoints.end()) return;
        ZWaypoint w = *it;
        w.id = 0;
        w.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_waypoints (area_name, x, y, z, next_a, next_b, pause_sec, spawn_actor_id,"
            " spawn_script, spawn_func, click_script, click_func, death_script, death_func,"
            " spawn_delay_sec, spawn_max, spawn_range)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, w.pos.x); sqlite3_bind_double(stmt, 3, w.pos.y); sqlite3_bind_double(stmt, 4, w.pos.z);
            sqlite3_bind_int(stmt, 5, w.nextA); sqlite3_bind_int(stmt, 6, w.nextB);
            sqlite3_bind_int(stmt, 7, w.pauseSec); sqlite3_bind_int(stmt, 8, w.spawnActorId);
            sqlite3_bind_text(stmt, 9, w.spawnScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,10, w.spawnFunc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,11, w.clickScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,12, w.clickFunc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,13, w.deathScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,14, w.deathFunc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,15, w.spawnDelaySec); sqlite3_bind_int(stmt,16, w.spawnMax);
            sqlite3_bind_double(stmt,17, w.spawnRange);
            sqlite3_step(stmt);
            w.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.waypoints.push_back(w);
            selectedID_ = w.id;
            PushUndo(kUndoCreate, kSelWaypoint, w.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated waypoint id=%d.", w.id);
        }
        break;
    }
    case kSelNpc: {
        auto it = std::find_if(scene_.npcs.begin(), scene_.npcs.end(),
                               [&](auto& n){ return n.id == selectedID_; });
        if (it == scene_.npcs.end()) return;
        ZNpcSpawn n = *it;
        n.id = 0;
        n.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO npc_spawns"
            " (name, race, class, level, area_name, x, y, z, yaw,"
            "  aggressiveness, aggressive_range, attack_range, respawn_delay_ms, actor_def_id,"
            "  spawn_script, spawn_func, click_script, click_func, death_script, death_func)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,  1, n.name.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,  2, n.race.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,  3, n.class_.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,   4, n.level);
            sqlite3_bind_text(stmt,  5, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 6, n.pos.x); sqlite3_bind_double(stmt, 7, n.pos.y); sqlite3_bind_double(stmt, 8, n.pos.z);
            sqlite3_bind_double(stmt, 9, n.yaw);
            sqlite3_bind_int(stmt,  10, n.aggressiveness);
            sqlite3_bind_double(stmt,11, n.aggroRange); sqlite3_bind_double(stmt,12, n.attackRange);
            sqlite3_bind_int(stmt,  13, n.respawnDelayMs); sqlite3_bind_int(stmt,14, n.actorDefId);
            sqlite3_bind_text(stmt, 15, n.spawnScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 16, n.spawnFunc.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 17, n.clickScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 18, n.clickFunc.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 19, n.deathScript.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 20, n.deathFunc.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            n.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.npcs.push_back(n);
            selectedID_ = n.id;
            PushUndo(kUndoCreate, kSelNpc, n.id);
            if (media) SyncSceneryCache(media);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated NPC id=%d.", n.id);
        }
        break;
    }
    case kSelEmitter: {
        auto it = std::find_if(scene_.emitters.begin(), scene_.emitters.end(),
                               [&](auto& e){ return e.id == selectedID_; });
        if (it == scene_.emitters.end()) return;
        ZEmitter e = *it;
        e.id = 0;
        e.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_emitters (area_name, config_name, x, y, z)"
            " VALUES (?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, e.configName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, e.pos.x); sqlite3_bind_double(stmt, 4, e.pos.y); sqlite3_bind_double(stmt, 5, e.pos.z);
            sqlite3_step(stmt);
            e.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.emitters.push_back(e);
            selectedID_ = e.id;
            PushUndo(kUndoCreate, kSelEmitter, e.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated emitter id=%d.", e.id);
        }
        break;
    }
    case kSelLight: {
        auto it = std::find_if(scene_.lights.begin(), scene_.lights.end(),
                               [&](auto& l){ return l.id == selectedID_; });
        if (it == scene_.lights.end()) return;
        ZLight l = *it;
        l.id = 0;
        l.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_lights (area_name, name, x, y, z, color_r, color_g, color_b, intensity, radius)"
            " VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, l.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, l.pos.x); sqlite3_bind_double(stmt, 4, l.pos.y); sqlite3_bind_double(stmt, 5, l.pos.z);
            sqlite3_bind_double(stmt, 6, l.color.r); sqlite3_bind_double(stmt, 7, l.color.g); sqlite3_bind_double(stmt, 8, l.color.b);
            sqlite3_bind_double(stmt, 9, l.intensity);
            sqlite3_bind_double(stmt, 10, l.radius);
            sqlite3_step(stmt);
            l.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.lights.push_back(l);
            selectedID_ = l.id;
            PushUndo(kUndoCreate, kSelLight, l.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated light id=%d.", l.id);
        }
        break;
    }
    case kSelWater: {
        auto it = std::find_if(scene_.water.begin(), scene_.water.end(),
                               [&](auto& w){ return w.id == selectedID_; });
        if (it == scene_.water.end()) return;
        ZWater w = *it;
        w.id = 0;
        w.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_water (area_name, x, y, z, scale_x, scale_z, color_r, color_g, color_b, opacity, tex_path, tex_scale, damage, dmg_type,"
            " wave_speed, wave_dir_x, wave_dir_z, wave_scale,"
            " shallow_r, shallow_g, shallow_b, deep_r, deep_g, deep_b, depth_fade_distance,"
            " foam_width, foam_r, foam_g, foam_b)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            auto to255 = [](float v) -> int {
                int iv = (int)(v * 255.f);
                if (iv < 0) iv = 0;
                if (iv > 255) iv = 255;
                return iv;
            };
            sqlite3_bind_text(stmt, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, w.pos.x); sqlite3_bind_double(stmt, 3, w.pos.y); sqlite3_bind_double(stmt, 4, w.pos.z);
            sqlite3_bind_double(stmt, 5, w.scale.x); sqlite3_bind_double(stmt, 6, w.scale.y);
            sqlite3_bind_int(stmt, 7, to255(w.color.r)); sqlite3_bind_int(stmt, 8, to255(w.color.g)); sqlite3_bind_int(stmt, 9, to255(w.color.b));
            sqlite3_bind_int(stmt,10, w.opacity);
            sqlite3_bind_text(stmt,11, w.texPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,12, w.texScale);
            sqlite3_bind_int(stmt,13, w.damage); sqlite3_bind_int(stmt,14, w.dmgType);
            sqlite3_bind_double(stmt,15, w.waveSpeed);
            sqlite3_bind_double(stmt,16, w.waveDirX);
            sqlite3_bind_double(stmt,17, w.waveDirZ);
            sqlite3_bind_double(stmt,18, w.waveScale);
            sqlite3_bind_int(stmt,19, to255(w.shallowColor.r));
            sqlite3_bind_int(stmt,20, to255(w.shallowColor.g));
            sqlite3_bind_int(stmt,21, to255(w.shallowColor.b));
            sqlite3_bind_int(stmt,22, to255(w.deepColor.r));
            sqlite3_bind_int(stmt,23, to255(w.deepColor.g));
            sqlite3_bind_int(stmt,24, to255(w.deepColor.b));
            sqlite3_bind_double(stmt,25, w.depthFadeDistance);
            sqlite3_bind_double(stmt,26, w.foamWidth);
            sqlite3_bind_int(stmt,27, to255(w.foamColor.r));
            sqlite3_bind_int(stmt,28, to255(w.foamColor.g));
            sqlite3_bind_int(stmt,29, to255(w.foamColor.b));
            sqlite3_step(stmt);
            w.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.water.push_back(w);
            selectedID_ = w.id;
            PushUndo(kUndoCreate, kSelWater, w.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated water id=%d.", w.id);
        }
        break;
    }
    case kSelSpawnPoint: {
        auto it = std::find_if(scene_.spawnPoints.begin(), scene_.spawnPoints.end(),
                               [&](auto& sp){ return sp.id == selectedID_; });
        if (it == scene_.spawnPoints.end()) return;
        ZSpawnPoint sp = *it;
        sp.id = 0;
        sp.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO spawn_points (name, area_name, x, y, z, radius)"
            " VALUES (?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, sp.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, sp.pos.x);
            sqlite3_bind_double(stmt, 4, sp.pos.y);
            sqlite3_bind_double(stmt, 5, sp.pos.z);
            sqlite3_bind_double(stmt, 6, sp.radius);
            sqlite3_step(stmt);
            sp.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);

            sqlite3_stmt* mobStmt = nullptr;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO spawn_point_mobs (spawn_point_id, actor_def_id, mob_count, name, race, class, level, aggressiveness, aggressive_range, attack_range, respawn_delay_ms)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?)", -1, &mobStmt, nullptr) == SQLITE_OK) {
                for (auto& m : sp.mobs) {
                    sqlite3_bind_int(mobStmt, 1, sp.id);
                    sqlite3_bind_int(mobStmt, 2, m.actor_def_id);
                    sqlite3_bind_int(mobStmt, 3, m.count);
                    sqlite3_bind_text(mobStmt, 4, m.name.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(mobStmt, 5, m.race.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(mobStmt, 6, m.class_.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(mobStmt, 7, m.level);
                    sqlite3_bind_int(mobStmt, 8, m.aggressiveness);
                    sqlite3_bind_double(mobStmt, 9, m.aggressive_range);
                    sqlite3_bind_double(mobStmt,10, m.attack_range);
                    sqlite3_bind_int(mobStmt,11, m.respawn_delay_ms);
                    sqlite3_step(mobStmt);
                    m.id = (int)sqlite3_last_insert_rowid(db);
                    sqlite3_reset(mobStmt);
                    sqlite3_clear_bindings(mobStmt);
                }
                sqlite3_finalize(mobStmt);
            }

            scene_.spawnPoints.push_back(sp);
            selectedID_ = sp.id;
            selectedType_ = kSelSpawnPoint;
            spawnPtSelMob_ = -1;
            PushUndo(kUndoCreate, kSelSpawnPoint, sp.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated spawn point id=%d.", sp.id);
        }
        break;
    }
    case kSelScenery: {
        auto it = std::find_if(scene_.scenery.begin(), scene_.scenery.end(),
                               [&](auto& s){ return s.id == selectedID_; });
        if (it == scene_.scenery.end()) return;
        ZScenery s = *it;
        s.id = 0; s.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_scenery (area_name, model_id, material_id, x, y, z, pitch, yaw, roll, sx, sy, sz, collision, anim_mode, inv_size, ownable, locked, folder)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,2,s.modelId); sqlite3_bind_int(stmt,3,s.materialId);
            sqlite3_bind_double(stmt,4,s.pos.x); sqlite3_bind_double(stmt,5,s.pos.y); sqlite3_bind_double(stmt,6,s.pos.z);
            sqlite3_bind_double(stmt,7,s.rot.x); sqlite3_bind_double(stmt,8,s.rot.y); sqlite3_bind_double(stmt,9,s.rot.z);
            sqlite3_bind_double(stmt,10,s.scale.x); sqlite3_bind_double(stmt,11,s.scale.y); sqlite3_bind_double(stmt,12,s.scale.z);
            sqlite3_bind_int(stmt,13,s.collision); sqlite3_bind_int(stmt,14,s.animMode);
            sqlite3_bind_int(stmt,15,s.invSize); sqlite3_bind_int(stmt,16,s.ownable?1:0); sqlite3_bind_int(stmt,17,s.locked?1:0);
            sqlite3_bind_text(stmt,18,s.folder.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.scenery.push_back(s);
            selectedID_ = s.id;
            PushUndo(kUndoCreate, kSelScenery, s.id);
            scene_.colVisDirty = true;
            if (media) SyncSceneryCache(media);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated scenery id=%d.", s.id);
        }
        break;
    }
    case kSelPlayerSpawn: {
        auto it = std::find_if(scene_.playerSpawns.begin(), scene_.playerSpawns.end(),
                               [&](auto& p){ return p.id == selectedID_; });
        if (it == scene_.playerSpawns.end()) return;
        ZPlayerSpawn ps = *it;
        ps.id   = 0;
        ps.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO player_spawns (name, area_name, x, y, z, yaw)"
            " VALUES (?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text  (stmt, 1, ps.name.c_str(),         -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (stmt, 2, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, ps.pos.x);
            sqlite3_bind_double(stmt, 4, ps.pos.y);
            sqlite3_bind_double(stmt, 5, ps.pos.z);
            sqlite3_bind_double(stmt, 6, ps.yaw);
            sqlite3_step(stmt);
            ps.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.playerSpawns.push_back(ps);
            selectedID_   = ps.id;
            selectedType_ = kSelPlayerSpawn;
            PushUndo(kUndoCreate, kSelPlayerSpawn, ps.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated player spawn id=%d.", ps.id);
        }
        break;
    }
    default:
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicate not supported for this object type.");
        break;
    }

    if (selectedID_ >= 0 && selectedType_ != kSelNone) {
        SelectSingle(selectedType_, selectedID_);
    }
}

// ─── DeleteSelected ───────────────────────────────────────────────────────────

void ZonesTab::DeleteSelected(sqlite3* db) {
    const auto refs = ActiveSelection();
    if (refs.size() > 1) {
        int deleted = 0;
        ClearSelection();
        for (const auto& ref : refs) {
            if (ref.id < 0 || ref.type == kSelNone) continue;
            selectedID_ = ref.id;
            selectedType_ = ref.type;
            DeleteSelected(db);
            ++deleted;
        }
        if (deleted > 0) {
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "Deleted %d object(s).", deleted);
        }
        return;
    }

    if (selectedID_ < 0) return;
    const char* table = nullptr;
    switch (selectedType_) {
    case kSelPortal:    table = "area_portals";     break;
    case kSelTrigger:   table = "area_triggers";    break;
    case kSelSoundZone: table = "area_sound_zones"; break;
    case kSelColBox:    table = "zone_colboxes";    break;
    case kSelColSphere: table = "zone_colspheres";  break;
    case kSelWaypoint:  table = "zone_waypoints";   break;
    case kSelNpc:       table = "npc_spawns";       break;
    case kSelEmitter:   table = "zone_emitters";    break;
    case kSelLight:     table = "zone_lights";      break;
    case kSelSpawnPoint: {
        // Cascade-delete mobs first, then the point itself.
        char sql[128];
        std::snprintf(sql, sizeof(sql),
            "DELETE FROM spawn_point_mobs WHERE spawn_point_id=%d", selectedID_);
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        std::snprintf(sql, sizeof(sql),
            "DELETE FROM spawn_points WHERE id=%d", selectedID_);
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        scene_.spawnPoints.erase(std::remove_if(scene_.spawnPoints.begin(),
            scene_.spawnPoints.end(), [&](auto& x){ return x.id == selectedID_; }),
            scene_.spawnPoints.end());
        std::snprintf(statusMsg_, sizeof(statusMsg_),
            "Deleted spawn point %d.", selectedID_);
        ClearSelection();
        spawnPtSelMob_ = -1;
        return;
    }
    case kSelWater:     table = "zone_water";       break;
    case kSelScenery:   table = "zone_scenery";     break;
    case kSelPlayerSpawn: {
        char sql[128];
        std::snprintf(sql, sizeof(sql),
            "DELETE FROM player_spawns WHERE id=%d", selectedID_);
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        scene_.playerSpawns.erase(
            std::remove_if(scene_.playerSpawns.begin(), scene_.playerSpawns.end(),
                           [&](auto& x){ return x.id == selectedID_; }),
            scene_.playerSpawns.end());
        needsSpawnReload_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
            "Deleted player spawn %d.", selectedID_);
        ClearSelection();
        return;
    }
    default: return;
    }
    char sql[128];
    std::snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id=?", table);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, selectedID_);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Remove from in-memory scene
    auto del = [&](auto& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&](auto& x){ return x.id == selectedID_; }), vec.end());
    };
    switch (selectedType_) {
    case kSelPortal:    del(scene_.portals);    break;
    case kSelTrigger:   del(scene_.triggers);   break;
    case kSelSoundZone: del(scene_.soundZones); break;
    case kSelColBox:    del(scene_.colBoxes);    break;
    case kSelColSphere: del(scene_.colSpheres); break;
    case kSelWaypoint:  del(scene_.waypoints);  break;
    case kSelNpc:         del(scene_.npcs);         break;
    case kSelEmitter:     del(scene_.emitters);     break;
    case kSelLight:       del(scene_.lights);       break;
    case kSelWater:       del(scene_.water);        break;
    case kSelScenery:     del(scene_.scenery);      break;
    case kSelPlayerSpawn: del(scene_.playerSpawns); break;
    }
    // Collision vis must be rebuilt whenever a scenery or col shape is removed.
    switch (selectedType_) {
    case kSelScenery:
    case kSelColBox:
    case kSelColSphere:
        scene_.colVisDirty = true;
        break;
    default: break;
    }
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted object %d.", selectedID_);
    ClearSelection();
}

// ─── Main Draw ────────────────────────────────────────────────────────────────

void ZonesTab::Draw(sqlite3* db, MediaTab* media) {
    renderer_.Init();
    EnsureScriptList();
    if (needFetchAreas_) FetchAreaList(db);

    // Auto-resync with Media: if the user edited materials or model
    // mappings since we last looked, re-sync our caches so textures /
    // material_map changes propagate without having to reload the zone.
    if (media && !scene_.areaName.empty()) {
        const int rev = media->MediaRevision();
        if (rev != last_media_revision_) {
            SyncSceneryCache(media);
            terrainMatsLoaded_ = false;   // media_materials may have changed
            last_media_revision_ = rev;
        }
        if (needsSpawnReload_) {
            media->ReloadPlayerSpawns(db);
            needsSpawnReload_ = false;
        }
    }

    // Promote completed thumbnails from background thread to main-thread cache.
    thumbs_.Tick();

    // ── Live sync: reload zone when external script bumps zone_dirty ──────
    if (!scene_.areaName.empty()) {
        using namespace std::chrono;
        double now = duration<double, std::milli>(
            steady_clock::now().time_since_epoch()).count();
        if (now >= zoneDirtyNextMs_) {
            zoneDirtyNextMs_ = now + 800.0;  // check every 800ms

            // Ensure the table exists (no-op if already present).
            sqlite3_exec(db,
                "CREATE TABLE IF NOT EXISTS zone_dirty "
                "(area_name TEXT PRIMARY KEY, version INTEGER DEFAULT 0)",
                nullptr, nullptr, nullptr);

            sqlite3_stmt* st = nullptr;
            sqlite3_prepare_v2(db,
                "SELECT version FROM zone_dirty WHERE area_name=?",
                -1, &st, nullptr);
            sqlite3_bind_text(st, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
            int ver = -1;
            if (sqlite3_step(st) == SQLITE_ROW)
                ver = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);

            if (ver != zoneDirtyVersion_) {
                if (zoneDirtyVersion_ >= 0) {
                    // Version changed — reload scene (preserves camera).
                    LoadZone(db, media, scene_.areaName);
                    std::snprintf(statusMsg_, sizeof(statusMsg_),
                        "Auto-reloaded: external change detected (v%d)", ver);
                }
                zoneDirtyVersion_ = ver;
            }
        }
    }

    // ── Top bar ────────────────────────────────────────────────────────────
    DrawTopBar(db, media);
    ImGui::Separator();

    float totalH  = ImGui::GetContentRegionAvail().y - kStatusBarH - 6.f;
    if (totalH < 64.f) totalH = 64.f;

    // ── Layout: [Scene | Viewport | Inspector]  +  Asset Browser below vp ─
    // The asset browser sits in the bottom portion of the viewport column
    // (like Unreal's content drawer). When a model is picked there, LMB in
    // the viewport places it directly — no right-click menu needed.
    static constexpr float kAssetBrowserH = 180.f;
    const float vpColH     = totalH - kAssetBrowserH - 2.f;

    // Left: scene hierarchy
    ImGui::BeginChild("##z_scene", {kSidebarW, totalH}, true);
    DrawSceneSidebar(db, media);
    ImGui::EndChild();
    ImGui::SameLine(0, 2.f);

    // Center column: viewport (top) + asset browser (bottom)
    float vpW = ImGui::GetContentRegionAvail().x - kInspectorW - 2.f;
    if (vpW < 64.f) vpW = 64.f;

    ImGui::BeginChild("##z_vpcol", {vpW, totalH}, false,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::BeginChild("##z_vp", {0.f, vpColH}, false);
    DrawViewport(db, media);

    // ── Drag-and-drop: ghost preview + placement ──────────────────────────
    // Check every frame whether a SCENERY_MODEL_ID payload is being dragged
    // over the viewport. While dragging: update ghost position (terrain snap).
    // On release (AcceptDragDropPayload): place the object.
    {
        const ImGuiPayload* hovering =
            ImGui::GetDragDropPayload();  // non-null while any drag is active
        const bool draggingScenery =
            hovering && hovering->IsDataType("SCENERY_MODEL_ID");

        if (draggingScenery && !scene_.areaName.empty()) {
            // Update ghost position under mouse every frame.
            ImVec2 mp  = ImGui::GetMousePos();
            float  vpX = mp.x - vpOrigin_.x;
            float  vpY = mp.y - vpOrigin_.y;
            glm::vec3 pos = RaycastScene(vpX, vpY);
            if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                pos.x = std::round(pos.x / scnGridSize_) * scnGridSize_;
                pos.z = std::round(pos.z / scnGridSize_) * scnGridSize_;
            }
            if (scnAlignGround_ && renderer_.terrain().Loaded())
                pos.y = renderer_.terrain().heightmap().SampleWorld(pos.x, pos.z);

            int dragId = *static_cast<const int*>(hovering->Data);

            // Resolve file path from media.
            std::string filePath;
            if (media) {
                for (auto& m : media->Models())
                    if (m.id == dragId) { filePath = m.file_path; break; }
            }
            renderer_.SetGhostModel(dragId, filePath, pos);
        } else {
            // Not dragging — clear ghost.
            renderer_.SetGhostModel(-1, {}, {});
        }

        // Accept the drop (mouse released over viewport).
        if (!scene_.areaName.empty() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl =
                    ImGui::AcceptDragDropPayload("SCENERY_MODEL_ID")) {
                int modelId = *static_cast<const int*>(pl->Data);
                ImVec2 mp  = ImGui::GetMousePos();
                glm::vec3 pos = RaycastScene(mp.x - vpOrigin_.x,
                                             mp.y - vpOrigin_.y);
                if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                    pos.x = std::round(pos.x / scnGridSize_) * scnGridSize_;
                    pos.z = std::round(pos.z / scnGridSize_) * scnGridSize_;
                }
                if (scnAlignGround_ && renderer_.terrain().Loaded())
                    pos.y = renderer_.terrain().heightmap().SampleWorld(pos.x, pos.z);

                scnModelId_ = modelId;
                zoneMode_   = kModeScenery;
                PlaceObject(pos, db, media);
                zoneMode_   = kModePortal;
                renderer_.SetGhostModel(-1, {}, {});  // clear ghost after place
            }
            ImGui::EndDragDropTarget();
        } else if (scene_.areaName.empty() && ImGui::BeginDragDropTarget()) {
            if (ImGui::AcceptDragDropPayload("SCENERY_MODEL_ID"))
                std::snprintf(statusMsg_, sizeof(statusMsg_),
                              "Load a zone first before placing objects.");
            ImGui::EndDragDropTarget();
        }
    }

    ImGui::EndChild();

    // ── Asset Browser ────────────────────────────────────────────────────
    ImGui::BeginChild("##z_assets", {0.f, 0.f}, true);
    if (media) {
        // Header row: search + snap controls
        ImGui::PushStyleColor(ImGuiCol_Text, {0.75f, 0.85f, 1.f, 1.f});
        ImGui::TextUnformatted("Asset Browser");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("— drag to viewport to place");

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 560.f);
        // Grid snap toggle (G key)
        if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_G, false))
            scnSnapGrid_ = !scnSnapGrid_;
        if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_O, false))
            scnObjSnap_ = !scnObjSnap_;
        if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_N, false))
            scnFaceSnap_ = !scnFaceSnap_;
        ImGui::Checkbox("Grid##ab", &scnSnapGrid_);
        if (scnSnapGrid_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.f);
            ImGui::DragFloat("##abgrid", &scnGridSize_, 0.25f, 0.25f, 32.f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grid size (world units)");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.f);
        ImGui::DragFloat("Rot##ab", &scnRotSnap_, 1.f, 0.f, 90.f, "%.0f°");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation snap (0 = free)");
        ImGui::SameLine();
        ImGui::Checkbox("ObjSnap##ab", &scnObjSnap_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pivot-to-pivot snap while moving gizmo [O]");
        if (scnObjSnap_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(55.f);
            ImGui::DragFloat("##abobjsnapdist", &scnObjSnapDist_, 0.05f, 0.05f, 8.f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap distance threshold (axis units)");
        }
        ImGui::SameLine();
        ImGui::Checkbox("FaceSnap##ab", &scnFaceSnap_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Face-to-face surface snap while moving gizmo [N]");
        if (scnFaceSnap_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.f);
            ImGui::DragFloat("##abfacesnapdist", &scnFaceSnapDist_, 0.05f, 0.05f, 8.f, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Face snap gap threshold");
            ImGui::SameLine();
            ImGui::Checkbox("AlignN##ab", &scnAlignNormal_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Align scenery yaw to snapped normal (XZ)");
            ImGui::SameLine();
            ImGui::Checkbox("AutoRot##ab", &scnAutoRotate_);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Auto-rotate scenery yaw during FaceSnap.\n"
                    "Picks the nearest snapped angle to current yaw.\n"
                    "Uses Rot step when > 0; otherwise falls back to 90°.");
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Ground##ab", &scnAlignGround_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap Y to terrain on placement");

        // Search bar
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputTextWithHint("##abfilt", "Search models...", scnFilter_, sizeof(scnFilter_));
        if (scnFilter_[0]) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x##abclear")) scnFilter_[0] = 0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl=fine  Shift=fast");

        // ── Model grid with folder groups ─────────────────────────────────
        ImGui::Separator();
        const auto& models = media->Models();
        const auto toLower = [](std::string s){
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        const std::string filt_lo = toLower(std::string(scnFilter_));

        // Tile dimensions.
        static constexpr float kThumbSz = 96.f;
        static constexpr float kTileW   = kThumbSz + 8.f;
        static constexpr float kTileH   = kThumbSz + 22.f;

        float avail_w = ImGui::GetContentRegionAvail().x;
        int   per_row = std::max(1, (int)(avail_w / (kTileW + 4.f)));

        // Helper: draw one model tile (thumbnail + label + drag source).
        // Returns true if clicked.
        auto DrawTile = [&](const MediaModel& m, int col_idx) {
            // In Foliage mode, tiles are a multi-select palette (toggle in/out
            // of foliageModelIds_) instead of the single-object scnModelId_
            // used by plain Scenery placement.
            const bool foliageMode = (zoneMode_ == kModeFoliage);
            bool sel = foliageMode
                ? (std::find(foliageModelIds_.begin(), foliageModelIds_.end(), m.id)
                       != foliageModelIds_.end())
                : (m.id == scnModelId_);
            ImVec2  tilePos = ImGui::GetCursorScreenPos();

            ImVec2 winMin = ImGui::GetWindowPos();
            ImVec2 winMax = {winMin.x + ImGui::GetWindowWidth(),
                             winMin.y + ImGui::GetWindowHeight()};
            bool visible = (tilePos.y + kTileH > winMin.y) &&
                           (tilePos.y           < winMax.y);
            GLuint thumb = visible ? thumbs_.Get(m.id, m.file_path) : 0;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec4 bgCol = sel ? (foliageMode ? ImVec4{0.20f,0.55f,0.25f,1.f}
                                              : ImVec4{0.18f,0.44f,0.80f,1.f})
                               : ImVec4{0.14f,0.14f,0.17f,1.f};
            dl->AddRectFilled(tilePos,
                {tilePos.x+kTileW, tilePos.y+kTileH},
                ImGui::ColorConvertFloat4ToU32(bgCol), 4.f);

            char bid[32]; std::snprintf(bid, sizeof(bid), "##t%d", m.id);
            ImGui::InvisibleButton(bid, {kTileW, kTileH});
            bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(0)) {
                if (foliageMode) {
                    auto fit = std::find(foliageModelIds_.begin(), foliageModelIds_.end(), m.id);
                    if (fit != foliageModelIds_.end()) foliageModelIds_.erase(fit);
                    else foliageModelIds_.push_back(m.id);
                } else {
                    scnModelId_ = m.id; zoneMode_ = kModeScenery;
                }
            }
            if (hov) {
                dl->AddRect(tilePos, {tilePos.x+kTileW, tilePos.y+kTileH},
                    IM_COL32(100,180,255,200), 4.f, 0, 1.5f);
                ImGui::SetTooltip(foliageMode
                    ? "%s\nid=%d\n%s\nClick to add/remove from foliage palette"
                    : "%s\nid=%d\n%s\nDrag to viewport to place",
                    m.name.c_str(), m.id, m.file_path.c_str());
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("SCENERY_MODEL_ID", &m.id, sizeof(int));
                if (thumb) { ImGui::Image((ImTextureID)(intptr_t)thumb,{48.f,48.f},{0,1},{1,0}); ImGui::SameLine(); }
                ImGui::TextColored({0.4f,0.9f,1.f,1.f}, "%s", m.name.c_str());
                ImGui::TextDisabled("Drop on viewport to place");
                ImGui::EndDragDropSource();
            }
            float ix = tilePos.x+4.f, iy = tilePos.y+4.f;
            if (thumb) {
                dl->AddImage((ImTextureID)(intptr_t)thumb,{ix,iy},{ix+kThumbSz,iy+kThumbSz},{0.f,1.f},{1.f,0.f});
            } else {
                dl->AddRectFilled({ix,iy},{ix+kThumbSz,iy+kThumbSz},IM_COL32(30,30,36,255),2.f);
                float t=ImGui::GetTime()*2.f, cx=ix+kThumbSz*.5f, cy=iy+kThumbSz*.5f;
                for (int d=0;d<3;++d){float a=t+d*2.094f,px=cx+std::cos(a)*10.f,py=cy+std::sin(a)*10.f;
                    float al=0.4f+0.6f*(0.5f+0.5f*std::sin(t+d));
                    dl->AddCircleFilled({px,py},3.f,IM_COL32(120,160,220,(int)(al*255)));}
            }
            // Label: show full media name (folder/name) to avoid ambiguity.
            const char* label = m.name.c_str();
            float ly = iy+kThumbSz+2.f;
            ImGui::PushClipRect({tilePos.x,ly},{tilePos.x+kTileW,ly+18.f},true);
            float tx = tilePos.x + 3.f;
            dl->AddText({tx,ly}, sel?IM_COL32(180,210,255,255):IM_COL32(200,200,200,255), label);
            ImGui::PopClipRect();
            if (col_idx % per_row != per_row-1) ImGui::SameLine(0,4.f);
        };

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  {4.f, 4.f});

        if (!filt_lo.empty()) {
            // ── Flat filtered view (no folders) ─────────────────────────
            int col = 0;
            for (const auto& m : models) {
                if (toLower(m.name).find(filt_lo) == std::string::npos) continue;
                DrawTile(m, col++);
            }
            if (col == 0)
                ImGui::TextDisabled("No models match \"%s\".", scnFilter_);
        } else {
            // ── Folder view ───────────────────────────────────────────────
            // Group models by their top-level folder (part before first '/').
            // Models without '/' go into a synthetic "(root)" group drawn first.
            struct FolderGroup {
                std::string              folder;   // "" = root
                std::vector<const MediaModel*> items;
            };
            std::vector<FolderGroup> groups;
            auto getGroup = [&](const std::string& folder) -> FolderGroup& {
                for (auto& g : groups) if (g.folder == folder) return g;
                groups.push_back({folder, {}});
                return groups.back();
            };
            for (const auto& m : models) {
                size_t sl = m.name.find('/');
                std::string folder = (sl != std::string::npos)
                                   ? m.name.substr(0, sl) : "";
                getGroup(folder).items.push_back(&m);
            }
            // Sort: root first, then alphabetically.
            std::sort(groups.begin(), groups.end(), [](auto& a, auto& b){
                if (a.folder.empty()) return true;
                if (b.folder.empty()) return false;
                return a.folder < b.folder;
            });

            // Persist open/closed state across frames.
            static std::unordered_map<std::string, bool> folderOpen;

            for (auto& g : groups) {
                if (g.items.empty()) continue;

                if (g.folder.empty()) {
                    // Root items — no folder header.
                    int col = 0;
                    for (const MediaModel* mp : g.items)
                        DrawTile(*mp, col++);
                    if (!groups.empty() && g.folder.empty()
                        && groups.size() > 1)
                        ImGui::Separator();
                } else {
                    // Folder header — collapsible.
                    // Auto-open if selected model is inside.
                    bool hasSelected = false;
                    for (auto* mp : g.items) if (mp->id == scnModelId_) { hasSelected = true; break; }
                    if (hasSelected && folderOpen.find(g.folder) == folderOpen.end())
                        folderOpen[g.folder] = true;

                    bool& open = folderOpen[g.folder];
                    char hdr[128];
                    std::snprintf(hdr, sizeof(hdr), "📁 %s  (%d)##fg_%s",
                        g.folder.c_str(), (int)g.items.size(), g.folder.c_str());
                    ImGui::PushStyleColor(ImGuiCol_Header,        {0.18f,0.20f,0.28f,1.f});
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.25f,0.30f,0.45f,1.f});
                    open = ImGui::CollapsingHeader(hdr,
                        open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                    ImGui::PopStyleColor(2);

                    if (open) {
                        ImGui::Indent(8.f);
                        int col = 0;
                        for (const MediaModel* mp : g.items)
                            DrawTile(*mp, col++);
                        ImGui::Unindent(8.f);
                        ImGui::Spacing();
                    }
                }
            }
        }

        ImGui::PopStyleVar(2);

    } else {
        ImGui::TextDisabled("No media loaded.");
    }
    ImGui::EndChild();  // ##z_assets

    ImGui::EndChild();  // ##z_vpcol

    ImGui::SameLine(0, 2.f);

    // Right: inspector
    ImGui::BeginChild("##z_insp", {0.f, totalH}, true);
    DrawInspector(db, media);
    ImGui::EndChild();

    // ── Status bar ─────────────────────────────────────────────────────────
    DrawStatusBar();
}

} // namespace gue
