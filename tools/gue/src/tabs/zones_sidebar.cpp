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

void ZonesTab::DrawTopBar(sqlite3* db, MediaTab* media) {
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
    if (ImGui::Button("Duplicate"))     DuplicateSelected(db, media);
    if (ImGui::IsItemHovered())         ImGui::SetTooltip("Ctrl+D");
    ImGui::SameLine();
    if (ImGui::Button("Copy"))          CopySelected();
    if (ImGui::IsItemHovered())         ImGui::SetTooltip("Ctrl+C");
    ImGui::SameLine();
    if (ImGui::Button("Paste"))         PasteSelected(db, media);
    if (ImGui::IsItemHovered())         ImGui::SetTooltip("Ctrl+V");

    // ── Mode selector ────────────────────────────────────────────────────
    ImGui::SameLine(0, 24.f);
    ImGui::TextDisabled("Mode:");
    ImGui::SameLine();
    static const char* kModeLabels[kModeCount] = {
        "Scenery","Terrain","Emitters","Water","ColBox",
        "Sound","Trigger","Waypoint","Portal","NPC",
        "Enviro","Other","SpawnPt","ColSphere","PlayerSpawn",
        "Foliage","Light"
    };
    ImGui::SetNextItemWidth(120.f);
    if (ImGui::BeginCombo("##zmode", kModeLabels[zoneMode_])) {
        for (int i = 0; i < kModeCount; ++i) {
            bool sel = (zoneMode_ == i);
            if (ImGui::Selectable(kModeLabels[i], sel)) {
                zoneMode_ = (ZoneMode)i;
                // Drop the current selection — the mode's placement panel takes over.
                ClearSelection();
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
    const bool ctrlSelect = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                         || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

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
            bool sel = IsInSelection(selType, obj.id);
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
                if (ctrlSelect) ToggleSelection(selType, obj.id);
                else SelectSingle(selType, obj.id);
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
    DrawGroup("G", "SpawnPts",    scene_.spawnPoints,  kSelSpawnPoint,  {0.10f,0.90f,0.40f,1.f});
    DrawGroup("@", "Player Spawns", scene_.playerSpawns, kSelPlayerSpawn, {1.00f,0.80f,0.10f,1.f});
    DrawGroup("~", "Water",      scene_.water,      kSelWater,     {0.10f,0.70f,1.00f,1.f});
    DrawGroup("E", "Emitters",   scene_.emitters,   kSelEmitter,   {0.80f,1.00f,0.20f,1.f});
    DrawGroup("L", "Lights",     scene_.lights,     kSelLight,     {1.00f,0.85f,0.50f,1.f});
    // Scenery: use model name instead of bare id, grouped by ZScenery::folder.
    // Root/ungrouped items (folder=="") render directly under "Scenery";
    // named folders get their own sub-node with click-to-select-all, a
    // right-click menu for bulk rename/delete, and act as drag-drop targets
    // so items can be dragged between folders.
    if (!scene_.scenery.empty()) {
        const bool shiftSelect = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
        static const char* kSceneryDragType = "SCENERY_FOLDER_MOVE";

        // Build modelId → base name map from media.
        std::unordered_map<int, std::string> modelNames;
        if (media) {
            for (const auto& m : media->Models()) {
                // Use only the part after the last '/' for brevity.
                const char* base = std::strrchr(m.name.c_str(), '/');
                modelNames[m.id] = base ? base + 1 : m.name;
            }
        }

        // Group indices by exact folder string (flat groups — "Forest" and
        // "Forest/Undergrowth" are two distinct nodes here, not a nested
        // tree; erase/delete/rename still treat "/" as nesting via prefix
        // match, see InSceneryFolder in zones.cpp).
        std::vector<std::string> folderOrder;
        std::unordered_map<std::string, std::vector<int>> byFolder;
        for (int i = 0; i < (int)scene_.scenery.size(); ++i) {
            const std::string& f = scene_.scenery[i].folder;
            auto fit = byFolder.find(f);
            if (fit == byFolder.end()) { folderOrder.push_back(f); byFolder[f] = {i}; }
            else fit->second.push_back(i);
        }
        // Registered-but-currently-empty folders (see CreateSceneryFolder) —
        // included even with 0 items so they stay visible/droppable.
        for (const auto& f : scene_.sceneryFolders) {
            if (byFolder.find(f) == byFolder.end()) {
                folderOrder.push_back(f);
                byFolder[f] = {};
            }
        }
        std::sort(folderOrder.begin(), folderOrder.end());

        // Visual draw order (root items + every folder's items, in the exact
        // sequence they'll render) — needed so Shift+click can select a
        // contiguous range the same way the Models list does in media.cpp.
        std::vector<int> visualOrderIds;
        visualOrderIds.reserve(scene_.scenery.size());
        for (const auto& folder : folderOrder)
            for (int idx : byFolder[folder])
                visualOrderIds.push_back(scene_.scenery[idx].id);

        auto selectRangeOrToggle = [&](int clickedId) {
            if (shiftSelect && sceneryMultiSelAnchorId_ >= 0) {
                auto itA = std::find(visualOrderIds.begin(), visualOrderIds.end(), sceneryMultiSelAnchorId_);
                auto itB = std::find(visualOrderIds.begin(), visualOrderIds.end(), clickedId);
                if (itA != visualOrderIds.end() && itB != visualOrderIds.end()) {
                    size_t posA = (size_t)(itA - visualOrderIds.begin());
                    size_t posB = (size_t)(itB - visualOrderIds.begin());
                    size_t lo = std::min(posA, posB);
                    size_t hi = std::max(posA, posB);
                    if (!ctrlSelect) ClearSelection();
                    for (size_t k = lo; k <= hi; ++k)
                        AddSelection(kSelScenery, visualOrderIds[k], k == hi);
                    return;
                }
            }
            if (ctrlSelect) {
                ToggleSelection(kSelScenery, clickedId);
                sceneryMultiSelAnchorId_ = clickedId;
            } else {
                SelectSingle(kSelScenery, clickedId);
                sceneryMultiSelAnchorId_ = clickedId;
            }
        };

        // Ids to move on a drag-drop: the whole active selection if the
        // dragged item is part of it, otherwise just that one item.
        auto dragIdsFor = [&](int itemId) -> std::vector<int> {
            if (IsInSelection(kSelScenery, itemId)) {
                std::vector<int> ids;
                for (const auto& ref : ActiveSelection())
                    if (ref.type == kSelScenery) ids.push_back(ref.id);
                if (!ids.empty()) return ids;
            }
            return {itemId};
        };

        auto folderDropTarget = [&](const std::string& targetFolder) {
            if (!ImGui::BeginDragDropTarget()) return;
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kSceneryDragType)) {
                size_t n = pl->DataSize / sizeof(int);
                const int* ids = static_cast<const int*>(pl->Data);
                MoveSceneryToFolder(db, std::vector<int>(ids, ids + n), targetFolder);
            }
            ImGui::EndDragDropTarget();
        };

        auto drawSceneryItem = [&](const ZScenery& obj) {
            bool sel = IsInSelection(kSelScenery, obj.id);
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
            ImGui::PushID(obj.id);
            ImGui::TreeNodeEx((void*)(intptr_t)obj.id, flags, "%s", lbl);
            if (sel) ImGui::PopStyleColor();
            // IsItemClicked() fires on mouse-DOWN, i.e. the very first frame
            // of a potential drag gesture too. If we always collapsed the
            // selection here, dragging any one item out of a multi-selection
            // would immediately shrink the selection to just that item
            // before BeginDragDropSource() below ever reads it — so the drag
            // only ever moved one object. Skip the (re)select when clicking
            // an item that's already part of the current selection with no
            // modifier held; that click either starts a drag (selection must
            // stay intact) or turns out to be a plain click-release (common
            // tools keep the multi-selection in that case too).
            if (ImGui::IsItemClicked()) {
                bool alreadySelected = IsInSelection(kSelScenery, obj.id);
                if (!alreadySelected || ctrlSelect || shiftSelect)
                    selectRangeOrToggle(obj.id);
            }

            // Drag source: carries the whole selection if this item is part
            // of one, so multi-selecting then dragging any of them moves all.
            if (ImGui::BeginDragDropSource()) {
                sceneryDragPayload_ = dragIdsFor(obj.id);
                ImGui::SetDragDropPayload(kSceneryDragType, sceneryDragPayload_.data(),
                                          sceneryDragPayload_.size() * sizeof(int));
                ImGui::Text("Move %d scenery instance(s)", (int)sceneryDragPayload_.size());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::BeginMenu("Move to folder")) {
                    if (ImGui::MenuItem("(root / ungrouped)"))
                        MoveSceneryToFolder(db, dragIdsFor(obj.id), "");
                    auto folders = DistinctSceneryFolders();
                    if (!folders.empty()) ImGui::Separator();
                    for (const auto& f : folders)
                        if (ImGui::MenuItem(f.c_str()))
                            MoveSceneryToFolder(db, dragIdsFor(obj.id), f);
                    ImGui::Separator();
                    if (ImGui::MenuItem("New Folder...")) {
                        sceneryFolderCreateBuf_[0] = 0;
                        showSceneryFolderCreate_ = true;
                        // Reuse rename-target as "who to assign once named" —
                        // sceneryDragPayload_ already holds the right id set.
                        sceneryDragPayload_ = dragIdsFor(obj.id);
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    if (!IsInSelection(kSelScenery, obj.id)) SelectSingle(kSelScenery, obj.id);
                    DeleteSelected(db);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        };

        ImVec4 col = {0.80f, 0.78f, 0.75f, 1.f};
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "M Scenery (%d)##grpScenery",
                      (int)scene_.scenery.size());
        bool open = ImGui::TreeNodeEx(hdr,
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::PopStyleColor();
        // Root header is also a drop target — drag an item here to ungroup it.
        folderDropTarget("");
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("New Folder...")) {
                sceneryFolderCreateBuf_[0] = 0;
                showSceneryFolderCreate_ = true;
                // If something is already selected, offer to move it into
                // the new folder too (matches the per-item "Move to folder ▸
                // New Folder..." flow); otherwise it's just created empty.
                sceneryDragPayload_.clear();
                for (const auto& ref : ActiveSelection())
                    if (ref.type == kSelScenery) sceneryDragPayload_.push_back(ref.id);
            }
            ImGui::EndPopup();
        }
        if (open) {
            for (const auto& folder : folderOrder) {
                auto& idxs = byFolder[folder];
                if (folder.empty()) {
                    for (int idx : idxs) drawSceneryItem(scene_.scenery[idx]);
                    continue;
                }
                ImGui::PushID(folder.c_str());
                char fhdr[160];
                std::snprintf(fhdr, sizeof(fhdr), "[ ] %s (%d)", folder.c_str(), (int)idxs.size());
                bool fopen = ImGui::TreeNodeEx(fhdr,
                    ImGuiTreeNodeFlags_SpanFullWidth |
                    ImGuiTreeNodeFlags_OpenOnArrow |
                    ImGuiTreeNodeFlags_OpenOnDoubleClick);
                // Click the label itself (not the arrow) selects every item
                // in the folder — matches DrawGroup's "click = select" habit.
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    ClearSelection();
                    for (size_t k = 0; k < idxs.size(); ++k)
                        AddSelection(kSelScenery, scene_.scenery[idxs[k]].id, k + 1 == idxs.size());
                    if (!idxs.empty()) sceneryMultiSelAnchorId_ = scene_.scenery[idxs.back()].id;
                }
                folderDropTarget(folder);  // drag an item onto this header to move it here
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Select All")) {
                        ClearSelection();
                        for (size_t k = 0; k < idxs.size(); ++k)
                            AddSelection(kSelScenery, scene_.scenery[idxs[k]].id, k + 1 == idxs.size());
                    }
                    if (ImGui::MenuItem("Rename / Move Folder...")) {
                        sceneryFolderRenameTarget_ = folder;
                        std::strncpy(sceneryFolderRenameBuf_, folder.c_str(), sizeof(sceneryFolderRenameBuf_) - 1);
                        sceneryFolderRenameBuf_[sizeof(sceneryFolderRenameBuf_) - 1] = 0;
                        showSceneryFolderRename_ = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete Folder (all contents)")) {
                        sceneryFolderDeleteTarget_ = folder;
                        showSceneryFolderDelete_ = true;
                    }
                    ImGui::EndPopup();
                }
                if (fopen) {
                    for (int idx : idxs) drawSceneryItem(scene_.scenery[idx]);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    // ── Folder create ─────────────────────────────────────────────────────
    // Always registers the folder (CreateSceneryFolder) so it shows up in the
    // sidebar immediately, with or without a selection. sceneryDragPayload_,
    // if non-empty, additionally moves those ids into the new folder.
    if (showSceneryFolderCreate_) ImGui::OpenPopup("New Scenery Folder");
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
    }
    if (ImGui::BeginPopupModal("New Scenery Folder", &showSceneryFolderCreate_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Folder name:");
        ImGui::SetNextItemWidth(280.f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##scnfoldnew", sceneryFolderCreateBuf_,
                                      sizeof(sceneryFolderCreateBuf_),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Use \"/\" to nest (e.g. \"Forest/Rocks\").");
        if (sceneryDragPayload_.empty()) {
            ImGui::TextDisabled("Creates an empty folder — drag items onto it\n"
                               "later, or it's pre-filled next time you place one.");
        } else {
            ImGui::TextDisabled("%d selected instance(s) will move into it.",
                                (int)sceneryDragPayload_.size());
        }
        ImGui::Spacing();
        if ((ImGui::Button("Create", {120.f, 0.f}) || enter) && sceneryFolderCreateBuf_[0]) {
            CreateSceneryFolder(db, sceneryFolderCreateBuf_);
            if (!sceneryDragPayload_.empty())
                MoveSceneryToFolder(db, sceneryDragPayload_, sceneryFolderCreateBuf_);
            std::strncpy(scnFolder_, sceneryFolderCreateBuf_, sizeof(scnFolder_) - 1);
            scnFolder_[sizeof(scnFolder_) - 1] = 0;
            std::strncpy(foliageFolder_, sceneryFolderCreateBuf_, sizeof(foliageFolder_) - 1);
            foliageFolder_[sizeof(foliageFolder_) - 1] = 0;
            showSceneryFolderCreate_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.f, 0.f})) {
            showSceneryFolderCreate_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Folder delete confirmation ──────────────────────────────────────────
    if (showSceneryFolderDelete_) ImGui::OpenPopup("Delete Scenery Folder");
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
    }
    if (ImGui::BeginPopupModal("Delete Scenery Folder", &showSceneryFolderDelete_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        int count = 0;
        for (auto& s : scene_.scenery)
            if (s.folder == sceneryFolderDeleteTarget_ ||
                (s.folder.size() > sceneryFolderDeleteTarget_.size() &&
                 s.folder.compare(0, sceneryFolderDeleteTarget_.size(), sceneryFolderDeleteTarget_) == 0 &&
                 s.folder[sceneryFolderDeleteTarget_.size()] == '/'))
                ++count;
        ImGui::TextColored({1.f, 0.6f, 0.2f, 1.f}, "Delete folder \"%s\"?",
                           sceneryFolderDeleteTarget_.c_str());
        ImGui::Text("This permanently deletes %d scenery instance(s)\n(including any nested sub-folders).", count);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete", {120.f, 0.f})) {
            DeleteSceneryFolder(db, media, sceneryFolderDeleteTarget_);
            showSceneryFolderDelete_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.f, 0.f})) {
            showSceneryFolderDelete_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Folder rename / move ────────────────────────────────────────────────
    if (showSceneryFolderRename_) ImGui::OpenPopup("Rename Scenery Folder");
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
    }
    if (ImGui::BeginPopupModal("Rename Scenery Folder", &showSceneryFolderRename_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename/move \"%s\" to:", sceneryFolderRenameTarget_.c_str());
        ImGui::SetNextItemWidth(280.f);
        bool enter = ImGui::InputText("##scnfoldren", sceneryFolderRenameBuf_,
                                      sizeof(sceneryFolderRenameBuf_),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Use \"/\" to nest under another folder (e.g. \"Forest/Rocks\").");
        ImGui::Spacing();
        if ((ImGui::Button("Rename", {120.f, 0.f}) || enter) && sceneryFolderRenameBuf_[0]) {
            RenameSceneryFolder(db, media, sceneryFolderRenameTarget_, sceneryFolderRenameBuf_);
            showSceneryFolderRename_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120.f, 0.f})) {
            showSceneryFolderRename_ = false;
            ImGui::CloseCurrentPopup();
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
        // 2+ scenery selected together (e.g. clicked a folder header) gets
        // the bulk move/delete panel instead of the single-instance editor.
        if (selectedType_ == kSelScenery) {
            const auto activeSel = ActiveSelection();
            bool allScenery = activeSel.size() > 1;
            for (const auto& ref : activeSel)
                if (ref.type != kSelScenery) { allScenery = false; break; }
            if (allScenery) {
                DrawPanelSceneryBulk(db, media, activeSel);
                return;
            }
        }
        switch (selectedType_) {
        case kSelPortal:    DrawPanelPortal   (db,        false); return;
        case kSelTrigger:   DrawPanelTrigger  (db,        false); return;
        case kSelSoundZone: DrawPanelSoundZone(db,        false); return;
        case kSelColBox:    DrawPanelColBox   (db,        false); return;
        case kSelColSphere: DrawPanelColSphere(db,        false); return;
        case kSelWaypoint:  DrawPanelWaypoint (db, media, false); return;
        case kSelNpc:        DrawPanelNPC       (db, media, false); return;
        case kSelSpawnPoint:  DrawPanelSpawnPoint(db, media, false); return;
        case kSelPlayerSpawn: DrawPanelPlayerSpawn(db,       false); return;
        case kSelEmitter:   DrawPanelEmitters (db,        false); return;
        case kSelLight:     DrawPanelLight    (db,        false); return;
        case kSelWater:     DrawPanelWater    (db, media, false); return;
        case kSelScenery:   DrawPanelScenery  (db, media, false); return;
        default: break;
        }
    }

    // No selection: show placement panel for current mode, or zone-wide settings
    switch (zoneMode_) {
    case kModeScenery:   DrawPanelScenery  (db, media, true); break;
    case kModeTerrain:   DrawPanelTerrain  (db,        true); break;
    case kModeEmitters:  DrawPanelEmitters (db,        true); break;
    case kModeWater:     DrawPanelWater    (db, media, true); break;
    case kModeColBox:    DrawPanelColBox   (db,        true); break;
    case kModeColSphere: DrawPanelColSphere(db,        true); break;
    case kModeSoundZone: DrawPanelSoundZone(db,        true); break;
    case kModeTrigger:   DrawPanelTrigger  (db,        true); break;
    case kModeWaypoint:  DrawPanelWaypoint (db, media, true); break;
    case kModePortal:    DrawPanelPortal   (db,        true); break;
    case kModeNPC:        DrawPanelNPC       (db, media, true); break;
    case kModeSpawnPoint:  DrawPanelSpawnPoint(db, media, true); break;
    case kModePlayerSpawn: DrawPanelPlayerSpawn(db,       true); break;
    case kModeFoliage:    DrawPanelFoliage  (db, media, true); break;
    case kModeLight:      DrawPanelLight    (db,        true); break;
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
            "Portal","Trigger","Sound","ColBox","Waypoint","NPC","Emitter","Water","Scenery","SpawnPt","ColSphere",
            "PlayerSpawn","Light"
        };
        int si = std::clamp(selectedType_, 0, 12);
        const int selCount = (int)ActiveSelection().size();
        if (selCount > 1) {
            ImGui::TextColored({0.4f, 0.8f, 1.f, 0.8f},
                               "Selected: %s #%d (+%d)", kSelNames[si], selectedID_, selCount - 1);
        } else {
            ImGui::TextColored({0.4f, 0.8f, 1.f, 0.8f},
                               "Selected: %s #%d", kSelNames[si], selectedID_);
        }
    } else {
        ImGui::TextDisabled("Nothing selected");
    }
    ImGui::SameLine(0, 16.f);
    ImGui::TextDisabled("%s", statusMsg_);
}

} // namespace gue
