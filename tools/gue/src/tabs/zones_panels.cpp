#include "zones.h"
#include "media.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <unordered_map>

// InputScript is a file-local helper shared between zones.cpp and zones_panels.cpp.
// Both define it as static so the linker sees two identical copies — one per TU.
static void InputScript(const char* label, char* scriptBuf, int sbLen,
                        char* funcBuf, int fbLen,
                        const std::vector<std::string>& scripts) {
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
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    char funcLabel[64];
    std::snprintf(funcLabel, sizeof(funcLabel), "##f%s", label);
    ImGui::InputTextWithHint(funcLabel, "function", funcBuf, fbLen);
}

// Small "..." button placed right after a folder InputText/InputTextWithHint
// (same line) that opens a popup listing already-used scenery folders so the
// user isn't stuck typing an exact name from memory. Selecting an entry
// overwrites buf; "(root / ungrouped)" clears it. Returns true if buf changed.
static bool FolderPickerButton(const char* popupId, char* buf, size_t bufSize,
                               const std::vector<std::string>& existing) {
    bool changed = false;
    ImGui::SameLine(0, 4.f);
    if (ImGui::SmallButton("...")) ImGui::OpenPopup(popupId);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick from existing folders");
    if (ImGui::BeginPopup(popupId)) {
        if (ImGui::Selectable("(root / ungrouped)")) {
            buf[0] = 0;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        if (existing.empty()) {
            ImGui::TextDisabled("No folders yet — type a new name\nin the field, it's created on placement.");
        } else {
            ImGui::Separator();
            for (const auto& f : existing) {
                if (ImGui::Selectable(f.c_str())) {
                    std::strncpy(buf, f.c_str(), bufSize - 1);
                    buf[bufSize - 1] = 0;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }
    return changed;
}

namespace gue {

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
    std::unordered_map<int, const MediaMaterial*> matByIdForCutout;
    for (const auto& mm : media->Materials()) matByIdForCutout[mm.id] = &mm;

    // Scenery: build ModelBind (path + material_map) per model_id in use.
    // black_cutout mirrors the server's LoadWorldObjects rule (model OR the
    // scenery instance's override material) — see db.go. ModelBind is keyed
    // per model_id though, so this uses the first scenery instance found for
    // that model, same limitation buildMapping already has for material_map.
    std::unordered_map<int, ZoneRenderer::ModelBind> sceneryBinds;
    for (const auto& s : scene_.scenery) {
        if (sceneryBinds.count(s.modelId)) continue;
        auto it = modelById.find(s.modelId);
        if (it == modelById.end()) continue;
        ZoneRenderer::ModelBind b;
        b.file_path    = it->second->file_path;
        b.material_map = buildMapping(*it->second);
        b.black_cutout = it->second->black_cutout;
        if (s.materialId != 0) {
            auto mit = matByIdForCutout.find(s.materialId);
            if (mit != matByIdForCutout.end() && mit->second->black_cutout)
                b.black_cutout = true;
        }
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
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 32.f);
        ImGui::InputTextWithHint("##scnfolder", "Folder (optional, e.g. \"Village Props\")",
                                 scnFolder_, sizeof(scnFolder_));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("New placements are tagged with this folder for organization\n"
                              "(scene sidebar, bulk select/move/delete, Foliage Brush erase mask).");
        FolderPickerButton("scnfolderpick", scnFolder_, sizeof(scnFolder_), DistinctSceneryFolders());
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
    static const char* kColModes[]  = {"None","Sphere","Box/Wedge","Polygon"};
    if (ImGui::Combo("Anim mode##scno",  &s.animMode,  kAnimModes, 4)) changed=true;
    if (ImGui::Combo("Collision##scno",  &s.collision, kColModes,  4)) changed=true;
    if (ImGui::InputInt("Inventory slots##scno", &s.invSize)) { if(s.invSize<0)s.invSize=0; changed=true; }
    if (ImGui::Checkbox("Ownable##scno", &s.ownable)) changed=true;
    if (ImGui::Checkbox("Locked##scno",  &s.locked))  changed=true;
    ImGui::Separator();
    {
        char folderBuf[128];
        std::strncpy(folderBuf, s.folder.c_str(), sizeof(folderBuf) - 1);
        folderBuf[sizeof(folderBuf) - 1] = 0;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 32.f);
        if (ImGui::InputText("Folder##scno", folderBuf, sizeof(folderBuf))) {
            s.folder = folderBuf;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Move this instance to a different organizational folder.\n"
                              "Empty = ungrouped/root. Use \"/\" to nest.");
        if (FolderPickerButton("scnofolderpick", folderBuf, sizeof(folderBuf), DistinctSceneryFolders())) {
            s.folder = folderBuf;
            changed = true;
        }
    }

    if (changed) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE zone_scenery SET x=?,y=?,z=?,pitch=?,yaw=?,roll=?,"
            "sx=?,sy=?,sz=?,collision=?,anim_mode=?,inv_size=?,ownable=?,locked=?,folder=? WHERE id=?",
            -1, &stmt, nullptr);
        sqlite3_bind_double(stmt,1,s.pos.x);   sqlite3_bind_double(stmt,2,s.pos.y);   sqlite3_bind_double(stmt,3,s.pos.z);
        sqlite3_bind_double(stmt,4,s.rot.x);   sqlite3_bind_double(stmt,5,s.rot.y);   sqlite3_bind_double(stmt,6,s.rot.z);
        sqlite3_bind_double(stmt,7,s.scale.x); sqlite3_bind_double(stmt,8,s.scale.y); sqlite3_bind_double(stmt,9,s.scale.z);
        sqlite3_bind_int(stmt,10,s.collision); sqlite3_bind_int(stmt,11,s.animMode);
        sqlite3_bind_int(stmt,12,s.invSize);   sqlite3_bind_int(stmt,13,s.ownable?1:0);
        sqlite3_bind_int(stmt,14,s.locked?1:0);
        sqlite3_bind_text(stmt,15,s.folder.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,16,s.id);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
        scene_.colVisDirty = true;
    }
    ImGui::Spacing();
    if (ImGui::Button("Duplicate##scno")) DuplicateSelected(db, media);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,{0.65f,0.1f,0.1f,1.f});
    if (ImGui::Button("Delete##scno")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

// Shown instead of DrawPanelScenery when 2+ scenery are selected together
// (e.g. clicking a folder header in the sidebar tree, or Ctrl/Shift-clicking
// several). Answers "move everything I just selected into a folder" without
// having to edit each instance's Folder field one at a time.
void ZonesTab::DrawPanelSceneryBulk(sqlite3* db, MediaTab* media, const std::vector<SelectionRef>& sel) {
    ImGui::TextColored({0.75f, 0.72f, 0.68f, 1.f}, "Scenery — %d selected", (int)sel.size());
    ImGui::Separator();

    static char bulkFolderBuf[128] = {};
    ImGui::TextUnformatted("Move to folder:");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 32.f);
    ImGui::InputTextWithHint("##scnbulkfolder", "e.g. \"Village Props\" (empty = root)",
                             bulkFolderBuf, sizeof(bulkFolderBuf));
    FolderPickerButton("scnbulkfolderpick", bulkFolderBuf, sizeof(bulkFolderBuf), DistinctSceneryFolders());
    if (ImGui::Button("Move Selected##scnbulk", {-1.f, 0.f})) {
        std::vector<int> ids;
        ids.reserve(sel.size());
        for (const auto& ref : sel) if (ref.type == kSelScenery) ids.push_back(ref.id);
        MoveSceneryToFolder(db, ids, bulkFolderBuf);
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Duplicate All##scnbulk")) DuplicateSelected(db, media);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
    if (ImGui::Button("Delete All##scnbulk")) DeleteSelected(db);
    ImGui::PopStyleColor();
}

// ─── Foliage brush panel ─────────────────────────────────────────────────────
// Scatter-paints trees/bushes/rocks as ordinary zone_scenery rows. The model
// palette (the "set" of models the brush picks randomly from, for variation)
// can be built two ways: the searchable checklist right here, or by clicking
// tiles in the Asset Browser at the bottom of the Zones tab while this mode
// is active (see the DrawTile lambda in zones.cpp) — both write/read the same
// foliageModelIds_ vector. Painted instances are regular ZScenery, so they're
// selectable/editable/deletable via the normal Scenery panel + gizmo.
void ZonesTab::DrawPanelFoliage(sqlite3* db, MediaTab* media, bool placement) {
    (void)db;
    if (!placement) return;

    ImGui::TextColored({0.55f, 0.85f, 0.40f, 1.f}, "Foliage Brush");
    ImGui::Separator();

    ImGui::Text("Palette (%d model%s)", (int)foliageModelIds_.size(),
                foliageModelIds_.size() == 1 ? "" : "s");
    if (!foliageModelIds_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##folpal")) foliageModelIds_.clear();
    }

    // Current palette — one row per model with a quick remove button, so the
    // "set of meshes for variation" is visible without leaving this panel.
    if (media && !foliageModelIds_.empty()) {
        const auto& models = media->Models();
        ImGui::BeginChild("##folpalette", {0.f, std::min(90.f, foliageModelIds_.size() * 22.f + 4.f)}, true);
        int removeId = -1;
        for (int id : foliageModelIds_) {
            const char* name = "(unknown)";
            for (auto& m : models) if (m.id == id) { name = m.name.c_str(); break; }
            ImGui::PushID(id);
            if (ImGui::SmallButton("x")) removeId = id;
            ImGui::SameLine();
            ImGui::TextUnformatted(name);
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (removeId >= 0) {
            foliageModelIds_.erase(std::remove(foliageModelIds_.begin(), foliageModelIds_.end(), removeId),
                                   foliageModelIds_.end());
        }
    } else if (foliageModelIds_.empty()) {
        ImGui::TextColored({1.f, 0.65f, 0.3f, 1.f}, "No models selected.");
    }
    ImGui::Spacing();

    // Searchable add-list — click a name to toggle it in/out of the palette.
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##folfilt", "Search models to add...",
                             foliageFilter_, sizeof(foliageFilter_));
    if (media) {
        const auto& models = media->Models();
        std::string filt = foliageFilter_;
        for (char& c : filt) c = (char)std::tolower((unsigned char)c);

        ImGui::BeginChild("##folpick", {0.f, 150.f}, true);
        for (auto& m : models) {
            if (!filt.empty()) {
                std::string nm = m.name;
                for (char& c : nm) c = (char)std::tolower((unsigned char)c);
                if (nm.find(filt) == std::string::npos) continue;
            }
            bool sel = std::find(foliageModelIds_.begin(), foliageModelIds_.end(), m.id)
                       != foliageModelIds_.end();
            ImGui::PushID(m.id);
            if (ImGui::Checkbox("##folchk", &sel)) {
                if (sel) foliageModelIds_.push_back(m.id);
                else foliageModelIds_.erase(
                    std::remove(foliageModelIds_.begin(), foliageModelIds_.end(), m.id),
                    foliageModelIds_.end());
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::TextDisabled("Tip: tiles in the Asset Browser below also toggle the palette.");
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::SliderFloat("Radius##fol",       &foliageRadius_,     1.f,  60.f, "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint circle radius. [ and ] also resize while hovering the viewport.");
    ImGui::SliderFloat("Density##fol",      &foliageDensity_,    0.02f, 3.f,  "%.2f /m²/s");
    ImGui::SliderFloat("Min Spacing##fol",  &foliageMinSpacing_, 0.1f, 10.f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reject a new instance if closer than this to any existing scenery.");
    ImGui::Spacing();

    float pw = (ImGui::GetContentRegionAvail().x - 8) * 0.5f;
    ImGui::SetNextItemWidth(pw);
    ImGui::DragFloat("Min Scale##fol", &foliageMinScale_, 0.01f, 0.05f, foliageMaxScale_, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("Max Scale##fol", &foliageMaxScale_, 0.01f, foliageMinScale_, 5.f, "%.2f");
    if (foliageMinScale_ > foliageMaxScale_) foliageMinScale_ = foliageMaxScale_;

    ImGui::Checkbox("Random yaw##fol", &foliageRandomYaw_);
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 32.f);
    ImGui::InputTextWithHint("##folfolder", "Folder (optional, e.g. \"Forest North\")",
                             foliageFolder_, sizeof(foliageFolder_));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("New painted instances are tagged with this folder.\n"
                          "Also the erase mask target below.");
    FolderPickerButton("folfolderpick", foliageFolder_, sizeof(foliageFolder_), DistinctSceneryFolders());

    ImGui::TextUnformatted("Erase mask (Shift+LMB):");
    ImGui::RadioButton("Palette##folerasemode", &foliageEraseMode_, kFoliageErasePalette);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only erases instances of the checked palette models above.\n"
                          "If nothing is checked, erase does nothing — switch mode instead.");
    ImGui::SameLine();
    ImGui::RadioButton("Folder##folerasemode", &foliageEraseMode_, kFoliageEraseFolder);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Erases ANY model whose folder matches the field above\n"
                          "(acts as a mask, ignores the palette — e.g. clear everything\n"
                          "in \"Forest North\" regardless of which models are in it).");
    ImGui::SameLine();
    ImGui::RadioButton("Any##folerasemode", &foliageEraseMode_, kFoliageEraseAny);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("No mask at all — erases every scenery instance in the brush\n"
                          "radius, regardless of model or folder. Use this if Palette/Folder\n"
                          "modes don't seem to do anything.");
    ImGui::Spacing();
    ImGui::Separator();

    static const char* kEraseHint[3] = {
        "LMB: paint    Shift+LMB: erase (checked palette models only)",
        "LMB: paint    Shift+LMB: erase (folder mask, all models)",
        "LMB: paint    Shift+LMB: erase (ANY scenery in radius)",
    };
    ImGui::TextDisabled("%s", kEraseHint[std::clamp(foliageEraseMode_, 0, 2)]);
    ImGui::TextDisabled("Ground snap is always on — instances sit on terrain.");
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

// ─── DrawPanelPlayerSpawn ─────────────────────────────────────────────────────

void ZonesTab::DrawPanelPlayerSpawn(sqlite3* db, bool placement) {
    if (scene_.areaName.empty()) { ImGui::TextDisabled("Load a zone first."); return; }

    if (placement) {
        ImGui::SeparatorText("New Player Spawn");
        ImGui::TextWrapped("Right-click the viewport and choose\n"
                           "'Player Spawn' to place a spawn point.\n"
                           "Assign it to a playable actor def via\n"
                           "Media > Actor Defs > Initial Spawn.");
        return;
    }

    ZPlayerSpawn* ps = nullptr;
    for (auto& p : scene_.playerSpawns) if (p.id == selectedID_) { ps = &p; break; }
    if (!ps) { ImGui::TextDisabled("No player spawn selected."); return; }

    ImGui::SeparatorText("Player Spawn");
    {
        char nameBuf[128];
        std::strncpy(nameBuf, ps->name.c_str(), sizeof(nameBuf)-1);
        nameBuf[sizeof(nameBuf)-1] = 0;
        if (ImGui::InputText("Name##psp", nameBuf, sizeof(nameBuf))) {
            ps->name = nameBuf;
            sqlite3_stmt* s = nullptr;
            sqlite3_prepare_v2(db, "UPDATE player_spawns SET name=? WHERE id=?", -1, &s, nullptr);
            sqlite3_bind_text(s, 1, ps->name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (s, 2, ps->id);
            sqlite3_step(s); sqlite3_finalize(s);
        }
        if (ImGui::SliderFloat("Yaw##psp", &ps->yaw, -180.f, 180.f, "%.1f deg")) {
            sqlite3_stmt* s = nullptr;
            sqlite3_prepare_v2(db, "UPDATE player_spawns SET yaw=? WHERE id=?", -1, &s, nullptr);
            sqlite3_bind_double(s, 1, ps->yaw);
            sqlite3_bind_int   (s, 2, ps->id);
            sqlite3_step(s); sqlite3_finalize(s);
        }
    }
    ImGui::Text("Position: (%.1f, %.1f, %.1f)", ps->pos.x, ps->pos.y, ps->pos.z);
    ImGui::Text("ID: %d", ps->id);
    ImGui::Spacing();
    ImGui::TextDisabled("Assign via Media > Actor Defs > Initial Spawn.");
}

} // namespace gue
