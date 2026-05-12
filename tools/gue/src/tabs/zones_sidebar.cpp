#include "zones.h"
#include "media.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace gue {

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
        "Enviro","Other","SpawnPt","ColSphere"
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
    // Scenery: use model name instead of bare id.
    if (!scene_.scenery.empty()) {
        // Build modelId → base name map from media.
        std::unordered_map<int, std::string> modelNames;
        if (media) {
            for (const auto& m : media->Models()) {
                // Use only the part after the last '/' for brevity.
                const char* base = std::strrchr(m.name.c_str(), '/');
                modelNames[m.id] = base ? base + 1 : m.name;
            }
        }

        ImVec4 col = {0.80f, 0.78f, 0.75f, 1.f};
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "M Scenery (%d)##grpScenery",
                      (int)scene_.scenery.size());
        bool open = ImGui::TreeNodeEx(hdr,
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::PopStyleColor();
        if (open) {
            for (const auto& obj : scene_.scenery) {
                bool sel = (selectedType_ == kSelScenery && selectedID_ == obj.id);
                ImGuiTreeNodeFlags flags =
                    ImGuiTreeNodeFlags_Leaf |
                    ImGuiTreeNodeFlags_NoTreePushOnOpen |
                    ImGuiTreeNodeFlags_SpanFullWidth;
                if (sel) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                    ImGui::PushStyleColor(ImGuiCol_Header,
                                         {0.25f, 0.55f, 0.90f, 0.60f});
                }
                char lbl[96];
                auto it = modelNames.find(obj.modelId);
                if (it != modelNames.end())
                    std::snprintf(lbl, sizeof(lbl), "%s #%d",
                                  it->second.c_str(), obj.id);
                else
                    std::snprintf(lbl, sizeof(lbl), "model%d #%d",
                                  obj.modelId, obj.id);
                ImGui::TreeNodeEx((void*)(intptr_t)obj.id, flags, "%s", lbl);
                if (sel) ImGui::PopStyleColor();
                if (ImGui::IsItemClicked()) {
                    selectedID_   = obj.id;
                    selectedType_ = kSelScenery;
                }
            }
            ImGui::TreePop();
        }
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

} // namespace gue
