#include "zones.h"
#include "media.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

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
    for (int i = 0; i < EditableTerrain::kMaxMats; ++i) {
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
    selectedID_   = -1;
    selectedType_ = kSelNone;
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
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Select a model first.");
            break;
        }
        ZScenery s;
        s.modelId    = scnModelId_;
        s.materialId = scnMaterialId_;
        s.pos        = wpos;
        s.scale      = {1,1,1};

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_scenery (area_name, model_id, material_id, x, y, z, sx, sy, sz)"
            " VALUES (?,?,?,?,?,?,1,1,1)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,2,s.modelId); sqlite3_bind_int(stmt,3,s.materialId);
            sqlite3_bind_double(stmt,4,s.pos.x); sqlite3_bind_double(stmt,5,s.pos.y); sqlite3_bind_double(stmt,6,s.pos.z);
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
        w.pos     = wpos;
        w.scale   = {wtrScaleX_, wtrScaleZ_};
        w.color   = wtrColor_;
        w.opacity = wtrOpacity_;
        w.damage  = wtrDamage_;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_water (area_name, x, y, z, scale_x, scale_z, color_r, color_g, color_b, opacity, damage)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,2,w.pos.x); sqlite3_bind_double(stmt,3,w.pos.y); sqlite3_bind_double(stmt,4,w.pos.z);
            sqlite3_bind_double(stmt,5,w.scale.x); sqlite3_bind_double(stmt,6,w.scale.y);
            sqlite3_bind_int(stmt,7,(int)(w.color.r*255)); sqlite3_bind_int(stmt,8,(int)(w.color.g*255)); sqlite3_bind_int(stmt,9,(int)(w.color.b*255));
            sqlite3_bind_int(stmt,10,w.opacity); sqlite3_bind_int(stmt,11,w.damage);
            sqlite3_step(stmt);
            w.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.water.push_back(w);
            selectedID_ = w.id; selectedType_ = kSelWater;
            PushUndo(kUndoCreate, kSelWater, w.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Placed water id=%d.", w.id);
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
    default: break;
    }
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
            selectedID_ = -1; selectedType_ = kSelNone;
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
        case kSelWater:     table = "zone_water";       break;
        case kSelScenery:   table = "zone_scenery";     break;
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
        case kSelNpc:       rem(scene_.npcs);       break;
        case kSelEmitter:   rem(scene_.emitters);   break;
        case kSelWater:     rem(scene_.water);      break;
        case kSelScenery:   rem(scene_.scenery);    break;
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

void ZonesTab::DuplicateSelected(sqlite3* db) {
    if (selectedID_ < 0) return;
    // Re-place the current object at pos + small offset
    // For scenery, portal, trigger etc. — just re-call PlaceObject with same settings
    // but offset by (2,0,0)
    glm::vec3 offset = {2.f, 0.f, 0.f};

    switch (selectedType_) {
    case kSelScenery: {
        auto it = std::find_if(scene_.scenery.begin(), scene_.scenery.end(),
                               [&](auto& s){ return s.id == selectedID_; });
        if (it == scene_.scenery.end()) return;
        ZScenery s = *it;
        s.id = 0; s.pos += offset;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO zone_scenery (area_name, model_id, material_id, x, y, z, pitch, yaw, roll, sx, sy, sz, collision, anim_mode, inv_size, ownable, locked)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt,1,scene_.areaName.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt,2,s.modelId); sqlite3_bind_int(stmt,3,s.materialId);
            sqlite3_bind_double(stmt,4,s.pos.x); sqlite3_bind_double(stmt,5,s.pos.y); sqlite3_bind_double(stmt,6,s.pos.z);
            sqlite3_bind_double(stmt,7,s.rot.x); sqlite3_bind_double(stmt,8,s.rot.y); sqlite3_bind_double(stmt,9,s.rot.z);
            sqlite3_bind_double(stmt,10,s.scale.x); sqlite3_bind_double(stmt,11,s.scale.y); sqlite3_bind_double(stmt,12,s.scale.z);
            sqlite3_bind_int(stmt,13,s.collision); sqlite3_bind_int(stmt,14,s.animMode);
            sqlite3_bind_int(stmt,15,s.invSize); sqlite3_bind_int(stmt,16,s.ownable?1:0); sqlite3_bind_int(stmt,17,s.locked?1:0);
            sqlite3_step(stmt);
            s.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(stmt);
            scene_.scenery.push_back(s);
            selectedID_ = s.id;
            PushUndo(kUndoCreate, kSelScenery, s.id);
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated scenery → id=%d.", s.id);
        }
        break;
    }
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
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicated portal → id=%d.", p.id);
        }
        break;
    }
    default:
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Duplicate not supported for this object type.");
        break;
    }
}

// ─── DeleteSelected ───────────────────────────────────────────────────────────

void ZonesTab::DeleteSelected(sqlite3* db) {
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
        selectedID_ = -1; selectedType_ = kSelNone; spawnPtSelMob_ = -1;
        return;
    }
    case kSelWater:     table = "zone_water";       break;
    case kSelScenery:   table = "zone_scenery";     break;
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
    case kSelNpc:       del(scene_.npcs);       break;
    case kSelEmitter:   del(scene_.emitters);   break;
    case kSelWater:     del(scene_.water);      break;
    case kSelScenery:   del(scene_.scenery);    break;
    }
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted object %d.", selectedID_);
    selectedID_ = -1; selectedType_ = kSelNone;
}

// ─── Top bar (Save / Undo / hints) ───────────────────────────────────────────

void ZonesTab::DrawTopBar(sqlite3* db) {
    bool hasZone = !scene_.areaName.empty();

    if (!hasZone) ImGui::BeginDisabled();
    if (ImGui::Button("Save"))   SaveZone(db);
    if (!hasZone) ImGui::EndDisabled();

    ImGui::SameLine();
    bool canReload = selectedArea_ >= 0 && selectedArea_ < (int)areaList_.size();
    if (!canReload) ImGui::BeginDisabled();
    if (ImGui::Button("Reload")) LoadZone(db, nullptr, areaList_[selectedArea_]);
    if (!canReload) ImGui::EndDisabled();

    ImGui::SameLine(0, 16.f);
    if (ImGui::Button("Undo"))          Undo(db);
    if (ImGui::IsItemHovered())         ImGui::SetTooltip("Ctrl+Z");
    ImGui::SameLine();
    if (ImGui::Button("Duplicate"))     DuplicateSelected(db);
    if (ImGui::IsItemHovered())         ImGui::SetTooltip("Ctrl+D");

    // ── Mode selector ────────────────────────────────────────────────────
    ImGui::SameLine(0, 24.f);
    ImGui::TextDisabled("Mode:");
    ImGui::SameLine();
    static const char* kModeLabels[kModeCount] = {
        "Scenery","Terrain","Emitters","Water","ColBox",
        "Sound","Trigger","Waypoint","Portal","NPC",
        "Enviro","Other","SpawnPt"
    };
    ImGui::SetNextItemWidth(120.f);
    if (ImGui::BeginCombo("##zmode", kModeLabels[zoneMode_])) {
        for (int i = 0; i < kModeCount; ++i) {
            bool sel = (zoneMode_ == i);
            if (ImGui::Selectable(kModeLabels[i], sel)) {
                zoneMode_ = (ZoneMode)i;
                // Drop the current selection — the mode's placement panel takes over.
                selectedID_   = -1;
                selectedType_ = kSelNone;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0, 24.f);
    if (hasZone) {
        ImGui::TextColored({0.5f, 0.8f, 1.f, 0.7f}, "%s", scene_.areaName.c_str());
    } else {
        ImGui::TextDisabled("no zone");
    }
}

// ─── Scene sidebar (left panel) ──────────────────────────────────────────────

static void SectionHeader(const char* label, ImVec4 col = {0.45f, 0.75f, 1.f, 1.f}) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

void ZonesTab::DrawSceneSidebar(sqlite3* db, MediaTab* media) {
    // ── Zone selector ──────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(-1.f);
    const char* cur = scene_.areaName.empty() ? "(select zone)" : scene_.areaName.c_str();
    if (ImGui::BeginCombo("##zsb_zone", cur)) {
        for (int i = 0; i < (int)areaList_.size(); ++i) {
            bool sel = (i == selectedArea_);
            if (ImGui::Selectable(areaList_[i].c_str(), sel)) {
                selectedArea_ = i;
                LoadZone(db, media, areaList_[i]);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ── Zone actions ───────────────────────────────────────────────────────
    float hw = (ImGui::GetContentRegionAvail().x - 2.f) * 0.5f;
    if (ImGui::Button("+ New##zsb", {hw, 0})) showNewArea_ = !showNewArea_;
    ImGui::SameLine(0, 2.f);
    ImGui::PushStyleColor(ImGuiCol_Button, {0.55f, 0.1f, 0.1f, 1.f});
    bool canDel = !scene_.areaName.empty();
    if (!canDel) ImGui::BeginDisabled();
    if (ImGui::Button("Delete##zsb", {-1.f, 0}) && canDel) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db, "DELETE FROM area_config WHERE name=?", -1, &s, nullptr);
        sqlite3_bind_text(s, 1, scene_.areaName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);
        scene_.Clear(); needFetchAreas_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Deleted zone.");
    }
    if (!canDel) ImGui::EndDisabled();
    ImGui::PopStyleColor();

    if (showNewArea_) {
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##zsb_newname", "Zone name", newAreaBuf_, sizeof(newAreaBuf_));
        if (ImGui::Button("Create##zsb") && newAreaBuf_[0]) {
            sqlite3_stmt* s = nullptr;
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO area_config (name) VALUES (?)",
                               -1, &s, nullptr);
            sqlite3_bind_text(s, 1, newAreaBuf_, -1, SQLITE_TRANSIENT);
            sqlite3_step(s); sqlite3_finalize(s);
            needFetchAreas_ = true;
            showNewArea_ = false;
            std::snprintf(statusMsg_, sizeof(statusMsg_), "Created '%s'.", newAreaBuf_);
            newAreaBuf_[0] = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##zsb")) showNewArea_ = false;
    }

    if (scene_.areaName.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a zone above.");
        return;
    }

    ImGui::Separator();

    // ── Object hierarchy ───────────────────────────────────────────────────
    // Helper: draw one group of objects as a collapsible tree.
    auto DrawGroup = [&](const char* icon, const char* label,
                         auto& vec, int selType, ImVec4 col) {
        if (vec.empty()) return;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        char hdr[64];
        std::snprintf(hdr, 64, "%s %s (%d)##grp%s", icon, label, (int)vec.size(), label);
        bool open = ImGui::TreeNodeEx(hdr,
                        ImGuiTreeNodeFlags_DefaultOpen |
                        ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::PopStyleColor();
        if (!open) return;
        for (auto& obj : vec) {
            bool sel = (selectedType_ == selType && selectedID_ == obj.id);
            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_Leaf |
                ImGuiTreeNodeFlags_NoTreePushOnOpen |
                ImGuiTreeNodeFlags_SpanFullWidth;
            if (sel) {
                flags |= ImGuiTreeNodeFlags_Selected;
                ImGui::PushStyleColor(ImGuiCol_Header,
                                      {0.25f, 0.55f, 0.90f, 0.60f});
            }
            // Build item label
            char lbl[64];
            if constexpr (requires { obj.name; }) {
                if (!obj.name.empty())
                    std::snprintf(lbl, 64, "%s #%d", obj.name.c_str(), obj.id);
                else
                    std::snprintf(lbl, 64, "#%d", obj.id);
            } else {
                std::snprintf(lbl, 64, "#%d", obj.id);
            }
            ImGui::TreeNodeEx((void*)(intptr_t)obj.id, flags, "%s", lbl);
            if (sel) ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) {
                selectedID_   = obj.id;
                selectedType_ = selType;
            }
        }
        ImGui::TreePop();
    };

    DrawGroup("P", "Portals",    scene_.portals,    kSelPortal,    {0.30f,0.55f,1.00f,1.f});
    DrawGroup("T", "Triggers",   scene_.triggers,   kSelTrigger,   {1.00f,0.55f,0.10f,1.f});
    DrawGroup("S", "Sound",      scene_.soundZones, kSelSoundZone, {1.00f,1.00f,0.30f,1.f});
    DrawGroup("B", "ColBoxes",   scene_.colBoxes,   kSelColBox,    {0.90f,0.25f,0.25f,1.f});
    DrawGroup("O", "ColSpheres", scene_.colSpheres, kSelColSphere, {1.00f,0.45f,0.00f,1.f});
    DrawGroup("W", "Waypoints",  scene_.waypoints,  kSelWaypoint,  {0.20f,0.90f,1.00f,1.f});
    DrawGroup("N", "NPCs",       scene_.npcs,       kSelNpc,       {0.30f,0.95f,0.30f,1.f});
    DrawGroup("G", "SpawnPts",   scene_.spawnPoints,kSelSpawnPoint,{0.10f,0.90f,0.40f,1.f});
    DrawGroup("~", "Water",      scene_.water,      kSelWater,     {0.10f,0.70f,1.00f,1.f});
    DrawGroup("E", "Emitters",   scene_.emitters,   kSelEmitter,   {0.80f,1.00f,0.20f,1.f});
    DrawGroup("M", "Scenery",    scene_.scenery,    kSelScenery,   {0.80f,0.78f,0.75f,1.f});
}

// ─── Floating toolbar (overlaid inside viewport) ──────────────────────────────

void ZonesTab::DrawFloatingToolbar() {
    // Keyboard shortcuts (Q/W/E/R and F1-F4)
    if (vpHovered_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) xformMode_ = kXFormSelect;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) xformMode_ = kXFormMove;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) xformMode_ = kXFormRotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) xformMode_ = kXFormScale;
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) xformMode_ = kXFormSelect;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) xformMode_ = kXFormMove;
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) xformMode_ = kXFormRotate;
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) xformMode_ = kXFormScale;
    }

    // Overlay buttons at top-left of viewport
    ImGui::SetCursorScreenPos({vpOrigin_.x + 6.f, vpOrigin_.y + 6.f});

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    {3.f, 3.f});
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.12f, 0.12f, 0.14f, 0.88f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.30f, 0.35f, 0.95f});

    struct Tool { const char* label; const char* key; XFormMode mode; };
    static const Tool kTools[] = {
        {"Select", "Q", kXFormSelect},
        {"Move",   "W", kXFormMove  },
        {"Rotate", "E", kXFormRotate},
        {"Scale",  "R", kXFormScale },
    };
    for (auto& t : kTools) {
        bool active = (xformMode_ == t.mode);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.22f, 0.52f, 0.88f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.62f, 1.00f, 1.f});
        }
        if (ImGui::Button(t.label, {60.f, 22.f})) xformMode_ = t.mode;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  [%s]", t.label, t.key);
        if (active) ImGui::PopStyleColor(2);
        ImGui::SameLine(0, 3.f);
    }

    // ── Render-mode toggle (Simple / Lit) ─────────────────────────────────
    ImGui::SameLine(0, 16.f);
    bool pbr = (renderer_.renderMode() == ZoneRenderer::kRenderPBR);
    if (pbr) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.95f, 0.55f, 0.10f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1.00f, 0.65f, 0.20f, 1.f});
    }
    if (ImGui::Button(pbr ? "Lit" : "Simple", {60.f, 22.f})) {
        renderer_.SetRenderMode(pbr ? ZoneRenderer::kRenderSimple
                                    : ZoneRenderer::kRenderPBR);
    }
    if (pbr) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Viewport shading:\n"
                          "  Simple  — fast, flat lighting\n"
                          "  Lit     — full client PBR (shadows/SSAO/IBL)");

    // Mouselook hint
    if (mouseLook_) {
        ImGui::SameLine(0, 12.f);
        ImGui::PushStyleColor(ImGuiCol_Text, {0.4f, 1.f, 0.4f, 1.f});
        ImGui::TextUnformatted("● look");
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ─── Viewport (fills its parent child window) ────────────────────────────────

void ZonesTab::DrawViewport(sqlite3* db, MediaTab* media) {
    ImGuiIO& io = ImGui::GetIO();
    float    dt = io.DeltaTime;

    // Fill the entire child window — no padding math needed.
    ImVec2 vp = ImGui::GetContentRegionAvail();
    if (vp.x < 64) vp.x = 64;
    if (vp.y < 64) vp.y = 64;
    vpSize_ = vp;

    renderer_.Resize((int)vp.x, (int)vp.y);
    if (scene_.colVisDirty) {
        scene_.RebuildColVis(db, meshTriCache_);
        renderer_.UploadColVisBatch(scene_.colVis);
    }
    renderer_.RenderFrame(cam_, scene_, selectedID_, selectedType_, dt);

    // Render the FBO as an ImGui Image.  This is the primary widget — ImGui
    // tracks its rect for hover/click detection automatically.
    ImGui::Image(renderer_.GetTexture(), vp, {0.f, 1.f}, {1.f, 0.f});
    vpOrigin_  = ImGui::GetItemRectMin();   // screen-space top-left of the image
    vpHovered_ = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // ── Terrain brush cursor: hover raycast + circle overlay ─────────────
    if (zoneMode_ == kModeTerrain && renderer_.terrain().Loaded()) {
        float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        ImVec2 mp = ImGui::GetMousePos();
        float vpX = mp.x - vpOrigin_.x;
        float vpY = mp.y - vpOrigin_.y;
        if (vpHovered_ && vpX >= 0.f && vpY >= 0.f && vpX < vpSize_.x && vpY < vpSize_.y) {
            float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
            float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
            glm::vec3 ray = cam_.NDCRay(ndcX, ndcY, aspect);
            glm::vec3 hit;
            if (renderer_.terrain().Raycast(cam_.pos, ray, hit)) {
                brushHitPos_     = hit;
                brushHoverValid_ = true;
            } else {
                brushHoverValid_ = false;
            }
        } else {
            brushHoverValid_ = false;
        }

        // Draw 3D cursor circle as ImGui overlay on top of the viewport image
        if (brushHoverValid_) {
            glm::mat4 vpMat = cam_.Proj(aspect) * cam_.View();
            ImDrawList* dl  = ImGui::GetWindowDrawList();

            // Project a world-space ring onto screen space
            static constexpr int kSeg = 48;
            static constexpr float kPi2 = 6.28318530f;
            ImVec2 pts[kSeg + 1];
            int ptCount = 0;
            for (int i = 0; i <= kSeg; ++i) {
                float a = (float)i / kSeg * kPi2;
                glm::vec4 wp = {
                    brushHitPos_.x + std::cos(a) * brushRadius_,
                    brushHitPos_.y + 0.25f,   // slight lift to avoid z-fighting
                    brushHitPos_.z + std::sin(a) * brushRadius_,
                    1.0f
                };
                glm::vec4 clip = vpMat * wp;
                if (clip.w <= 0.0001f) continue;
                clip /= clip.w;
                if (clip.x < -1.5f || clip.x > 1.5f || clip.y < -1.5f || clip.y > 1.5f) continue;
                float sx = vpOrigin_.x + (clip.x * 0.5f + 0.5f) * vpSize_.x;
                float sy = vpOrigin_.y + (-clip.y * 0.5f + 0.5f) * vpSize_.y;
                pts[ptCount++] = {sx, sy};
            }
            if (ptCount > 2) {
                // Dark outline for contrast, then coloured ring
                dl->AddPolyline(pts, ptCount, IM_COL32(0, 0, 0, 140), ImDrawFlags_None, 3.5f);
                ImU32 ringCol = (brushMode_ == 4)
                    ? IM_COL32(255, 215, 50, 230)   // yellow for paint
                    : IM_COL32(255, 255, 255, 230);  // white for sculpt
                dl->AddPolyline(pts, ptCount, ringCol, ImDrawFlags_None, 1.5f);
            }

            // Centre cross-hair
            glm::vec4 cen4 = vpMat * glm::vec4(brushHitPos_, 1.f);
            if (cen4.w > 0.f) {
                cen4 /= cen4.w;
                float cx = vpOrigin_.x + (cen4.x * 0.5f + 0.5f) * vpSize_.x;
                float cy = vpOrigin_.y + (-cen4.y * 0.5f + 0.5f) * vpSize_.y;
                float cs = 4.f;
                dl->AddLine({cx - cs, cy}, {cx + cs, cy}, IM_COL32(0,0,0,140), 2.5f);
                dl->AddLine({cx, cy - cs}, {cx, cy + cs}, IM_COL32(0,0,0,140), 2.5f);
                ImU32 dotCol = (brushMode_ == 4) ? IM_COL32(255,215,50,230) : IM_COL32(255,255,255,230);
                dl->AddLine({cx - cs, cy}, {cx + cs, cy}, dotCol, 1.5f);
                dl->AddLine({cx, cy - cs}, {cx, cy + cs}, dotCol, 1.5f);
            }
        }
    } else {
        brushHoverValid_ = false;
    }

    // ── Camera controls — RC 1.26 style ─────────────────────────────────
    //
    //   SPACE hold          → mouselook on (cursor hidden, mouse rotates view)
    //   SPACE + LMB         → move forward
    //   SPACE + RMB         → move backward
    //   MMB drag            → pan (horizontal = strafe, vertical = up/down)
    //   Shift (held)        → 3× speed boost on ALL movement (keys/pan/zoom)
    //   Numpad 8/2/4/6      → forward / back / strafe left / strafe right
    //   Numpad 9/7          → up / down
    //   Numpad 1/3          → turn left / right (yaw, even without mouselook)
    //   Scroll              → zoom (dolly along view direction)
    //   Ctrl + Scroll       → adjust base camera speed (slider in toolbar)
    //
    // Without SPACE: LMB selects, RMB opens the context/placement menu —
    // those are handled further down.
    const bool spaceDown = vpHovered_ && ImGui::IsKeyDown(ImGuiKey_Space);
    mouseLook_ = spaceDown;

    // Middle-mouse pan — engages when MMB is pressed over the viewport and
    // stays engaged while held, even if the cursor leaves the image rect.
    // Separate from mouselook so you can pan without rotating.
    if (vpHovered_ && ImGui::IsMouseClicked(2)) mmbPan_ = true;
    if (!ImGui::IsMouseDown(2))                 mmbPan_ = false;

    if (vpHovered_ || mouseLook_ || mmbPan_) {
        const bool shiftBoost = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                             || ImGui::IsKeyDown(ImGuiKey_RightShift);
        const bool ctrlDown   = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                             || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

        // Ctrl + Scroll → adjust the base (persisted) camera speed,
        // exactly like dragging the Speed slider in the toolbar.
        if (io.MouseWheel != 0.f && ctrlDown) {
            cam_.speed = std::clamp(cam_.speed * std::pow(1.15f, io.MouseWheel),
                                    1.f, 500.f);
        }

        // Shift boost — 3× effective speed for THIS FRAME only. Applied
        // by temporarily bumping cam_.speed so every speed-based system
        // below (cam.Update, MMB pan, scroll zoom) picks it up uniformly.
        // Restored at the end of the block so the boost never persists.
        const float baseSpeed = cam_.speed;
        if (shiftBoost) cam_.speed = baseSpeed * 3.f;

        bool fwd = ImGui::IsKeyDown(ImGuiKey_Keypad8)
                || ImGui::IsKeyDown(ImGuiKey_UpArrow)
                || (mouseLook_ && ImGui::IsMouseDown(0));   // SPACE + LMB
        bool back = ImGui::IsKeyDown(ImGuiKey_Keypad2)
                || ImGui::IsKeyDown(ImGuiKey_DownArrow)
                || (mouseLook_ && ImGui::IsMouseDown(1));   // SPACE + RMB
        bool left  = ImGui::IsKeyDown(ImGuiKey_Keypad4);
        bool right = ImGui::IsKeyDown(ImGuiKey_Keypad6);
        bool up    = ImGui::IsKeyDown(ImGuiKey_Keypad9) || ImGui::IsKeyDown(ImGuiKey_PageUp);
        bool down  = ImGui::IsKeyDown(ImGuiKey_Keypad7) || ImGui::IsKeyDown(ImGuiKey_PageDown);

        float mdx = mouseLook_ ? io.MouseDelta.x : 0.f;
        float mdy = mouseLook_ ? io.MouseDelta.y : 0.f;

        // Numpad 1/3 = yaw keys — emulate as extra horizontal mouse delta.
        // Roughly 90°/s at the camera's sensitivity so the control feels
        // comparable to holding the numpad in RC.
        const float kYawKeyRate = 90.f / std::max(cam_.sens, 0.01f);
        if (ImGui::IsKeyDown(ImGuiKey_Keypad1)) mdx -= kYawKeyRate * dt;
        if (ImGui::IsKeyDown(ImGuiKey_Keypad3)) mdx += kYawKeyRate * dt;

        cam_.Update(dt, fwd, back, left, right, up, down, mdx, mdy);

        // MMB pan — drag mouse to slide the camera sideways / vertically.
        // "Scene follows the cursor": drag right → camera moves left.
        // Scales with (boosted) camera speed so Shift speeds up panning too.
        if (mmbPan_) {
            const float kPanScale = 0.02f * cam_.speed / 20.f;
            glm::vec3 r = cam_.Right();
            glm::vec3 u = glm::vec3(0.f, 1.f, 0.f);
            cam_.pos -= r * (io.MouseDelta.x * kPanScale);
            cam_.pos += u * (io.MouseDelta.y * kPanScale);
        }

        // Scroll (no Ctrl) → dolly zoom, UNLESS we are in terrain mode without
        // mouselook — there scroll adjusts brush radius (Shift = strength).
        bool terrainScrollOverride = (zoneMode_ == kModeTerrain && !mouseLook_ && vpHovered_);
        if (io.MouseWheel != 0.f && !ctrlDown) {
            if (terrainScrollOverride) {
                bool shiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                              || ImGui::IsKeyDown(ImGuiKey_RightShift);
                if (shiftDown) {
                    brushStrength_ = std::clamp(brushStrength_ + io.MouseWheel * 0.1f, 0.05f, 3.f);
                } else {
                    brushRadius_ = std::clamp(brushRadius_ + io.MouseWheel * 2.f, 1.f, 80.f);
                }
            } else {
                const float kZoomStep = 0.1f * cam_.speed;
                cam_.pos += cam_.Forward() * (io.MouseWheel * kZoomStep);
            }
        }

        // Restore base speed so the boost never persists across frames.
        cam_.speed = baseSpeed;
    }

    // Selection — LMB click (select mode)
    if (vpHovered_ && ImGui::IsMouseClicked(0) && xformMode_ == kXFormSelect) {
        ImVec2 mp = ImGui::GetMousePos();
        float vpX = mp.x - vpOrigin_.x;
        float vpY = mp.y - vpOrigin_.y;
        float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
        float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
        float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        glm::vec3 orig = cam_.pos;
        glm::vec3 dir  = cam_.NDCRay(ndcX, ndcY, aspect);

        // Ray-sphere intersection helper
        auto raySphere = [](const glm::vec3& ro, const glm::vec3& rd,
                            const glm::vec3& center, float r) -> float {
            glm::vec3 oc = ro - center;
            float b = glm::dot(oc, rd);
            float c = glm::dot(oc, oc) - r*r;
            float disc = b*b - c;
            if (disc < 0.f) return -1.f;
            return -b - std::sqrt(disc);
        };
        // Ray-AABB intersection helper
        auto rayAABB = [](const glm::vec3& ro, const glm::vec3& rd,
                          const glm::vec3& mn, const glm::vec3& mx) -> float {
            glm::vec3 invD = 1.f / rd;
            glm::vec3 t0   = (mn - ro) * invD;
            glm::vec3 t1   = (mx - ro) * invD;
            glm::vec3 tmin = glm::min(t0, t1);
            glm::vec3 tmax = glm::max(t0, t1);
            float tenter = std::max({tmin.x, tmin.y, tmin.z});
            float texit  = std::min({tmax.x, tmax.y, tmax.z});
            if (texit < tenter || texit < 0.f) return -1.f;
            return tenter;
        };

        float bestT = 1e9f;
        int   bestID   = -1;
        int   bestType = kSelNone;

        // Test portals
        for (auto& p : scene_.portals) {
            glm::vec3 c = {p.pos.x, p.radius, p.pos.z};
            float t = raySphere(orig, dir, c, p.radius);
            if (t > 0.f && t < bestT) { bestT = t; bestID = p.id; bestType = kSelPortal; }
        }
        // Test triggers
        for (auto& t : scene_.triggers) {
            float r = t.radius;
            float tt = raySphere(orig, dir, {t.x, 0.f, t.z}, r);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = t.id; bestType = kSelTrigger; }
        }
        // Test sound zones
        for (auto& s : scene_.soundZones) {
            float tt = raySphere(orig, dir, {s.x, 0.f, s.z}, s.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = s.id; bestType = kSelSoundZone; }
        }
        // Test colboxes
        for (auto& c : scene_.colBoxes) {
            glm::vec3 half = c.scale * 0.5f;
            float tt = rayAABB(orig, dir, c.pos - half, c.pos + half);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = c.id; bestType = kSelColBox; }
        }
        // Test colspheres
        for (auto& s : scene_.colSpheres) {
            float tt = raySphere(orig, dir, s.pos, s.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = s.id; bestType = kSelColSphere; }
        }
        // Test waypoints
        for (auto& w : scene_.waypoints) {
            float tt = raySphere(orig, dir, w.pos, 0.8f);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = w.id; bestType = kSelWaypoint; }
        }
        // Test NPCs
        for (auto& n : scene_.npcs) {
            glm::vec3 mn = n.pos - glm::vec3(0.35f, 0.f, 0.35f);
            glm::vec3 mx = n.pos + glm::vec3(0.35f, 1.0f, 0.35f);
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = n.id; bestType = kSelNpc; }
        }
        // Test emitters
        for (auto& e : scene_.emitters) {
            glm::vec3 mn = e.pos - glm::vec3(0.4f, 0.f, 0.4f);
            glm::vec3 mx = e.pos + glm::vec3(0.4f, 1.5f, 0.4f);
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = e.id; bestType = kSelEmitter; }
        }
        // Test water
        for (auto& w : scene_.water) {
            glm::vec3 mn = {w.pos.x - w.scale.x*0.5f, w.pos.y - 0.1f, w.pos.z - w.scale.y*0.5f};
            glm::vec3 mx = {w.pos.x + w.scale.x*0.5f, w.pos.y + 0.2f, w.pos.z + w.scale.y*0.5f};
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = w.id; bestType = kSelWater; }
        }
        // Test scenery (approximate AABB from scale)
        for (auto& s : scene_.scenery) {
            glm::vec3 half = s.scale * 0.5f;
            float tt = rayAABB(orig, dir, s.pos - half, s.pos + half);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = s.id; bestType = kSelScenery; }
        }

        if (bestID >= 0) {
            // Waypoint link mode: clicking a waypoint sets nextA or nextB on the current selection
            if (wpLinkMode_ && bestType == kSelWaypoint &&
                selectedType_ == kSelWaypoint && selectedID_ >= 0 && bestID != selectedID_) {
                for (auto& w : scene_.waypoints) {
                    if (w.id == selectedID_) {
                        if (wpLinkB_) w.nextB = bestID; else w.nextA = bestID;
                        sqlite3_stmt* s = nullptr;
                        sqlite3_prepare_v2(db,
                            "UPDATE zone_waypoints SET next_a=?,next_b=? WHERE id=?",
                            -1, &s, nullptr);
                        sqlite3_bind_int(s,1,w.nextA); sqlite3_bind_int(s,2,w.nextB);
                        sqlite3_bind_int(s,3,w.id);
                        sqlite3_step(s); sqlite3_finalize(s);
                        std::snprintf(statusMsg_, sizeof(statusMsg_),
                                      "Linked WP #%d → %s WP #%d.", selectedID_,
                                      wpLinkB_ ? "B" : "A", bestID);
                        break;
                    }
                }
                wpLinkMode_ = false;
            } else {
                selectedID_   = bestID;
                selectedType_ = bestType;
                // Switch to mode matching selected object type
                switch (bestType) {
                case kSelPortal:    zoneMode_ = kModePortal;    break;
                case kSelTrigger:   zoneMode_ = kModeTrigger;   break;
                case kSelSoundZone: zoneMode_ = kModeSoundZone; break;
                case kSelColBox:    zoneMode_ = kModeColBox;    break;
                case kSelColSphere: zoneMode_ = kModeColSphere; break;
                case kSelWaypoint:  zoneMode_ = kModeWaypoint;  break;
                case kSelNpc:       zoneMode_ = kModeNPC;       break;
                case kSelEmitter:   zoneMode_ = kModeEmitters;  break;
                case kSelWater:     zoneMode_ = kModeWater;     break;
                default: break;
                }
            }
        } else {
            selectedID_   = -1;
            selectedType_ = kSelNone;
        }
    }

    // ── Terrain brush (LMB-drag in Terrain mode) ─────────────────────────
    if (zoneMode_ == kModeTerrain && !mouseLook_ && renderer_.terrain().Loaded()) {
        if (vpHovered_ && ImGui::IsMouseDown(0)) {
            // Capture one snapshot at the start of each brush stroke
            if (!terrainStrokeActive_) {
                terrainStrokeActive_ = true;
                TerrainSnapshot snap;
                snap.heights = renderer_.terrain().heightmap().heights;
                snap.splat   = renderer_.terrain().splatmap().data;
                if ((int)terrainUndo_.size() >= kMaxTerrainUndo)
                    terrainUndo_.erase(terrainUndo_.begin());
                terrainUndo_.push_back(std::move(snap));
                terrainRedo_.clear();
            }

            ImVec2 mp  = ImGui::GetMousePos();
            float  vpX = mp.x - vpOrigin_.x;
            float  vpY = mp.y - vpOrigin_.y;
            float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
            float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
            float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
            glm::vec3 origin = cam_.pos;
            glm::vec3 dir    = cam_.NDCRay(ndcX, ndcY, aspect);
            glm::vec3 hit;
            if (renderer_.terrain().Raycast(origin, dir, hit)) {
                if (brushMode_ == 4) {
                    renderer_.terrain().Paint(hit.x, hit.z, brushRadius_,
                                              brushStrength_, dt, brushMaterial_,
                                              (BrushFalloff)brushFalloff_);
                } else {
                    // UI index 5 = Noise → BrushMode::Noise (enum value 4)
                    BrushMode bm = (brushMode_ == 5) ? BrushMode::Noise
                                                      : (BrushMode)brushMode_;
                    renderer_.terrain().ApplyBrush(hit.x, hit.z, brushRadius_,
                                                   brushStrength_, dt, bm,
                                                   brushFlattenH_,
                                                   (BrushFalloff)brushFalloff_);
                }
                brushActive_ = true;
            }
        } else if (ImGui::IsMouseReleased(0)) {
            terrainStrokeActive_ = false;
            brushActive_ = false;
        }
        // Radius shortcuts
        if (vpHovered_) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))  brushRadius_ = std::max(1.f,  brushRadius_ - 2.f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) brushRadius_ = std::min(80.f, brushRadius_ + 2.f);
        }
    }

    // Delete key
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        DeleteSelected(db);

    // Ctrl+Z — undo (terrain undo when in terrain mode, scene undo otherwise)
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (zoneMode_ == kModeTerrain && !terrainUndo_.empty()) {
            TerrainSnapshot cur;
            cur.heights = renderer_.terrain().heightmap().heights;
            cur.splat   = renderer_.terrain().splatmap().data;
            terrainRedo_.push_back(std::move(cur));
            auto& prev = terrainUndo_.back();
            renderer_.terrain().heightmap().heights = prev.heights;
            renderer_.terrain().heightmap().InitGPU();
            renderer_.terrain().splatmap().data  = prev.splat;
            renderer_.terrain().splatmap().dirty = true;
            terrainUndo_.pop_back();
        } else {
            Undo(db);
        }
    }
    // Ctrl+Y — redo (terrain only)
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (zoneMode_ == kModeTerrain && !terrainRedo_.empty()) {
            TerrainSnapshot cur;
            cur.heights = renderer_.terrain().heightmap().heights;
            cur.splat   = renderer_.terrain().splatmap().data;
            if ((int)terrainUndo_.size() >= kMaxTerrainUndo)
                terrainUndo_.erase(terrainUndo_.begin());
            terrainUndo_.push_back(std::move(cur));
            auto& next = terrainRedo_.back();
            renderer_.terrain().heightmap().heights = next.heights;
            renderer_.terrain().heightmap().InitGPU();
            renderer_.terrain().splatmap().data  = next.splat;
            renderer_.terrain().splatmap().dirty = true;
            terrainRedo_.pop_back();
        }
    }

    // Ctrl+D — duplicate
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        DuplicateSelected(db);

    // Tab — cycle to next object of same type
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        auto cycle = [&](auto& vec, int selType) {
            if (vec.empty()) return;
            int idx = -1;
            for (int i = 0; i < (int)vec.size(); ++i)
                if (vec[i].id == selectedID_) { idx = i; break; }
            idx = (idx + 1) % (int)vec.size();
            selectedID_   = vec[idx].id;
            selectedType_ = selType;
        };
        switch (selectedType_) {
        case kSelPortal:    cycle(scene_.portals,    kSelPortal);    break;
        case kSelTrigger:   cycle(scene_.triggers,   kSelTrigger);   break;
        case kSelSoundZone: cycle(scene_.soundZones, kSelSoundZone); break;
        case kSelColBox:    cycle(scene_.colBoxes,   kSelColBox);    break;
    case kSelColSphere: cycle(scene_.colSpheres, kSelColSphere); break;
        case kSelWaypoint:  cycle(scene_.waypoints,  kSelWaypoint);  break;
        case kSelNpc:       cycle(scene_.npcs,       kSelNpc);       break;
        case kSelEmitter:   cycle(scene_.emitters,   kSelEmitter);   break;
        case kSelWater:     cycle(scene_.water,      kSelWater);     break;
        case kSelScenery:   cycle(scene_.scenery,    kSelScenery);   break;
        }
    }

    // ── Gizmos: Move / Rotate / Scale ────────────────────────────────────
    // All three share hit-testing against the object's origin; only the
    // meaning of "drag along axis X" differs: translate / rotate / scale.
    glm::vec3 selPos;
    const bool haveSel = (xformMode_ != kXFormSelect && selectedID_ >= 0 &&
                          selectedType_ != kSelNone && SelectedPos(selPos));

    if (haveSel) {
        const glm::mat4 viewProj =
            cam_.Proj(vpSize_.x / std::max(vpSize_.y, 1.f)) * cam_.View();
        const glm::vec3 kAxes[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
        const float axisLen    = ZoneRenderer::GizmoAxisLength(selPos, cam_.pos);
        const float pickRadius = axisLen * 0.18f;

        // Which axes does this object support for rotate / scale?
        unsigned allowRot   = 0, allowScale = 0;
        if (selectedType_ == kSelScenery) { allowRot = 0b111; allowScale = 0b111; }
        else if (selectedType_ == kSelNpc)     { allowRot = 0b010; }
        else if (selectedType_ == kSelColBox)    { allowScale = 0b111; }
        else if (selectedType_ == kSelColSphere) { allowScale = 0b001; }   // uniform — X drives radius
        else if (selectedType_ == kSelWater)     { allowScale = 0b101; }

        // Tell the renderer to draw the active gizmo inside its forward pass
        // (so it lands on the same FBO as the rest of the scene).
        ZoneRenderer::GizmoState gz;
        gz.pos        = selPos;
        gz.axis       = gizmoAxis_;
        if (xformMode_ == kXFormMove) {
            gz.mode = ZoneRenderer::kGizmoMove;
            gz.allow_axes = 0b111;
        } else if (xformMode_ == kXFormRotate && allowRot) {
            gz.mode = ZoneRenderer::kGizmoRotate;
            gz.allow_axes = allowRot;
        } else if (xformMode_ == kXFormScale && allowScale) {
            gz.mode = ZoneRenderer::kGizmoScale;
            gz.allow_axes = allowScale;
        }
        renderer_.SetGizmo(gz);

        // Build current ray
        ImVec2 mp  = ImGui::GetMousePos();
        float  mx  = mp.x - vpOrigin_.x;
        float  my  = mp.y - vpOrigin_.y;
        float  ndcX =  (mx / vpSize_.x) * 2.f - 1.f;
        float  ndcY = -((my / vpSize_.y) * 2.f - 1.f);
        float  aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        glm::vec3 ro = cam_.pos;
        glm::vec3 rd = cam_.NDCRay(ndcX, ndcY, aspect);

        // Closest-approach `s` on axis line through selPos, plus ray↔axis dist.
        auto rayAxis = [&](const glm::vec3& ad, float& s, float& dist) {
            glm::vec3 w = ro - selPos;
            float b = glm::dot(rd, ad);
            float d = glm::dot(rd, w);
            float e = glm::dot(ad, w);
            float denom = 1.f - b * b;
            if (denom < 1e-6f) { s = 0; dist = 1e9f; return; }
            s = (e - b * d) / denom;
            float t = (b * e - d) / denom;
            dist = glm::length((ro + rd * t) - (selPos + ad * s));
        };

        // Ray-plane intersection: returns hit point. plane passes through
        // selPos with normal `n`. Falls back to selPos if ray is parallel.
        auto rayPlane = [&](const glm::vec3& n) -> glm::vec3 {
            float denom = glm::dot(rd, n);
            if (std::abs(denom) < 1e-6f) return selPos;
            float t = glm::dot(selPos - ro, n) / denom;
            return ro + rd * t;
        };

        // Signed angle of a point on the plane perpendicular to axis `a`.
        auto angleOnRing = [&](int a, const glm::vec3& pt) {
            glm::vec3 v = pt - selPos;
            // Pick the same basis used when drawing the ring.
            glm::vec3 u = (a == 1) ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
            glm::vec3 t = glm::normalize(glm::cross(kAxes[a], u));
            glm::vec3 b = glm::normalize(glm::cross(kAxes[a], t));
            return std::atan2(glm::dot(v, b), glm::dot(v, t));
        };

        // ── Press: hit-test and latch onto an axis ────────────────────────
        if (vpHovered_ && ImGui::IsMouseClicked(0) && gizmoAxis_ < 0 && !mouseLook_) {
            if (xformMode_ == kXFormMove || xformMode_ == kXFormScale) {
                int   best = -1;
                float bestDist = pickRadius;
                float bestS    = 0.f;
                unsigned allow = (xformMode_ == kXFormMove) ? 0b111u : allowScale;
                for (int a = 0; a < 3; ++a) {
                    if ((allow & (1u << a)) == 0) continue;
                    float s, d;
                    rayAxis(kAxes[a], s, d);
                    if (s >= -axisLen * 0.2f && s <= axisLen * 1.2f && d < bestDist) {
                        bestDist = d; best = a; bestS = s;
                    }
                }
                // Centre handle for uniform scale — hit if the ray passes near
                // the origin itself.
                if (xformMode_ == kXFormScale) {
                    float s0, d0; rayAxis(glm::vec3(0, 1, 0), s0, d0);
                    // Cheaper: distance ray ↔ point
                    glm::vec3 w = selPos - ro;
                    float t = glm::dot(w, rd);
                    glm::vec3 closest = ro + rd * t;
                    float d = glm::length(closest - selPos);
                    if (d < pickRadius * 0.65f) { best = 3; bestS = 0.f; }
                }
                if (best >= 0) {
                    gizmoAxis_       = best;
                    gizmoStartPos_   = selPos;
                    gizmoStartS_     = bestS;
                    gizmoPrePos_     = selPos;
                    glm::vec3 rot;   SelectedRot(rot);     gizmoStartRot_   = rot;   gizmoPreRot_ = rot;
                    glm::vec3 scl;   SelectedScale(scl);   gizmoStartScale_ = scl;   gizmoPreScale_ = scl;
                }
            } else if (xformMode_ == kXFormRotate && allowRot) {
                int   best = -1;
                float bestDist = axisLen * 0.08f;
                for (int a = 0; a < 3; ++a) {
                    if ((allowRot & (1u << a)) == 0) continue;
                    glm::vec3 hit = rayPlane(kAxes[a]);
                    float dist = std::abs(glm::length(hit - selPos) - axisLen);
                    if (dist < bestDist) { bestDist = dist; best = a; }
                }
                if (best >= 0) {
                    gizmoAxis_     = best;
                    gizmoStartS_   = angleOnRing(best, rayPlane(kAxes[best]));
                    glm::vec3 rot; SelectedRot(rot); gizmoStartRot_ = rot; gizmoPreRot_ = rot;
                }
            }
        }

        // ── Drag / release ───────────────────────────────────────────────
        if (gizmoAxis_ >= 0) {
            if (ImGui::IsMouseDown(0)) {
                if (xformMode_ == kXFormMove) {
                    float s, d;
                    rayAxis(kAxes[gizmoAxis_], s, d);
                    glm::vec3 np = gizmoStartPos_ + kAxes[gizmoAxis_] * (s - gizmoStartS_);
                    SetSelectedPos(np);
                } else if (xformMode_ == kXFormScale) {
                    if (gizmoAxis_ == 3) {
                        // Uniform: drag vertically on screen. Use mouse ΔY.
                        float dy = -ImGui::GetIO().MouseDelta.y * 0.01f;
                        glm::vec3 s = gizmoStartScale_ * (1.f + dy);
                        s = glm::max(s, glm::vec3(0.01f));
                        SetSelectedScale(s);
                        gizmoStartScale_ = s;   // accumulate on next frame
                    } else {
                        float s, d;
                        rayAxis(kAxes[gizmoAxis_], s, d);
                        float factor = (axisLen > 0.f) ? (s / axisLen) : 1.f;
                        factor = glm::clamp(factor, 0.01f, 100.f);
                        glm::vec3 ns = gizmoStartScale_;
                        ns[gizmoAxis_] = gizmoStartScale_[gizmoAxis_] * factor;
                        SetSelectedScale(ns);
                    }
                } else if (xformMode_ == kXFormRotate) {
                    glm::vec3 hit = rayPlane(kAxes[gizmoAxis_]);
                    float a = angleOnRing(gizmoAxis_, hit);
                    float delta_deg = glm::degrees(a - gizmoStartS_);
                    glm::vec3 rot = gizmoStartRot_;
                    rot[gizmoAxis_] += delta_deg;
                    SetSelectedRot(rot);
                }
            } else {
                // Release: persist + undo entry for the modified transform.
                if (xformMode_ == kXFormMove) {
                    PushUndo(kUndoMove,   selectedType_, selectedID_, gizmoPrePos_);
                    PersistSelectedPos(db);
                    std::snprintf(statusMsg_, sizeof(statusMsg_), "Moved id=%d.", selectedID_);
                } else if (xformMode_ == kXFormRotate) {
                    PushUndo(kUndoRotate, selectedType_, selectedID_, gizmoPreRot_);
                    PersistSelectedRot(db);
                    std::snprintf(statusMsg_, sizeof(statusMsg_), "Rotated id=%d.", selectedID_);
                } else if (xformMode_ == kXFormScale) {
                    PushUndo(kUndoScale,  selectedType_, selectedID_, gizmoPreScale_);
                    PersistSelectedScale(db);
                    std::snprintf(statusMsg_, sizeof(statusMsg_), "Scaled id=%d.", selectedID_);
                }
                gizmoAxis_ = -1;
            }
        }
    } else {
        gizmoAxis_ = -1;
        renderer_.SetGizmo({});   // clear — nothing to draw this frame
    }

    // Floating toolbar overlaid on top-left of viewport
    DrawFloatingToolbar();

    // Right-click context menu: "Add Object" when in select mode,
    // direct placement in any other mode.
    if (vpHovered_ && ImGui::IsMouseClicked(1) && !mouseLook_) {
        ImVec2 mp  = ImGui::GetMousePos();
        float  vpX = mp.x - vpOrigin_.x;
        float  vpY = mp.y - vpOrigin_.y;
        if (xformMode_ == kXFormSelect) {
            pendingPlacePos_ = RaycastScene(vpX, vpY);
            ImGui::OpenPopup("##vp_add_ctx");
        } else if (zoneMode_ != kModeEnviro && zoneMode_ != kModeOther && zoneMode_ != kModeTerrain) {
            PlaceObject(RaycastScene(vpX, vpY), db, media);
        }
    }

    // F key: focus camera on selected object
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_F, false))
        FocusOnSelected();

    // ── Add-object context menu ──────────────────────────────────────────
    ImGui::SetNextWindowSize({160.f, 0.f});
    if (ImGui::BeginPopup("##vp_add_ctx")) {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.6f, 0.8f, 1.f, 1.f});
        ImGui::TextUnformatted("Add object");
        ImGui::PopStyleColor();
        ImGui::Separator();

        struct AddEntry { const char* label; ZoneMode mode; };
        static const AddEntry kEntries[] = {
            {"Portal",         kModePortal     },
            {"Trigger",        kModeTrigger    },
            {"Sound Zone",     kModeSoundZone  },
            {"Collision Box",    kModeColBox     },
            {"Collision Sphere", kModeColSphere  },
            {"Waypoint",       kModeWaypoint   },
            {"NPC",            kModeNPC        },
            {"Water",          kModeWater      },
            {"Emitter",        kModeEmitters   },
            {"Scenery",        kModeScenery    },
            {"Spawn Point",    kModeSpawnPoint },
        };
        for (auto& e : kEntries) {
            if (ImGui::MenuItem(e.label)) {
                zoneMode_ = e.mode;
                PlaceObject(pendingPlacePos_, db, media);
            }
        }
        ImGui::EndPopup();
    }
}

// ─── Inspector (right panel) ──────────────────────────────────────────────────

void ZonesTab::DrawInspector(sqlite3* db, MediaTab* media) {
    if (scene_.areaName.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a zone in the\nscene panel to start.");
        return;
    }

    // When an object is selected, dispatch to its panel (always "options" mode)
    if (selectedID_ >= 0) {
        switch (selectedType_) {
        case kSelPortal:    DrawPanelPortal   (db,        false); return;
        case kSelTrigger:   DrawPanelTrigger  (db,        false); return;
        case kSelSoundZone: DrawPanelSoundZone(db,        false); return;
        case kSelColBox:    DrawPanelColBox   (db,        false); return;
        case kSelColSphere: DrawPanelColSphere(db,        false); return;
        case kSelWaypoint:  DrawPanelWaypoint (db, media, false); return;
        case kSelNpc:        DrawPanelNPC       (db, media, false); return;
        case kSelSpawnPoint: DrawPanelSpawnPoint(db, media, false); return;
        case kSelEmitter:   DrawPanelEmitters (db,        false); return;
        case kSelWater:     DrawPanelWater    (db,        false); return;
        case kSelScenery:   DrawPanelScenery  (db, media, false); return;
        default: break;
        }
    }

    // No selection: show placement panel for current mode, or zone-wide settings
    switch (zoneMode_) {
    case kModeScenery:   DrawPanelScenery  (db, media, true); break;
    case kModeTerrain:   DrawPanelTerrain  (db,        true); break;
    case kModeEmitters:  DrawPanelEmitters (db,        true); break;
    case kModeWater:     DrawPanelWater    (db,        true); break;
    case kModeColBox:    DrawPanelColBox   (db,        true); break;
    case kModeColSphere: DrawPanelColSphere(db,        true); break;
    case kModeSoundZone: DrawPanelSoundZone(db,        true); break;
    case kModeTrigger:   DrawPanelTrigger  (db,        true); break;
    case kModeWaypoint:  DrawPanelWaypoint (db, media, true); break;
    case kModePortal:    DrawPanelPortal   (db,        true); break;
    case kModeNPC:        DrawPanelNPC       (db, media, true); break;
    case kModeSpawnPoint: DrawPanelSpawnPoint(db, media, true); break;
    case kModeEnviro:     DrawPanelEnviro   (db); break;
    case kModeOther:     DrawPanelOther    (db); break;
    default:
        ImGui::Spacing();
        ImGui::TextDisabled("Right-click in the viewport\nto add an object.");
        break;
    }
}

// ─── Status bar ───────────────────────────────────────────────────────────────

void ZonesTab::DrawStatusBar() {
    ImGui::Separator();
    ImGui::SetNextItemWidth(80.f);
    ImGui::SliderFloat("##cspd", &cam_.speed, 1.f, 200.f, "spd %.0f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera speed (scroll to adjust)");
    ImGui::SameLine(0, 16.f);
    ImGui::TextDisabled("X:%.1f  Y:%.1f  Z:%.1f",
                        cam_.pos.x, cam_.pos.y, cam_.pos.z);
    ImGui::SameLine(0, 16.f);
    if (selectedID_ >= 0) {
        static const char* kSelNames[] = {
            "Portal","Trigger","Sound","ColBox","Waypoint","NPC","Emitter","Water","Scenery","SpawnPt","ColSphere"
        };
        int si = std::clamp(selectedType_, 0, 10);
        ImGui::TextColored({0.4f, 0.8f, 1.f, 0.8f},
                           "Selected: %s #%d", kSelNames[si], selectedID_);
    } else {
        ImGui::TextDisabled("Nothing selected");
    }
    ImGui::SameLine(0, 16.f);
    ImGui::TextDisabled("%s", statusMsg_);
}

// ─── Selected-object transform helpers ──────────────────────────────────────

bool ZonesTab::SelectedPos(glm::vec3& out) const {
    if (selectedID_ < 0) return false;
    auto tryVec = [&](const auto& vec, int st) -> bool {
        if (selectedType_ != st) return false;
        for (auto& o : vec) if (o.id == selectedID_) { out = o.pos; return true; }
        return false;
    };
    if (tryVec(scene_.portals,   kSelPortal))   return true;
    if (tryVec(scene_.colBoxes,   kSelColBox))    return true;
    if (tryVec(scene_.colSpheres, kSelColSphere)) return true;
    if (tryVec(scene_.waypoints, kSelWaypoint)) return true;
    if (tryVec(scene_.npcs,      kSelNpc))      return true;
    if (tryVec(scene_.emitters,  kSelEmitter))  return true;
    if (tryVec(scene_.water,     kSelWater))    return true;
    if (tryVec(scene_.scenery,     kSelScenery))    return true;
    if (tryVec(scene_.spawnPoints, kSelSpawnPoint)) return true;
    if (selectedType_ == kSelTrigger)
        for (auto& t : scene_.triggers) if (t.id == selectedID_) { out = {t.x, 0, t.z}; return true; }
    if (selectedType_ == kSelSoundZone)
        for (auto& s : scene_.soundZones) if (s.id == selectedID_) { out = {s.x, 0, s.z}; return true; }
    return false;
}

void ZonesTab::SetSelectedPos(const glm::vec3& pos) {
    auto trySet = [&](auto& vec, int st) {
        if (selectedType_ != st) return;
        for (auto& o : vec) if (o.id == selectedID_) o.pos = pos;
    };
    trySet(scene_.portals,   kSelPortal);
    trySet(scene_.colBoxes,   kSelColBox);
    trySet(scene_.colSpheres, kSelColSphere);
    trySet(scene_.waypoints, kSelWaypoint);
    trySet(scene_.npcs,      kSelNpc);
    trySet(scene_.emitters,  kSelEmitter);
    trySet(scene_.water,     kSelWater);
    trySet(scene_.scenery,     kSelScenery);
    trySet(scene_.spawnPoints, kSelSpawnPoint);
    if (selectedType_ == kSelTrigger)
        for (auto& t : scene_.triggers)
            if (t.id == selectedID_) { t.x = pos.x; t.z = pos.z; }
    if (selectedType_ == kSelSoundZone)
        for (auto& s : scene_.soundZones)
            if (s.id == selectedID_) { s.x = pos.x; s.z = pos.z; }
}

void ZonesTab::PersistSelectedPos(sqlite3* db) {
    glm::vec3 pos;
    if (!SelectedPos(pos)) return;

    const char* sql = nullptr;
    bool three = true;   // false = 2D tables (x,z only)
    switch (selectedType_) {
    case kSelPortal:    sql = "UPDATE area_portals     SET x=?,z=?     WHERE id=?"; three = false; break;
    case kSelTrigger:   sql = "UPDATE area_triggers    SET x=?,z=?     WHERE id=?"; three = false; break;
    case kSelSoundZone: sql = "UPDATE area_sound_zones SET x=?,z=?     WHERE id=?"; three = false; break;
    case kSelColBox:    sql = "UPDATE zone_colboxes    SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelColSphere: sql = "UPDATE zone_colspheres  SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelWaypoint:  sql = "UPDATE zone_waypoints   SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelNpc:       sql = "UPDATE npc_spawns       SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelEmitter:   sql = "UPDATE zone_emitters    SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelWater:     sql = "UPDATE zone_water       SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelScenery:    sql = "UPDATE zone_scenery  SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelSpawnPoint: sql = "UPDATE spawn_points  SET x=?,y=?,z=? WHERE id=?"; break;
    default: return;
    }

    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
    int idx = 1;
    sqlite3_bind_double(s, idx++, pos.x);
    if (three) sqlite3_bind_double(s, idx++, pos.y);
    sqlite3_bind_double(s, idx++, pos.z);
    sqlite3_bind_int(s, idx, selectedID_);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

// ─── Rotation helpers ───────────────────────────────────────────────────────
//
// Only scenery carries full Euler rotation. NPCs have a single yaw value —
// we return it as (0, yaw, 0) so the gizmo code can treat rotation uniformly.

bool ZonesTab::SelectedRot(glm::vec3& out) const {
    if (selectedID_ < 0) return false;
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) { out = s.rot; return true; }
    } else if (selectedType_ == kSelNpc) {
        for (auto& n : scene_.npcs) if (n.id == selectedID_) {
            out = glm::vec3(0.f, n.yaw, 0.f); return true;
        }
    }
    return false;
}

void ZonesTab::SetSelectedRot(const glm::vec3& rot) {
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) { s.rot = rot; return; }
    } else if (selectedType_ == kSelNpc) {
        for (auto& n : scene_.npcs) if (n.id == selectedID_) { n.yaw = rot.y; return; }
    }
}

void ZonesTab::PersistSelectedRot(sqlite3* db) {
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) {
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
            return;
        }
    } else if (selectedType_ == kSelNpc) {
        for (auto& n : scene_.npcs) if (n.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "UPDATE npc_spawns SET yaw=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, n.yaw);
                sqlite3_bind_int   (st, 2, n.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            return;
        }
    }
}

// ─── Scale helpers ──────────────────────────────────────────────────────────
//
// Scenery and ColBox have 3-axis scale. Water has 2-axis (XZ plane) — we
// report .y as 0 and use only x/z on write. Others don't scale — return false.

bool ZonesTab::SelectedScale(glm::vec3& out) const {
    if (selectedID_ < 0) return false;
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) { out = s.scale; return true; }
    } else if (selectedType_ == kSelColBox) {
        for (auto& c : scene_.colBoxes) if (c.id == selectedID_) { out = c.scale; return true; }
    } else if (selectedType_ == kSelColSphere) {
        for (auto& s : scene_.colSpheres) if (s.id == selectedID_) {
            out = glm::vec3(s.radius); return true;
        }
    } else if (selectedType_ == kSelWater) {
        for (auto& w : scene_.water) if (w.id == selectedID_) {
            out = glm::vec3(w.scale.x, 1.f, w.scale.y); return true;
        }
    }
    return false;
}

void ZonesTab::SetSelectedScale(const glm::vec3& s) {
    if (selectedType_ == kSelScenery) {
        for (auto& o : scene_.scenery) if (o.id == selectedID_) { o.scale = s; return; }
    } else if (selectedType_ == kSelColBox) {
        for (auto& c : scene_.colBoxes) if (c.id == selectedID_) { c.scale = s; return; }
    } else if (selectedType_ == kSelColSphere) {
        for (auto& cs : scene_.colSpheres) if (cs.id == selectedID_) { cs.radius = s.x; return; }
    } else if (selectedType_ == kSelWater) {
        for (auto& w : scene_.water) if (w.id == selectedID_) {
            w.scale.x = s.x; w.scale.y = s.z; return;
        }
    }
}

void ZonesTab::PersistSelectedScale(sqlite3* db) {
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) {
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
            return;
        }
    } else if (selectedType_ == kSelColBox) {
        for (auto& c : scene_.colBoxes) if (c.id == selectedID_) {
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
            return;
        }
    } else if (selectedType_ == kSelColSphere) {
        for (auto& cs : scene_.colSpheres) if (cs.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_colspheres SET radius=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, cs.radius);
                sqlite3_bind_int   (st, 2, cs.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            return;
        }
    } else if (selectedType_ == kSelWater) {
        for (auto& w : scene_.water) if (w.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_water SET scale_x=?,scale_z=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, w.scale.x);
                sqlite3_bind_double(st, 2, w.scale.y);
                sqlite3_bind_int   (st, 3, w.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            return;
        }
    }
}

// ─── FocusOnSelected ─────────────────────────────────────────────────────────

void ZonesTab::FocusOnSelected() {
    glm::vec3 pos;
    if (!SelectedPos(pos)) return;
    cam_.pos   = pos + glm::vec3(0.f, 12.f, 12.f);
    cam_.pitch = 30.f;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Focused on id=%d.", selectedID_);
}

// ─── Right panel implementations ──────────────────────────────────────────────

void ZonesTab::DrawPanelPortal(sqlite3* db, bool placement) {
    EnsureScriptList();

    if (placement || selectedType_ != kSelPortal) {
        // Placement panel
        ImGui::TextColored({0.2f,0.4f,1.f,1.f}, "Portal placement");
        ImGui::Separator();
        ImGui::InputTextWithHint("Name##pn", "Portal name", portalNameBuf_, sizeof(portalNameBuf_));
        ImGui::InputTextWithHint("Link area##pa", "Target area", portalLinkAreaBuf_, sizeof(portalLinkAreaBuf_));
        ImGui::InputTextWithHint("Link portal##pp", "Target portal", portalLinkNameBuf_, sizeof(portalLinkNameBuf_));
        ImGui::InputFloat("Radius##pr", &portalRadius_, 0.5f, 2.f, "%.1f");
        if (portalRadius_ < 0.5f) portalRadius_ = 0.5f;
        ImGui::Spacing();
        ImGui::TextDisabled("RMB in viewport to place portal.");
        return;
    }

    // Options panel (portal selected)
    auto it = std::find_if(scene_.portals.begin(), scene_.portals.end(),
                           [&](auto& p){ return p.id == selectedID_; });
    if (it == scene_.portals.end()) return;
    ZPortal& p = *it;

    ImGui::TextColored({0.2f,0.4f,1.f,1.f}, "Portal [id=%d]", p.id);
    ImGui::Separator();

    bool changed = false;
    char nbuf[64]; std::strncpy(nbuf, p.name.c_str(), 63);
    if (ImGui::InputText("Name##po", nbuf, 64)) { p.name = nbuf; changed = true; }

    char labuf[64]; std::strncpy(labuf, p.linkArea.c_str(), 63);
    if (ImGui::InputText("Link area##po", labuf, 64)) { p.linkArea = labuf; changed = true; }

    char lpbuf[64]; std::strncpy(lpbuf, p.linkPortal.c_str(), 63);
    if (ImGui::InputText("Link portal##po", lpbuf, 64)) { p.linkPortal = lpbuf; changed = true; }

    if (ImGui::InputFloat("Radius##po", &p.radius, 0.5f, 2.f, "%.1f")) changed = true;
    if (ImGui::InputFloat("X##pox",     &p.pos.x,  0.f,  0.f, "%.1f")) changed = true;
    if (ImGui::InputFloat("Z##poz",     &p.pos.z,  0.f,  0.f, "%.1f")) changed = true;

    // Dest coords
    ImGui::Spacing(); ImGui::TextUnformatted("Destination:");
    // Load from DB for editing
    static float destX=0, destY=0, destZ=0, destYaw=0;
    static int   lastPortalId = -1;
    if (lastPortalId != p.id) {
        lastPortalId = p.id;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db, "SELECT dest_x,dest_y,dest_z,dest_yaw FROM area_portals WHERE id=?", -1, &s, nullptr);
        sqlite3_bind_int(s, 1, p.id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            destX=(float)sqlite3_column_double(s,0); destY=(float)sqlite3_column_double(s,1);
            destZ=(float)sqlite3_column_double(s,2); destYaw=(float)sqlite3_column_double(s,3);
        }
        sqlite3_finalize(s);
    }
    bool dc = false;
    float dw = (ImGui::GetContentRegionAvail().x - 12) * 0.5f;
    ImGui::SetNextItemWidth(dw); if(ImGui::InputFloat("DX##d",   &destX,   0,0,"%.1f")) dc=true; ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if(ImGui::InputFloat("DY##d",   &destY,   0,0,"%.1f")) dc=true;
    ImGui::SetNextItemWidth(dw); if(ImGui::InputFloat("DZ##d",   &destZ,   0,0,"%.1f")) dc=true; ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if(ImGui::InputFloat("DYaw##d", &destYaw, 0,0,"%.1f")) dc=true;

    if (changed || dc) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE area_portals SET x=?,z=?,radius=?,target_area=?,dest_x=?,dest_y=?,dest_z=?,dest_yaw=? WHERE id=?",
            -1, &s, nullptr);
        sqlite3_bind_double(s,1,p.pos.x); sqlite3_bind_double(s,2,p.pos.z); sqlite3_bind_double(s,3,p.radius);
        sqlite3_bind_text(s,4,p.linkArea.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_double(s,5,destX); sqlite3_bind_double(s,6,destY); sqlite3_bind_double(s,7,destZ); sqlite3_bind_double(s,8,destYaw);
        sqlite3_bind_int(s,9,p.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete portal")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelTrigger(sqlite3* db, bool placement) {
    EnsureScriptList();
    if (placement || selectedType_ != kSelTrigger) {
        ImGui::TextColored({1.f,0.5f,0.f,1.f}, "Trigger placement");
        ImGui::Separator();
        InputScript("Script##tgp", trigScriptBuf_, sizeof(trigScriptBuf_),
                    trigFuncBuf_, sizeof(trigFuncBuf_), scriptList_);
        ImGui::InputFloat("Radius##tgr", &trigRadius_, 0.5f, 2.f, "%.1f");
        if (trigRadius_ < 0.5f) trigRadius_ = 0.5f;
        ImGui::Checkbox("Trigger once##tgo", &trigOnce_);
        ImGui::Spacing();
        ImGui::TextDisabled("RMB in viewport to place trigger.");
        return;
    }

    auto it = std::find_if(scene_.triggers.begin(), scene_.triggers.end(),
                           [&](auto& t){ return t.id == selectedID_; });
    if (it == scene_.triggers.end()) return;
    ZTrigger& t = *it;

    ImGui::TextColored({1.f,0.5f,0.f,1.f}, "Trigger [id=%d]", t.id);
    ImGui::Separator();
    bool changed = false;
    char sbuf[128]; std::strncpy(sbuf, t.script.c_str(), 127);
    char fbuf[64];  std::strncpy(fbuf, t.func.c_str(), 63);
    InputScript("Script##tgo", sbuf, 128, fbuf, 64, scriptList_);
    if (t.script != sbuf) { t.script = sbuf; changed = true; }
    if (t.func   != fbuf) { t.func   = fbuf; changed = true; }
    if (ImGui::InputFloat("X##tgox",     &t.x,      0,0,"%.1f")) changed = true;
    if (ImGui::InputFloat("Z##tgoz",     &t.z,      0,0,"%.1f")) changed = true;
    if (ImGui::InputFloat("Radius##tgor",&t.radius,  0.5f,2.f,"%.1f")) changed = true;
    if (ImGui::Checkbox("Trigger once##tgoc", &t.once)) changed = true;

    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE area_triggers SET x=?,z=?,radius=?,script=?,func=?,trigger_once=? WHERE id=?",
            -1, &s, nullptr);
        sqlite3_bind_double(s,1,t.x); sqlite3_bind_double(s,2,t.z); sqlite3_bind_double(s,3,t.radius);
        sqlite3_bind_text(s,4,t.script.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,5,t.func.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,6,t.once?1:0); sqlite3_bind_int(s,7,t.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete trigger")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelSoundZone(sqlite3* db, bool placement) {
    if (placement || selectedType_ != kSelSoundZone) {
        ImGui::TextColored({1.f,1.f,0.f,1.f}, "Sound zone placement");
        ImGui::Separator();
        ImGui::InputTextWithHint("Sound##snp", "sound file name", sndNameBuf_, sizeof(sndNameBuf_));
        ImGui::SliderInt("Volume##snv", &sndVolume_, 0, 100);
        ImGui::InputInt("Loop interval ms##snl", &sndLoopMs_);
        if (sndLoopMs_ < 0) sndLoopMs_ = 0;
        ImGui::InputFloat("Radius##snr", &sndRadius_, 0.5f, 2.f, "%.1f");
        ImGui::TextDisabled("RMB in viewport to place sound zone.");
        return;
    }

    auto it = std::find_if(scene_.soundZones.begin(), scene_.soundZones.end(),
                           [&](auto& s){ return s.id == selectedID_; });
    if (it == scene_.soundZones.end()) return;
    ZSoundZone& s = *it;

    ImGui::TextColored({1.f,1.f,0.f,1.f}, "Sound zone [id=%d]", s.id);
    ImGui::Separator();
    bool changed = false;
    char nbuf[128]; std::strncpy(nbuf, s.soundName.c_str(), 127);
    if (ImGui::InputText("Sound##sno", nbuf, 128)) { s.soundName = nbuf; changed = true; }
    if (ImGui::SliderInt("Volume##sno",    &s.volume,  0, 100)) changed = true;
    if (ImGui::InputInt ("Loop ms##sno",   &s.loopMs))           changed = true;
    if (ImGui::InputFloat("X##snox",       &s.x,       0,0,"%.1f")) changed = true;
    if (ImGui::InputFloat("Z##snoz",       &s.z,       0,0,"%.1f")) changed = true;
    if (ImGui::InputFloat("Radius##snor",  &s.radius,  0.5f,2.f,"%.1f")) changed = true;
    if (changed) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE area_sound_zones SET x=?,z=?,radius=?,sound_name=?,volume=?,loop_interval_ms=? WHERE id=?",
            -1, &stmt, nullptr);
        sqlite3_bind_double(stmt,1,s.x); sqlite3_bind_double(stmt,2,s.z); sqlite3_bind_double(stmt,3,s.radius);
        sqlite3_bind_text(stmt,4,s.soundName.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,5,s.volume); sqlite3_bind_int(stmt,6,s.loopMs); sqlite3_bind_int(stmt,7,s.id);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete sound zone")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelColBox(sqlite3* db, bool placement) {
    if (placement || selectedType_ != kSelColBox) {
        ImGui::TextColored({0.8f,0.1f,0.1f,1.f}, "Collision box");
        ImGui::Separator();
        ImGui::InputFloat("Scale X##cbp", &cbScaleX_, 0.5f, 2.f, "%.1f");
        ImGui::InputFloat("Scale Y##cbp", &cbScaleY_, 0.5f, 2.f, "%.1f");
        ImGui::InputFloat("Scale Z##cbp", &cbScaleZ_, 0.5f, 2.f, "%.1f");
        ImGui::TextDisabled("RMB in viewport to place.");
        return;
    }

    auto it = std::find_if(scene_.colBoxes.begin(), scene_.colBoxes.end(),
                           [&](auto& c){ return c.id == selectedID_; });
    if (it == scene_.colBoxes.end()) return;
    ZColBox& c = *it;

    ImGui::TextColored({0.8f,0.1f,0.1f,1.f}, "ColBox [id=%d]", c.id);
    ImGui::Separator();
    bool changed = false;
    if (ImGui::InputFloat("X##cbo",       &c.pos.x,   0,0,"%.2f")) changed = true;
    if (ImGui::InputFloat("Y##cbo",       &c.pos.y,   0,0,"%.2f")) changed = true;
    if (ImGui::InputFloat("Z##cbo",       &c.pos.z,   0,0,"%.2f")) changed = true;
    ImGui::Separator();
    if (ImGui::InputFloat("Scale X##cbo", &c.scale.x, 0.5f,2.f,"%.1f")) changed = true;
    if (ImGui::InputFloat("Scale Y##cbo", &c.scale.y, 0.5f,2.f,"%.1f")) changed = true;
    if (ImGui::InputFloat("Scale Z##cbo", &c.scale.z, 0.5f,2.f,"%.1f")) changed = true;
    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db, "UPDATE zone_colboxes SET x=?,y=?,z=?,scale_x=?,scale_y=?,scale_z=? WHERE id=?",
                           -1, &s, nullptr);
        sqlite3_bind_double(s,1,c.pos.x); sqlite3_bind_double(s,2,c.pos.y); sqlite3_bind_double(s,3,c.pos.z);
        sqlite3_bind_double(s,4,c.scale.x); sqlite3_bind_double(s,5,c.scale.y); sqlite3_bind_double(s,6,c.scale.z);
        sqlite3_bind_int(s,7,c.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete colbox")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelColSphere(sqlite3* db, bool placement) {
    if (placement || selectedType_ != kSelColSphere) {
        ImGui::TextColored({1.0f,0.45f,0.0f,1.f}, "Collision sphere");
        ImGui::Separator();
        ImGui::InputFloat("Radius##csp", &csRadius_, 0.25f, 1.f, "%.2f");
        if (csRadius_ < 0.25f) csRadius_ = 0.25f;
        ImGui::TextDisabled("RMB in viewport to place.");
        return;
    }

    auto it = std::find_if(scene_.colSpheres.begin(), scene_.colSpheres.end(),
                           [&](auto& s){ return s.id == selectedID_; });
    if (it == scene_.colSpheres.end()) return;
    ZColSphere& s = *it;

    ImGui::TextColored({1.0f,0.45f,0.0f,1.f}, "ColSphere [id=%d]", s.id);
    ImGui::Separator();
    bool changed = false;
    if (ImGui::InputFloat("X##cso", &s.pos.x, 0,0,"%.2f")) changed = true;
    if (ImGui::InputFloat("Y##cso", &s.pos.y, 0,0,"%.2f")) changed = true;
    if (ImGui::InputFloat("Z##cso", &s.pos.z, 0,0,"%.2f")) changed = true;
    ImGui::Separator();
    if (ImGui::InputFloat("Radius##cso", &s.radius, 0.25f,1.f,"%.2f")) changed = true;
    if (s.radius < 0.25f) s.radius = 0.25f;
    if (changed) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE zone_colspheres SET x=?,y=?,z=?,radius=? WHERE id=?",
            -1, &st, nullptr);
        sqlite3_bind_double(st,1,s.pos.x); sqlite3_bind_double(st,2,s.pos.y); sqlite3_bind_double(st,3,s.pos.z);
        sqlite3_bind_double(st,4,s.radius); sqlite3_bind_int(st,5,s.id);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete colsphere")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelWaypoint(sqlite3* db, MediaTab* media, bool placement) {
    EnsureScriptList();

    if (placement || selectedType_ != kSelWaypoint) {
        ImGui::TextColored({0.f,0.8f,1.f,1.f}, "Waypoint placement");
        ImGui::Separator();
        ImGui::TextDisabled("RMB in viewport to place waypoint.");
        ImGui::Spacing();
        ImGui::TextDisabled("After placing, select it to link A/B and add NPC spawns.");
        return;
    }

    auto it = std::find_if(scene_.waypoints.begin(), scene_.waypoints.end(),
                           [&](auto& w){ return w.id == selectedID_; });
    if (it == scene_.waypoints.end()) return;
    ZWaypoint& w = *it;

    ImGui::TextColored({0.f,0.8f,1.f,1.f}, "Waypoint [id=%d]", w.id);
    ImGui::Separator();

    // Position
    bool changed = false;
    float ww = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    ImGui::SetNextItemWidth(ww); if(ImGui::InputFloat("X##wpo",&w.pos.x,0,0,"%.1f"))changed=true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if(ImGui::InputFloat("Z##wpo",&w.pos.z,0,0,"%.1f"))changed=true;
    ImGui::SetNextItemWidth(ww); if(ImGui::InputFloat("Y##wpo",&w.pos.y,0,0,"%.1f"))changed=true;
    ImGui::Spacing();

    // Waypoint links
    ImGui::TextUnformatted("Navigation links:");
    auto linkName = [&](int id) -> const char* {
        static char buf[32];
        if (id < 0) return "(none)";
        std::snprintf(buf, 32, "WP #%d", id);
        return buf;
    };
    ImGui::Text("Next A: %s", linkName(w.nextA));
    ImGui::SameLine();
    if (ImGui::SmallButton("Set A")) { wpLinkMode_ = true; wpLinkB_ = false; }
    ImGui::SameLine();
    if (w.nextA >= 0 && ImGui::SmallButton("Clear A##wa")) {
        w.nextA = -1; changed = true;
    }
    ImGui::Text("Next B: %s", linkName(w.nextB));
    ImGui::SameLine();
    if (ImGui::SmallButton("Set B")) { wpLinkMode_ = true; wpLinkB_ = true; }
    ImGui::SameLine();
    if (w.nextB >= 0 && ImGui::SmallButton("Clear B##wb")) {
        w.nextB = -1; changed = true;
    }
    if (wpLinkMode_) {
        ImGui::TextColored({1.f,1.f,0.f,1.f}, "Click another waypoint in the viewport to link %s...",
                           wpLinkB_ ? "B" : "A");
        if (ImGui::Button("Cancel link")) wpLinkMode_ = false;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Behaviour:");
    if (ImGui::InputInt("Pause here (sec)##wp", &w.pauseSec)) changed = true;
    if (w.pauseSec < 0) w.pauseSec = 0;

    // NPC spawn settings
    ImGui::Spacing();
    ImGui::TextUnformatted("NPC spawn:");

    if (media) {
        const auto& defs = media->ActorDefs();
        const char* cur = "(none)";
        for (auto& d : defs) if (d.id == w.spawnActorId) { cur = d.name.c_str(); break; }
        if (ImGui::BeginCombo("Actor def##wp", cur)) {
            if (ImGui::Selectable("(none)",    w.spawnActorId == 0)) { w.spawnActorId = 0; changed = true; }
            for (auto& d : defs) {
                bool sel = d.id == w.spawnActorId;
                if (ImGui::Selectable(d.name.c_str(), sel)) { w.spawnActorId = d.id; changed = true; }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::InputInt("Spawn delay (sec)##wp",  &w.spawnDelaySec)) changed = true;
    if (ImGui::InputInt("Max spawns##wp",          &w.spawnMax))      changed = true;
    if (ImGui::InputFloat("Wander range##wp",      &w.spawnRange, 0.5f, 2.f, "%.1f")) changed = true;
    if (w.spawnDelaySec < 0) w.spawnDelaySec = 0;
    if (w.spawnMax < 1)     w.spawnMax = 1;
    if (w.spawnRange < 0)   w.spawnRange = 0;

    ImGui::Spacing();
    ImGui::TextUnformatted("Scripts:");
    char sbuf[128], fbuf[64];
    auto applyScript = [&](std::string& s, char* sb, std::string& fn, char* fb, const char* l) {
        std::strncpy(sb, s.c_str(), 127); std::strncpy(fb, fn.c_str(), 63);
        InputScript(l, sb, 128, fb, 64, scriptList_);
        if (s != sb) { s = sb; changed = true; }
        if (fn != fb) { fn = fb; changed = true; }
    };
    applyScript(w.spawnScript, sbuf, w.spawnFunc, fbuf, "Spawn##wp");
    applyScript(w.clickScript, sbuf, w.clickFunc, fbuf, "Click##wp");
    applyScript(w.deathScript, sbuf, w.deathFunc, fbuf, "Death##wp");

    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE zone_waypoints SET x=?,y=?,z=?,next_a=?,next_b=?,pause_sec=?,"
            "spawn_actor_id=?,spawn_script=?,spawn_func=?,click_script=?,click_func=?,"
            "death_script=?,death_func=?,spawn_delay_sec=?,spawn_max=?,spawn_range=? WHERE id=?",
            -1, &s, nullptr);
        sqlite3_bind_double(s,1,w.pos.x); sqlite3_bind_double(s,2,w.pos.y); sqlite3_bind_double(s,3,w.pos.z);
        sqlite3_bind_int(s,4,w.nextA);   sqlite3_bind_int(s,5,w.nextB);
        sqlite3_bind_int(s,6,w.pauseSec); sqlite3_bind_int(s,7,w.spawnActorId);
        sqlite3_bind_text(s,8,  w.spawnScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,9,  w.spawnFunc.c_str(),  -1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,10, w.clickScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,11, w.clickFunc.c_str(),  -1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,12, w.deathScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,13, w.deathFunc.c_str(),  -1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,14,w.spawnDelaySec); sqlite3_bind_int(s,15,w.spawnMax);
        sqlite3_bind_double(s,16,w.spawnRange); sqlite3_bind_int(s,17,w.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete waypoint")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelNPC(sqlite3* db, MediaTab* media, bool placement) {
    EnsureScriptList();

    static const char* kAggNames[] = {"Passive","Defensive","Aggressive","Dialog-only"};

    auto DrawNpcFields = [&](int& actorDef, char* name, char* race, char* cls,
                             int& level, int& agg, float& aggR, float& atkR, int& respMs,
                             char* spSc, char* spFn, char* clSc, char* clFn,
                             char* deSc, char* deFn, bool& changed) {
        if (media) {
            const auto& defs = media->ActorDefs();
            const char* cur = "(none)";
            for (auto& d : defs) if (d.id == actorDef) { cur = d.name.c_str(); break; }
            if (ImGui::BeginCombo("Actor def##npc", cur)) {
                if (ImGui::Selectable("(none)", actorDef == 0)) { actorDef = 0; changed = true; }
                for (auto& d : defs) {
                    bool sel = d.id == actorDef;
                    if (ImGui::Selectable(d.name.c_str(), sel)) { actorDef = d.id; changed = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputText("Name##npc",  name, 64)) changed = true;
        float hw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
        ImGui::SetNextItemWidth(hw); if(ImGui::InputText("Race##npc",  race, 64)) changed = true; ImGui::SameLine();
        ImGui::SetNextItemWidth(-1); if(ImGui::InputText("Class##npc", cls,  64)) changed = true;
        if (ImGui::InputInt("Level##npc",  &level)) { if(level<1)level=1; changed = true; }
        if (ImGui::Combo("Aggro##npc", &agg, kAggNames, 4)) changed = true;
        if (agg == 1 || agg == 2)
            if (ImGui::InputFloat("Aggro range##npc", &aggR, 0.5f,2.f,"%.1f")) changed = true;
        if (ImGui::InputFloat("Attack range##npc", &atkR, 0.5f, 2.f, "%.1f")) changed = true;
        if (ImGui::InputInt("Respawn (ms)##npc",   &respMs)) { if(respMs<0)respMs=0; changed = true; }
        ImGui::Spacing();
        ImGui::TextUnformatted("Scripts:");
        InputScript("Spawn##npc",  spSc, 128, spFn, 64, scriptList_);
        InputScript("Click##npc",  clSc, 128, clFn, 64, scriptList_);
        InputScript("Death##npc",  deSc, 128, deFn, 64, scriptList_);
    };

    if (placement || selectedType_ != kSelNpc) {
        // Auto-fill the placement form from the chosen Actor Def's defaults
        // whenever the selection changes. Copies non-empty/non-zero defaults
        // only, so the user's manual edits stick between selections.
        if (media && npcActorDefId_ != npcLastActorDefId_) {
            npcLastActorDefId_ = npcActorDefId_;
            for (const auto& d : media->ActorDefs()) {
                if (d.id != npcActorDefId_) continue;
                if (!d.default_name.empty())
                    std::strncpy(npcNameBuf_,  d.default_name.c_str(),  63);
                if (!d.default_race.empty())
                    std::strncpy(npcRaceBuf_,  d.default_race.c_str(),  63);
                if (!d.default_class.empty())
                    std::strncpy(npcClassBuf_, d.default_class.c_str(), 63);
                if (d.default_level          > 0) npcLevel_       = d.default_level;
                if (d.default_aggressiveness > 0) npcAgg_         = d.default_aggressiveness;
                if (d.default_aggro_range    > 0) npcAggroRange_  = d.default_aggro_range;
                if (d.default_attack_range   > 0) npcAtkRange_    = d.default_attack_range;
                if (d.default_respawn_ms     > 0) npcRespawnMs_   = d.default_respawn_ms;
                break;
            }
        }

        ImGui::TextColored({0.1f,0.9f,0.1f,1.f}, "NPC placement");
        ImGui::Separator();

        // ── Creature Library ────────────────────────────────────────────
        // Grid of cards for every Actor Def. Clicking one selects it for
        // placement and auto-fills the form (see logic above). Plug-and-play:
        // click card → RMB in terrain → done.
        if (media) {
            ImGui::TextColored({0.7f, 0.9f, 1.f, 1.f}, "Creature Library");
            const auto& defs = media->ActorDefs();
            if (defs.empty()) {
                ImGui::TextDisabled(
                    "No actor defs yet — go to Media tab → Actor Defs.");
            } else {
                const float avail_w = ImGui::GetContentRegionAvail().x;
                const float card_w  = std::max(80.f, (avail_w - 6.f) * 0.5f);
                const float card_h  = 46.f;
                int col = 0;
                for (const auto& d : defs) {
                    const bool sel = (d.id == npcActorDefId_);
                    if (col > 0) ImGui::SameLine(0, 4.f);
                    ImGui::PushID(d.id);
                    if (sel) {
                        ImGui::PushStyleColor(ImGuiCol_Button,
                                              {0.15f, 0.45f, 0.85f, 1.f});
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                              {0.22f, 0.55f, 0.95f, 1.f});
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                              {0.22f, 0.55f, 0.95f, 1.f});
                    }
                    char label[128];
                    std::snprintf(label, sizeof(label), "%s\nlv %d",
                                  d.name.c_str(),
                                  d.default_level > 0 ? d.default_level : 1);
                    if (ImGui::Button(label, ImVec2(card_w, card_h))) {
                        npcActorDefId_ = d.id;  // triggers auto-fill next frame
                    }
                    if (sel) ImGui::PopStyleColor(3);
                    ImGui::PopID();
                    col = (col + 1) % 2;
                }
            }
            ImGui::Spacing();
        }

        // ── Overrides (optional) — the classic form for manual tweaks ───
        if (ImGui::CollapsingHeader("Overrides")) {
            bool dummy = false;
            DrawNpcFields(npcActorDefId_, npcNameBuf_, npcRaceBuf_, npcClassBuf_,
                          npcLevel_, npcAgg_, npcAggroRange_, npcAtkRange_, npcRespawnMs_,
                          npcSpawnScript_, npcSpawnFunc_,
                          npcClickScript_, npcClickFunc_,
                          npcDeathScript_, npcDeathFunc_, dummy);
        }

        ImGui::Spacing();
        if (npcActorDefId_ > 0)
            ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f},
                               "► RMB in viewport to place.");
        else
            ImGui::TextDisabled("Pick a creature above to place.");
        return;
    }

    auto it = std::find_if(scene_.npcs.begin(), scene_.npcs.end(),
                           [&](auto& n){ return n.id == selectedID_; });
    if (it == scene_.npcs.end()) return;
    ZNpcSpawn& n = *it;

    ImGui::TextColored({0.1f,0.9f,0.1f,1.f}, "NPC [id=%d]", n.id);
    ImGui::Separator();

    bool changed = false;
    char nameBuf[64], raceBuf[64], clsBuf[64];
    std::strncpy(nameBuf, n.name.c_str(), 63);
    std::strncpy(raceBuf, n.race.c_str(), 63);
    std::strncpy(clsBuf,  n.class_.c_str(), 63);
    char spSc[128],spFn[64],clSc[128],clFn[64],deSc[128],deFn[64];
    std::strncpy(spSc,n.spawnScript.c_str(),127); std::strncpy(spFn,n.spawnFunc.c_str(),63);
    std::strncpy(clSc,n.clickScript.c_str(),127); std::strncpy(clFn,n.clickFunc.c_str(),63);
    std::strncpy(deSc,n.deathScript.c_str(),127); std::strncpy(deFn,n.deathFunc.c_str(),63);

    DrawNpcFields(n.actorDefId, nameBuf, raceBuf, clsBuf,
                  n.level, n.aggressiveness, n.aggroRange, n.attackRange, n.respawnDelayMs,
                  spSc, spFn, clSc, clFn, deSc, deFn, changed);

    if (changed) {
        n.name   = nameBuf; n.race = raceBuf; n.class_ = clsBuf;
        n.spawnScript = spSc; n.spawnFunc = spFn;
        n.clickScript = clSc; n.clickFunc = clFn;
        n.deathScript = deSc; n.deathFunc = deFn;

        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE npc_spawns SET name=?,race=?,class=?,level=?,aggressiveness=?,"
            "aggressive_range=?,attack_range=?,respawn_delay_ms=?,actor_def_id=?,"
            "spawn_script=?,spawn_func=?,click_script=?,click_func=?,"
            "death_script=?,death_func=?,x=?,y=?,z=?,yaw=? WHERE id=?",
            -1, &s, nullptr);
        sqlite3_bind_text(s,1,n.name.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,2,n.race.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,3,n.class_.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(s,4,n.level); sqlite3_bind_int(s,5,n.aggressiveness);
        sqlite3_bind_double(s,6,n.aggroRange); sqlite3_bind_double(s,7,n.attackRange);
        sqlite3_bind_int(s,8,n.respawnDelayMs); sqlite3_bind_int(s,9,n.actorDefId);
        sqlite3_bind_text(s,10,n.spawnScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,11,n.spawnFunc.c_str(),  -1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,12,n.clickScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,13,n.clickFunc.c_str(),  -1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,14,n.deathScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,15,n.deathFunc.c_str(),  -1,SQLITE_TRANSIENT);
        sqlite3_bind_double(s,16,n.pos.x); sqlite3_bind_double(s,17,n.pos.y); sqlite3_bind_double(s,18,n.pos.z);
        sqlite3_bind_double(s,19,n.yaw);   sqlite3_bind_int(s,20,n.id);
        sqlite3_step(s); sqlite3_finalize(s);

        // Re-bind the renderer in case actor_def_id changed — otherwise the
        // viewport keeps showing the old model (or nothing) until the next
        // zone reload / MediaRevision bump.
        if (media) SyncSceneryCache(media);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Position:");
    float pw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    bool pc = false;
    ImGui::SetNextItemWidth(pw); if(ImGui::InputFloat("X##npco",&n.pos.x,0,0,"%.2f"))pc=true; ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if(ImGui::InputFloat("Z##npco",&n.pos.z,0,0,"%.2f"))pc=true;
    ImGui::SetNextItemWidth(pw); if(ImGui::InputFloat("Y##npco",&n.pos.y,0,0,"%.2f"))pc=true; ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if(ImGui::InputFloat("Yaw##npco",&n.yaw,0,0,"%.1f"))pc=true;
    if (pc) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,"UPDATE npc_spawns SET x=?,y=?,z=?,yaw=? WHERE id=?", -1, &s, nullptr);
        sqlite3_bind_double(s,1,n.pos.x); sqlite3_bind_double(s,2,n.pos.y); sqlite3_bind_double(s,3,n.pos.z);
        sqlite3_bind_double(s,4,n.yaw); sqlite3_bind_int(s,5,n.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete NPC")) {
        DeleteSelected(db);
        // Evict the deleted NPC's actor from the renderer cache —
        // SyncNpcModels prunes any actors no longer referenced in scene_.npcs.
        if (media) SyncSceneryCache(media);
    }
    ImGui::PopStyleColor();
}

// ─── Scenery panel (Phase 7) ──────────────────────────────────────────────────

void ZonesTab::SyncSceneryCache(MediaTab* media) {
    if (!media) return;
    scene_.colVisDirty = true;

    // Resolve a MediaModel's material_map into Model::MaterialPaths keyed by
    // aiMaterial name, pulling texture paths from media_materials.
    // Mirrors MediaTab's buildLookups — kept here so the Zone viewport
    // matches the Media preview.
    std::unordered_map<std::string, const MediaMaterial*> matByName;
    for (const auto& mm : media->Materials()) matByName[mm.name] = &mm;

    auto buildMapping = [&](const MediaModel& mdl) {
        std::unordered_map<std::string, rco::renderer::Model::MaterialPaths> out;
        for (const auto& [ai_name, media_name] : mdl.material_map) {
            auto it = matByName.find(media_name);
            if (it == matByName.end()) continue;
            rco::renderer::Model::MaterialPaths p;
            p.albedo = it->second->albedo_path;
            p.normal = it->second->normal_path;
            p.orm    = it->second->orm_path;
            out[ai_name] = std::move(p);
        }
        return out;
    };

    std::unordered_map<int, const MediaModel*> modelById;
    for (const auto& m : media->Models()) modelById[m.id] = &m;

    // Scenery: build ModelBind (path + material_map) per model_id in use.
    std::unordered_map<int, ZoneRenderer::ModelBind> sceneryBinds;
    for (const auto& s : scene_.scenery) {
        if (sceneryBinds.count(s.modelId)) continue;
        auto it = modelById.find(s.modelId);
        if (it == modelById.end()) continue;
        ZoneRenderer::ModelBind b;
        b.file_path    = it->second->file_path;
        b.material_map = buildMapping(*it->second);
        sceneryBinds[s.modelId] = std::move(b);
    }
    renderer_.SyncSceneryModels(scene_.scenery, sceneryBinds);

    // NPCs: resolve actor_def_id → Body slot → model, then build ModelBind.
    std::unordered_map<int, MediaMaterial> matById;
    for (const auto& mm : media->Materials()) matById[mm.id] = mm;

    std::unordered_map<int, ZoneRenderer::ModelBind> npcBinds;
    for (const auto& n : scene_.npcs) {
        ZoneRenderer::ModelBind b;  // empty → placeholder cube fallback
        if (n.actorDefId > 0) {
            const ActorDef* def = nullptr;
            for (auto& d : media->ActorDefs())
                if (d.id == n.actorDefId) { def = &d; break; }
            if (def) {
                const ActorMeshSlot* bodySlot = nullptr;
                for (auto& s : def->mesh_slots)
                    if (s.slot == SlotBody) { bodySlot = &s; break; }
                if (!bodySlot && !def->mesh_slots.empty())
                    bodySlot = &def->mesh_slots.front();

                if (bodySlot) {
                    auto it = modelById.find(bodySlot->model_id);
                    if (it != modelById.end()) {
                        b.file_path    = it->second->file_path;
                        b.material_map = buildMapping(*it->second);
                    }
                    // Per-slot material override — matches Media preview's
                    // OverrideMaterial path. Applied after material_map so
                    // a single chosen material paints every submesh.
                    if (bodySlot->material_id > 0) {
                        auto mit = matById.find(bodySlot->material_id);
                        if (mit != matById.end()) {
                            const MediaMaterial& mm = mit->second;
                            b.material_override.albedo = mm.albedo_path;
                            b.material_override.normal = mm.normal_path;
                            b.material_override.orm    = mm.orm_path;
                            b.ovr_albedo_r = mm.albedo_r;
                            b.ovr_albedo_g = mm.albedo_g;
                            b.ovr_albedo_b = mm.albedo_b;
                            b.ovr_roughness = mm.roughness;
                            b.ovr_metallic  = mm.metallic;
                            b.has_override  = true;
                        }
                    }
                }
                // Actor-level scale multiplier (filhote/pai grandão). The
                // renderer multiplies this on top of each submesh's model
                // scale at draw time.
                b.actor_scale = def->scale > 0.f ? def->scale : 1.f;
            }
        }
        npcBinds[n.id] = std::move(b);
    }
    renderer_.SyncNpcModels(npcBinds);
}

void ZonesTab::DrawPanelScenery(sqlite3* db, MediaTab* media, bool placement) {
    if (placement || selectedType_ != kSelScenery) {
        ImGui::TextColored({0.75f, 0.72f, 0.68f, 1.f}, "Scenery placement");
        ImGui::Separator();

        if (media) {
            const auto& models = media->Models();
            const char* curName = "(none)";
            for (auto& m : models) if (m.id == scnModelId_) { curName = m.name.c_str(); break; }
            if (ImGui::BeginCombo("Model##scnp", curName)) {
                if (ImGui::Selectable("(none)", scnModelId_ == 0)) scnModelId_ = 0;
                for (auto& m : models) {
                    bool sel = m.id == scnModelId_;
                    if (ImGui::Selectable(m.name.c_str(), sel)) scnModelId_ = m.id;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            const auto& mats = media->Materials();
            const char* curMat = "(embedded)";
            for (auto& m : mats) if (m.id == scnMaterialId_) { curMat = m.name.c_str(); break; }
            if (ImGui::BeginCombo("Material##scnp", curMat)) {
                if (ImGui::Selectable("(embedded)", scnMaterialId_ == 0)) scnMaterialId_ = 0;
                for (auto& m : mats) {
                    bool sel = m.id == scnMaterialId_;
                    if (ImGui::Selectable(m.name.c_str(), sel)) scnMaterialId_ = m.id;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Align to ground##scnp", &scnAlignGround_);
        ImGui::Spacing();
        ImGui::TextDisabled("RMB in viewport to place.");
        return;
    }

    // Options panel (selected scenery)
    auto it = std::find_if(scene_.scenery.begin(), scene_.scenery.end(),
                           [&](auto& s){ return s.id == selectedID_; });
    if (it == scene_.scenery.end()) return;
    ZScenery& s = *it;

    ImGui::TextColored({0.75f, 0.72f, 0.68f, 1.f}, "Scenery [id=%d]", s.id);
    ImGui::Separator();

    bool changed = false;
    // Position
    float pw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("X##scno", &s.pos.x, 0,0,"%.2f")) changed=true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if (ImGui::InputFloat("Z##scno", &s.pos.z, 0,0,"%.2f")) changed=true;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Y##scno", &s.pos.y, 0,0,"%.2f")) changed=true;
    ImGui::Separator();
    // Rotation
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Pitch##scno", &s.rot.x, 1.f,5.f,"%.1f")) changed=true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if (ImGui::InputFloat("Yaw##scno",   &s.rot.y, 1.f,5.f,"%.1f")) changed=true;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Roll##scno",  &s.rot.z, 1.f,5.f,"%.1f")) changed=true;
    ImGui::Separator();
    // Scale
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Sx##scno", &s.scale.x, 0.1f,0.5f,"%.2f")) changed=true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if (ImGui::InputFloat("Sy##scno", &s.scale.y, 0.1f,0.5f,"%.2f")) changed=true;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Sz##scno", &s.scale.z, 0.1f,0.5f,"%.2f")) changed=true;
    ImGui::Separator();

    static const char* kAnimModes[] = {"None","Loop","Ping-pong","On select"};
    static const char* kColModes[]  = {"None","Sphere","Box","Polygon"};
    if (ImGui::Combo("Anim mode##scno",  &s.animMode,  kAnimModes, 4)) changed=true;
    if (ImGui::Combo("Collision##scno",  &s.collision, kColModes,  4)) changed=true;
    if (ImGui::InputInt("Inventory slots##scno", &s.invSize)) { if(s.invSize<0)s.invSize=0; changed=true; }
    if (ImGui::Checkbox("Ownable##scno", &s.ownable)) changed=true;
    if (ImGui::Checkbox("Locked##scno",  &s.locked))  changed=true;

    if (changed) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE zone_scenery SET x=?,y=?,z=?,pitch=?,yaw=?,roll=?,"
            "sx=?,sy=?,sz=?,collision=?,anim_mode=?,inv_size=?,ownable=?,locked=? WHERE id=?",
            -1, &stmt, nullptr);
        sqlite3_bind_double(stmt,1,s.pos.x);   sqlite3_bind_double(stmt,2,s.pos.y);   sqlite3_bind_double(stmt,3,s.pos.z);
        sqlite3_bind_double(stmt,4,s.rot.x);   sqlite3_bind_double(stmt,5,s.rot.y);   sqlite3_bind_double(stmt,6,s.rot.z);
        sqlite3_bind_double(stmt,7,s.scale.x); sqlite3_bind_double(stmt,8,s.scale.y); sqlite3_bind_double(stmt,9,s.scale.z);
        sqlite3_bind_int(stmt,10,s.collision); sqlite3_bind_int(stmt,11,s.animMode);
        sqlite3_bind_int(stmt,12,s.invSize);   sqlite3_bind_int(stmt,13,s.ownable?1:0);
        sqlite3_bind_int(stmt,14,s.locked?1:0); sqlite3_bind_int(stmt,15,s.id);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    ImGui::Spacing();
    if (ImGui::Button("Duplicate##scno")) DuplicateSelected(db);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete##scno")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

// ─── Terrain mode panel ──────────────────────────────────────────────────────

void ZonesTab::DrawPanelTerrain(sqlite3* db, bool) {
    if (scene_.areaName.empty()) {
        ImGui::TextColored({0.55f, 0.8f, 0.40f, 1.f}, "Terrain");
        ImGui::Separator();
        ImGui::TextDisabled("Load a zone first.");
        return;
    }

    auto& terrain = renderer_.terrain();

    // ── Brush mode ────────────────────────────────────────────────────────
    ImGui::TextColored({0.55f, 0.85f, 0.40f, 1.f}, "Brush Mode");
    ImGui::Separator();

    struct BrushDef { const char* label; const char* hint; };
    static const BrushDef kModes[] = {
        {"+ Raise",   "Sculpt terrain upward"},
        {"- Lower",   "Sculpt terrain downward"},
        {"~ Smooth",  "Smooth rough surfaces"},
        {"= Flatten", "Flatten to a target height"},
        {"# Paint",   "Paint material layers"},
        {"* Noise",   "Apply random value-noise displacement"},
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {3.f, 3.f});
    for (int i = 0; i < 6; ++i) {
        bool sel = (brushMode_ == i);
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.20f, 0.50f, 0.85f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.28f, 0.60f, 1.00f, 1.f});
        }
        ImGui::PushID(i);
        if (ImGui::Button(kModes[i].label, {-1.f, 24.f})) brushMode_ = i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kModes[i].hint);
        ImGui::PopID();
        if (sel) ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();

    // ── Radius / strength ─────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Brush Settings");

    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##rad", &brushRadius_, 1.f, 80.f, "Radius  %.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scroll wheel to resize\nShift+Scroll for strength\n[ ] keys also work");

    ImGui::SetNextItemWidth(-1.f);
    ImGui::SliderFloat("##str", &brushStrength_, 0.05f, 3.f, "Strength  %.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shift + Scroll to adjust");

    ImGui::SetNextItemWidth(-1.f);
    ImGui::Combo("Falloff##tf", &brushFalloff_, "Smooth\0Gaussian\0Linear\0Spherical\0");

    if (brushMode_ == 3) {
        ImGui::Spacing();
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(avail - (brushHoverValid_ ? 68.f : 0.f));
        ImGui::InputFloat("##flatH", &brushFlattenH_, 1.f, 10.f, "Target Y  %.1f m");
        if (brushHoverValid_) {
            ImGui::SameLine(0, 4.f);
            if (ImGui::Button("Sample", {-1.f, 0.f}))
                brushFlattenH_ = brushHitPos_.y;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Copy cursor elevation (%.2f m)", brushHitPos_.y);
        }
    }

    // ── Paint material picker ─────────────────────────────────────────────
    if (brushMode_ == 4) {
        ImGui::Spacing();
        ImGui::SeparatorText("Material Layer");

        // Per-channel accent colours (R G B A)
        static const ImVec4 kChanCol[] = {
            {0.75f, 0.28f, 0.22f, 0.90f},
            {0.22f, 0.62f, 0.28f, 0.90f},
            {0.22f, 0.42f, 0.78f, 0.90f},
            {0.72f, 0.65f, 0.12f, 0.90f},
        };
        static const ImVec4 kChanHov[] = {
            {0.90f, 0.40f, 0.35f, 1.00f},
            {0.32f, 0.80f, 0.38f, 1.00f},
            {0.32f, 0.55f, 1.00f, 1.00f},
            {0.90f, 0.82f, 0.22f, 1.00f},
        };
        const auto& mats = terrain.materials();
        float btnW = (ImGui::GetContentRegionAvail().x - 4.f) * 0.5f;
        for (int i = 0; i < 4; ++i) {
            bool  has  = (i < (int)mats.size() && !mats[i].name.empty());
            bool  sel  = (brushMaterial_ == i && has);
            const char* label = has ? mats[i].name.c_str() : "(empty)";

            if (i & 1) ImGui::SameLine(0, 4.f);

            ImVec4 bg  = has ? kChanCol[i] : ImVec4(0.18f, 0.18f, 0.18f, 0.55f);
            ImVec4 hov = has ? kChanHov[i] : ImVec4(0.28f, 0.28f, 0.28f, 0.75f);
            ImVec4 act = sel ? hov : bg;
            ImGui::PushStyleColor(ImGuiCol_Button,        act);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 1.f, 1.f, 1.f});
            ImGui::PushID(100 + i);
            if (ImGui::Button(label, {btnW, 30.f}) && has) brushMaterial_ = i;
            ImGui::PopID();
            if (sel) ImGui::PopStyleColor();
            ImGui::PopStyleColor(2);
        }
        if ((int)mats.size() < 1)
            ImGui::TextDisabled("Configure materials below.");
    }

    // ── Auto-paint by slope ───────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Auto-paint Slope")) {
        static const char* kLayerNames[] = {"Layer 0", "Layer 1", "Layer 2", "Layer 3"};

        ImGui::SetNextItemWidth(-1.f);
        ImGui::Combo("Flat layer##asl", &slopeFlatLayer_, kLayerNames, 4);
        ImGui::SetNextItemWidth(-1.f);
        ImGui::Combo("Rock layer##asl", &slopeRockLayer_, kLayerNames, 4);

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##slopeMin", &slopeMinDeg_, 0.f, 89.f, "Flat below  %.0f deg");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##slopeMax", &slopeMaxDeg_, 0.f, 89.f, "Rock above  %.0f deg");
        if (slopeMaxDeg_ <= slopeMinDeg_) slopeMaxDeg_ = slopeMinDeg_ + 1.f;

        ImGui::Spacing();
        if (ImGui::Button("Apply##asl", {-1.f, 0.f}) && terrain.Loaded()) {
            // Capture undo snapshot before the full-terrain operation
            TerrainSnapshot snap;
            snap.heights = terrain.heightmap().heights;
            snap.splat   = terrain.splatmap().data;
            if ((int)terrainUndo_.size() >= kMaxTerrainUndo)
                terrainUndo_.erase(terrainUndo_.begin());
            terrainUndo_.push_back(std::move(snap));
            terrainRedo_.clear();

            terrain.AutoPaintBySlope(slopeFlatLayer_, slopeRockLayer_,
                                     slopeMinDeg_, slopeMaxDeg_);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paints the entire terrain based on slope angle.\nCtrl+Z to undo.");
    }

    // ── Material layer configuration (DB-backed) ──────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Lazy-load material list from DB
        if (!terrainMatsLoaded_) LoadTerrainMats(db);

        // Refresh button — re-queries media_materials (useful after adding new ones)
        if (ImGui::SmallButton("Refresh##mats")) {
            terrainMatsLoaded_ = false;
            LoadTerrainMats(db);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%d materials in DB)", (int)terrainMats_.size());

        ImGui::Spacing();

        static const char* kSlotNames[] = {"Layer 0  (R)", "Layer 1  (G)", "Layer 2  (B)", "Layer 3  (A)"};
        static const ImVec4 kSlotAccent[] = {
            {0.75f, 0.28f, 0.22f, 1.f},
            {0.22f, 0.62f, 0.28f, 1.f},
            {0.22f, 0.42f, 0.78f, 1.f},
            {0.72f, 0.65f, 0.12f, 1.f},
        };

        for (int i = 0; i < EditableTerrain::kMaxMats; ++i) {
            int curId = terrain.materialId(i);

            // Find current selection index in terrainMats_
            int curIdx = -1;
            for (int j = 0; j < (int)terrainMats_.size(); ++j)
                if (terrainMats_[j].id == curId) { curIdx = j; break; }

            const char* preview = curIdx >= 0 ? terrainMats_[curIdx].name.c_str() : "(none)";

            // Coloured label
            ImGui::PushStyleColor(ImGuiCol_Text, kSlotAccent[i]);
            ImGui::TextUnformatted(kSlotNames[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine(80.f);

            ImGui::SetNextItemWidth(-1.f);
            ImGui::PushID(200 + i);
            if (ImGui::BeginCombo("##matslot", preview)) {
                // "(none)" clears the slot back to fallback colour
                if (ImGui::Selectable("(none)", curIdx < 0)) {
                    terrain.ClearMaterialSlot(i);
                }
                if (curIdx < 0) ImGui::SetItemDefaultFocus();

                for (int j = 0; j < (int)terrainMats_.size(); ++j) {
                    bool sel = (j == curIdx);
                    if (ImGui::Selectable(terrainMats_[j].name.c_str(), sel)) {
                        const auto& tm = terrainMats_[j];
                        TerrainMatSpec spec;
                        spec.media_id        = tm.id;
                        spec.name            = tm.name;
                        spec.albedo_path     = tm.albedo_path;
                        spec.normal_path     = tm.normal_path;
                        spec.roughness_path  = tm.orm_path;
                        spec.tiling          = terrain.tilings[i];
                        spec.normal_strength = tm.normal_strength;
                        terrain.SetMaterialSlot(i, spec);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Tiling per layer:");
        static const char* kTilingLabels[] = {"L0", "L1", "L2", "L3"};
        static const ImVec4 kTilingAccent[] = {
            {0.75f, 0.28f, 0.22f, 1.f},
            {0.22f, 0.62f, 0.28f, 1.f},
            {0.22f, 0.42f, 0.78f, 1.f},
            {0.72f, 0.65f, 0.12f, 1.f},
        };
        for (int i = 0; i < EditableTerrain::kMaxMats; ++i) {
            ImGui::PushStyleColor(ImGuiCol_Text, kTilingAccent[i]);
            ImGui::TextUnformatted(kTilingLabels[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine(30.f);
            ImGui::PushID(500 + i);
            ImGui::SetNextItemWidth(-1.f);
            ImGui::SliderFloat("##til", &terrain.tilings[i], 1.f, 64.f, "%.0f");
            ImGui::PopID();
        }

        if (terrainMats_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored({1.f, 0.7f, 0.2f, 1.f},
                "No materials in DB yet.");
            ImGui::TextDisabled("Add materials in the Media tab.");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Macro variation:");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##macro_str", &terrain.macroStrength, 0.f, 1.f, "Strength %.2f");
        if (ImGui::SmallButton("Generate Macro")) {
            terrain.GenerateMacro();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Save Macro")) {
            bool ok = terrain.SaveMacro();
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                ok ? "Macro texture saved." : "Failed to save macro.");
        }
    }

    // ── Save ──────────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.18f, 0.45f, 0.18f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.24f, 0.60f, 0.24f, 1.f});
    if (ImGui::Button("  Save Terrain  ", {-1.f, 0.f})) {
        bool ok = terrain.SaveArea();
        std::snprintf(statusMsg_, sizeof(statusMsg_),
            ok ? "Saved heightmap + splatmap." : "Failed to save terrain.");
    }
    ImGui::PopStyleColor(2);

    // ── Cursor info ───────────────────────────────────────────────────────
    ImGui::Spacing();
    if (brushHoverValid_) {
        ImGui::TextDisabled("%.0f, %.2f, %.0f  (h=%.2f m)",
            brushHitPos_.x, brushHitPos_.y, brushHitPos_.z, brushHitPos_.y);
    } else {
        ImGui::TextDisabled("LMB-drag to sculpt/paint");
    }
    ImGui::TextDisabled("Scroll = radius   Shift+Scroll = strength");
    ImGui::TextDisabled("[ ] also resize brush");
}

// ─── Emitters panel (Phase 9) ─────────────────────────────────────────────────

void ZonesTab::DrawPanelEmitters(sqlite3* db, bool placement) {
    static constexpr int kEmCount = 6;

    if (placement || selectedType_ != kSelEmitter) {
        ImGui::TextColored({0.8f, 1.f, 0.2f, 1.f}, "Emitter placement");
        ImGui::Separator();
        ImGui::TextUnformatted("Emitter type:");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##emtp", kEmitterNames[emtConfigIdx_])) {
            for (int i = 0; i < kEmCount; ++i) {
                bool sel = i == emtConfigIdx_;
                if (ImGui::Selectable(kEmitterNames[i], sel)) emtConfigIdx_ = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("RMB in viewport to place.");
        return;
    }

    auto it = std::find_if(scene_.emitters.begin(), scene_.emitters.end(),
                           [&](auto& e){ return e.id == selectedID_; });
    if (it == scene_.emitters.end()) return;
    ZEmitter& e = *it;

    ImGui::TextColored({0.8f,1.f,0.2f,1.f}, "Emitter [id=%d]", e.id);
    ImGui::Separator();
    bool changed = false;

    // Config picker
    int curIdx = 0;
    for (int i = 0; i < kEmCount; ++i)
        if (e.configName == kEmitterNames[i]) { curIdx = i; break; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("Type##emo", kEmitterNames[curIdx])) {
        for (int i = 0; i < kEmCount; ++i) {
            bool sel = i == curIdx;
            if (ImGui::Selectable(kEmitterNames[i], sel)) {
                e.configName = kEmitterNames[i]; changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    float pw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("X##emo",&e.pos.x,0,0,"%.2f")) changed=true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if (ImGui::InputFloat("Z##emo",&e.pos.z,0,0,"%.2f")) changed=true;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Y##emo",&e.pos.y,0,0,"%.2f")) changed=true;

    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE zone_emitters SET config_name=?,x=?,y=?,z=? WHERE id=?",
            -1, &s, nullptr);
        sqlite3_bind_text(s,1,e.configName.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_double(s,2,e.pos.x); sqlite3_bind_double(s,3,e.pos.y); sqlite3_bind_double(s,4,e.pos.z);
        sqlite3_bind_int(s,5,e.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete emitter")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

// ─── Water panel (Phase 8) ────────────────────────────────────────────────────

void ZonesTab::DrawPanelWater(sqlite3* db, bool placement) {
    if (placement || selectedType_ != kSelWater) {
        ImGui::TextColored({0.f, 0.6f, 0.9f, 1.f}, "Water placement");
        ImGui::Separator();
        ImGui::InputFloat("Scale X##wtrp", &wtrScaleX_, 1.f, 5.f, "%.0f");
        ImGui::InputFloat("Scale Z##wtrp", &wtrScaleZ_, 1.f, 5.f, "%.0f");
        float col[3] = {wtrColor_.r, wtrColor_.g, wtrColor_.b};
        if (ImGui::ColorEdit3("Color##wtrp", col)) { wtrColor_ = {col[0], col[1], col[2]}; }
        ImGui::SliderInt("Opacity##wtrp",       &wtrOpacity_, 0, 100);
        ImGui::InputInt ("Damage/s##wtrp",      &wtrDamage_);
        ImGui::Spacing();
        ImGui::TextDisabled("RMB in viewport to place water.");
        return;
    }

    auto it = std::find_if(scene_.water.begin(), scene_.water.end(),
                           [&](auto& w){ return w.id == selectedID_; });
    if (it == scene_.water.end()) return;
    ZWater& w = *it;

    ImGui::TextColored({0.f, 0.6f, 0.9f, 1.f}, "Water [id=%d]", w.id);
    ImGui::Separator();
    bool changed = false;

    float pw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("X##wtro",&w.pos.x,0,0,"%.2f")) changed=true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1); if (ImGui::InputFloat("Z##wtro",&w.pos.z,0,0,"%.2f")) changed=true;
    ImGui::SetNextItemWidth(pw); if (ImGui::InputFloat("Y##wtro",&w.pos.y,0,0,"%.2f")) changed=true;
    ImGui::Separator();
    if (ImGui::InputFloat("Scale X##wtro", &w.scale.x, 1.f,5.f,"%.0f")) changed=true;
    if (ImGui::InputFloat("Scale Z##wtro", &w.scale.y, 1.f,5.f,"%.0f")) changed=true;
    float col[3] = {w.color.r, w.color.g, w.color.b};
    if (ImGui::ColorEdit3("Color##wtro", col)) { w.color={col[0],col[1],col[2]}; changed=true; }
    if (ImGui::SliderInt("Opacity##wtro",     &w.opacity, 0,100)) changed=true;
    if (ImGui::InputInt ("Damage/s##wtro",    &w.damage))          changed=true;

    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE zone_water SET x=?,y=?,z=?,scale_x=?,scale_z=?,"
            "color_r=?,color_g=?,color_b=?,opacity=?,damage=? WHERE id=?",
            -1, &s, nullptr);
        sqlite3_bind_double(s,1,w.pos.x); sqlite3_bind_double(s,2,w.pos.y); sqlite3_bind_double(s,3,w.pos.z);
        sqlite3_bind_double(s,4,w.scale.x); sqlite3_bind_double(s,5,w.scale.y);
        sqlite3_bind_int(s,6,(int)(w.color.r*255)); sqlite3_bind_int(s,7,(int)(w.color.g*255)); sqlite3_bind_int(s,8,(int)(w.color.b*255));
        sqlite3_bind_int(s,9,w.opacity); sqlite3_bind_int(s,10,w.damage); sqlite3_bind_int(s,11,w.id);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete water")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

void ZonesTab::DrawPanelEnviro(sqlite3* db) {
    if (scene_.areaName.empty()) { ImGui::TextDisabled("Load a zone first."); return; }
    ZEnvConfig& e = scene_.env;
    ImGui::TextColored({0.9f,0.7f,0.2f,1.f}, "Environment: %s", scene_.areaName.c_str());
    ImGui::Separator();
    bool changed = false;

    static const char* kMusicNames[] = {"0 - Stop","1 - Starter Zone","2 - Forest","3 - Combat"};
    if (ImGui::Combo("Music##env", &e.musicTrack, kMusicNames, 4)) changed = true;
    if (ImGui::Checkbox("Is outdoor##env",  &e.isOutdoor))  changed = true;
    if (ImGui::Checkbox("PvP enabled##env", &e.pvpEnabled)) changed = true;
    ImGui::Separator();
    ImGui::TextUnformatted("Fog:");
    if (ImGui::InputFloat("Near##env",    &e.fogNear,  10.f, 50.f, "%.0f")) changed = true;
    if (ImGui::InputFloat("Far##env",     &e.fogFar,   10.f, 50.f, "%.0f")) changed = true;
    float fc[3] = {e.fogR, e.fogG, e.fogB};
    if (ImGui::ColorEdit3("Fog color##env", fc)) { e.fogR=fc[0];e.fogG=fc[1];e.fogB=fc[2]; changed=true; }
    ImGui::Separator();
    ImGui::TextUnformatted("Ambient light:");
    int ac[3] = {e.ambientR, e.ambientG, e.ambientB};
    if (ImGui::SliderInt3("Ambient RGB##env", ac, 0, 255)) {
        e.ambientR=ac[0]; e.ambientG=ac[1]; e.ambientB=ac[2]; changed=true;
    }
    ImGui::Separator();
    if (ImGui::InputFloat("Gravity##env",  &e.gravity,  0.05f, 0.2f, "%.2f")) changed = true;
    ImGui::Separator();
    ImGui::TextUnformatted("Weather probabilities:");
    if (ImGui::SliderInt("Rain##env",  &e.weatherRain,  0,100)) changed = true;
    if (ImGui::SliderInt("Snow##env",  &e.weatherSnow,  0,100)) changed = true;
    if (ImGui::SliderInt("Fog%%##env", &e.weatherFog,   0,100)) changed = true;
    if (ImGui::SliderInt("Storm##env", &e.weatherStorm, 0,100)) changed = true;
    if (ImGui::SliderInt("Wind##env",  &e.weatherWind,  0,100)) changed = true;

    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO area_config"
            " (name, music_track, fog_density, is_outdoor, pvp_enabled,"
            "  fog_near, fog_far, fog_r, fog_g, fog_b,"
            "  ambient_r, ambient_g, ambient_b, gravity,"
            "  weather_rain, weather_snow, weather_fog, weather_storm, weather_wind)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &s, nullptr);
        sqlite3_bind_text(s, 1, e.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s,2,e.musicTrack); sqlite3_bind_double(s,3,e.fogDensity);
        sqlite3_bind_int(s,4,e.isOutdoor?1:0); sqlite3_bind_int(s,5,e.pvpEnabled?1:0);
        sqlite3_bind_double(s,6,e.fogNear); sqlite3_bind_double(s,7,e.fogFar);
        sqlite3_bind_double(s,8,e.fogR); sqlite3_bind_double(s,9,e.fogG); sqlite3_bind_double(s,10,e.fogB);
        sqlite3_bind_int(s,11,e.ambientR); sqlite3_bind_int(s,12,e.ambientG); sqlite3_bind_int(s,13,e.ambientB);
        sqlite3_bind_double(s,14,e.gravity);
        sqlite3_bind_int(s,15,e.weatherRain); sqlite3_bind_int(s,16,e.weatherSnow);
        sqlite3_bind_int(s,17,e.weatherFog); sqlite3_bind_int(s,18,e.weatherStorm); sqlite3_bind_int(s,19,e.weatherWind);
        sqlite3_step(s); sqlite3_finalize(s);
    }
}

// ─── DrawPanelSpawnPoint ─────────────────────────────────────────────────────

void ZonesTab::DrawPanelSpawnPoint(sqlite3* db, MediaTab* media, bool placement) {
    if (scene_.areaName.empty()) { ImGui::TextDisabled("Load a zone first."); return; }

    static const char* kAggNames[] = {"Passive","Defensive","Aggressive","Dialog-only"};

    if (placement) {
        // Placement mode: show radius config + instructions.
        ImGui::SeparatorText("New Spawn Point");
        ImGui::SliderFloat("Radius##sp", &spawnPtRadius_, 1.f, 50.f, "%.1f");
        ImGui::Spacing();
        ImGui::TextWrapped("Right-click the viewport and choose\n"
                           "'Spawn Point' from the menu to place.\n"
                           "Each spawn point defines a group of mobs\n"
                           "that respawn together within the radius.");
        return;
    }

    // Editing mode: find the selected spawn point.
    ZSpawnPoint* sp = nullptr;
    for (auto& p : scene_.spawnPoints) if (p.id == selectedID_) { sp = &p; break; }
    if (!sp) { ImGui::TextDisabled("No spawn point selected."); return; }

    // ── Point fields ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Spawn Point");
    {
        char nameBuf[128];
        std::strncpy(nameBuf, sp->name.c_str(), sizeof(nameBuf)-1); nameBuf[sizeof(nameBuf)-1] = 0;
        if (ImGui::InputText("Name##sp", nameBuf, sizeof(nameBuf))) {
            sp->name = nameBuf;
            sqlite3_stmt* s = nullptr;
            sqlite3_prepare_v2(db, "UPDATE spawn_points SET name=? WHERE id=?", -1, &s, nullptr);
            sqlite3_bind_text(s, 1, sp->name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, sp->id);
            sqlite3_step(s); sqlite3_finalize(s);
        }
        if (ImGui::SliderFloat("Radius##sp", &sp->radius, 1.f, 50.f, "%.1f")) {
            sqlite3_stmt* s = nullptr;
            sqlite3_prepare_v2(db, "UPDATE spawn_points SET radius=? WHERE id=?", -1, &s, nullptr);
            sqlite3_bind_double(s, 1, sp->radius);
            sqlite3_bind_int(s, 2, sp->id);
            sqlite3_step(s); sqlite3_finalize(s);
        }
    }
    ImGui::Text("Position: (%.1f, %.1f, %.1f)", sp->pos.x, sp->pos.y, sp->pos.z);
    ImGui::Text("Mobs: %d  |  ID: %d", (int)sp->mobs.size(), sp->id);

    // ── Mob list ────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Mobs");
    if (ImGui::Button("+ Add Mob##sp")) {
        ZSpawnPointMob nm;
        nm.name  = "NPC";
        nm.race  = "Human";
        nm.class_ = "Warrior";
        nm.count = 1;
        nm.aggressiveness = 2;

        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO spawn_point_mobs"
            " (spawn_point_id, actor_def_id, mob_count, name, race, class,"
            "  level, aggressiveness, aggressive_range, attack_range, respawn_delay_ms)"
            " VALUES (?,0,1,'NPC','Human','Warrior',1,2,8.0,2.0,30000)",
            -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, sp->id);
            sqlite3_step(s);
            nm.id = (int)sqlite3_last_insert_rowid(db);
            sqlite3_finalize(s);
            sp->mobs.push_back(nm);
            spawnPtSelMob_ = (int)sp->mobs.size() - 1;
        }
    }

    ImGui::BeginChild("##moblist_sp", {0, 120}, true);
    for (int mi = 0; mi < (int)sp->mobs.size(); ++mi) {
        auto& m = sp->mobs[mi];
        const char* defName = "(none)";
        if (media)
            for (auto& d : media->ActorDefs())
                if (d.id == m.actor_def_id) { defName = d.name.c_str(); break; }
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "[%d] %s x%d  lv%d  %s##msp%d",
            m.id, m.name.c_str(), m.count, m.level, defName, mi);
        if (ImGui::Selectable(lbl, spawnPtSelMob_ == mi))
            spawnPtSelMob_ = mi;
    }
    ImGui::EndChild();

    // ── Selected mob editor ─────────────────────────────────────────────────
    if (spawnPtSelMob_ >= 0 && spawnPtSelMob_ < (int)sp->mobs.size()) {
        auto& m = sp->mobs[spawnPtSelMob_];
        ImGui::SeparatorText("Edit Mob");
        bool mdirty = false;

        // Actor Def picker
        if (media) {
            auto& defs = media->ActorDefs();
            const char* cur = "(none)";
            for (auto& d : defs) if (d.id == m.actor_def_id) { cur = d.name.c_str(); break; }
            if (ImGui::BeginCombo("Actor Def##msp", cur)) {
                if (ImGui::Selectable("(none)", m.actor_def_id == 0)) { m.actor_def_id = 0; mdirty = true; }
                for (auto& d : defs) {
                    bool sel = (d.id == m.actor_def_id);
                    if (ImGui::Selectable(d.name.c_str(), sel)) { m.actor_def_id = d.id; mdirty = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        char nbuf[64]; std::strncpy(nbuf, m.name.c_str(), 63); nbuf[63] = 0;
        if (ImGui::InputText("Name##msp", nbuf, 64)) { m.name = nbuf; mdirty = true; }
        char rbuf[32]; std::strncpy(rbuf, m.race.c_str(), 31); rbuf[31] = 0;
        char cbuf[32]; std::strncpy(cbuf, m.class_.c_str(), 31); cbuf[31] = 0;
        float hw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
        ImGui::SetNextItemWidth(hw);
        if (ImGui::InputText("Race##msp",  rbuf, 32)) { m.race  = rbuf; mdirty = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Class##msp", cbuf, 32)) { m.class_ = cbuf; mdirty = true; }
        if (ImGui::InputInt("Level##msp",  &m.level))  { if (m.level < 1) m.level = 1; mdirty = true; }
        if (ImGui::InputInt("Count##msp",  &m.count))  { if (m.count < 1) m.count = 1; mdirty = true; }
        if (ImGui::Combo("Aggro##msp", &m.aggressiveness, kAggNames, 4)) mdirty = true;
        if (ImGui::InputFloat("Aggro Range##msp",  &m.aggressive_range, 0.5f, 5.f, "%.1f")) mdirty = true;
        if (ImGui::InputFloat("Attack Range##msp", &m.attack_range,     0.5f, 5.f, "%.1f")) mdirty = true;
        if (ImGui::InputInt("Respawn (ms)##msp", &m.respawn_delay_ms))  {
            if (m.respawn_delay_ms < 0) m.respawn_delay_ms = 0; mdirty = true;
        }

        if (mdirty) {
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE spawn_point_mobs SET actor_def_id=?, mob_count=?, name=?, race=?,"
                " class=?, level=?, aggressiveness=?, aggressive_range=?, attack_range=?,"
                " respawn_delay_ms=? WHERE id=?",
                -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(s,    1, m.actor_def_id);
                sqlite3_bind_int(s,    2, m.count);
                sqlite3_bind_text(s,   3, m.name.c_str(),   -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s,   4, m.race.c_str(),   -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s,   5, m.class_.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(s,    6, m.level);
                sqlite3_bind_int(s,    7, m.aggressiveness);
                sqlite3_bind_double(s, 8, m.aggressive_range);
                sqlite3_bind_double(s, 9, m.attack_range);
                sqlite3_bind_int(s,   10, m.respawn_delay_ms);
                sqlite3_bind_int(s,   11, m.id);
                sqlite3_step(s); sqlite3_finalize(s);
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Remove Mob##msp")) {
            char sql[64];
            std::snprintf(sql, sizeof(sql), "DELETE FROM spawn_point_mobs WHERE id=%d", m.id);
            sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
            sp->mobs.erase(sp->mobs.begin() + spawnPtSelMob_);
            spawnPtSelMob_ = -1;
        }
    }
}

void ZonesTab::DrawPanelOther(sqlite3* db) {
    if (scene_.areaName.empty()) { ImGui::TextDisabled("Load a zone first."); return; }
    ZEnvConfig& e = scene_.env;
    ImGui::TextColored({0.7f,0.7f,0.7f,1.f}, "Other options: %s", scene_.areaName.c_str());
    ImGui::Separator();
    EnsureScriptList();
    bool changed = false;
    char es[128], ef[64], xs[128], xf[64];
    std::strncpy(es, e.entryScript.c_str(), 127); std::strncpy(ef, "", 63);
    std::strncpy(xs, e.exitScript.c_str(), 127);  std::strncpy(xf, "", 63);
    InputScript("Entry script##ot", es, 128, ef, 64, scriptList_);
    if (e.entryScript != es) { e.entryScript = es; changed = true; }
    InputScript("Exit script##ot",  xs, 128, xf, 64, scriptList_);
    if (e.exitScript  != xs) { e.exitScript  = xs; changed = true; }

    if (changed) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE area_config SET entry_script=?, exit_script=? WHERE name=?",
            -1, &s, nullptr);
        sqlite3_bind_text(s,1,e.entryScript.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,2,e.exitScript.c_str(), -1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,3,e.name.c_str(),       -1,SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);
    }
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
    }

    // ── Top bar ────────────────────────────────────────────────────────────
    DrawTopBar(db);
    ImGui::Separator();

    float contentH = ImGui::GetContentRegionAvail().y - kStatusBarH - 6.f;
    if (contentH < 64.f) contentH = 64.f;

    // ── Three-column body ─────────────────────────────────────────────────
    // [Scene hierarchy | Viewport | Inspector]

    // Left: scene hierarchy
    ImGui::BeginChild("##z_scene", {kSidebarW, contentH}, true);
    DrawSceneSidebar(db, media);
    ImGui::EndChild();
    ImGui::SameLine(0, 2.f);

    // Center: viewport (takes remaining width minus inspector)
    float vpW = ImGui::GetContentRegionAvail().x - kInspectorW - 2.f;
    if (vpW < 64.f) vpW = 64.f;
    ImGui::BeginChild("##z_vp", {vpW, contentH}, false);
    DrawViewport(db, media);
    ImGui::EndChild();
    ImGui::SameLine(0, 2.f);

    // Right: inspector
    ImGui::BeginChild("##z_insp", {0.f, contentH}, true);
    DrawInspector(db, media);
    ImGui::EndChild();

    // ── Status bar ─────────────────────────────────────────────────────────
    DrawStatusBar();
}

} // namespace gue
