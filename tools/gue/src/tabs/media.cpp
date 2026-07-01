#include "media.h"
#include "../file_import.h"
#include "../asset_path.h"
#include "../ui_widgets.h"

#include <imgui.h>
#include <stb_image.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <functional>
#include <numeric>
#include <filesystem>
#include <array>
#include <cfloat>
#include <cmath>

#include "rco/renderer/col_bake.h"
#include "rco/renderer/pipeline.h"

namespace gue {

// ---------------------------------------------------------------------------
// Folder-list helper — arbitrary nesting via "A/B/C" name convention.
// When tree_key is provided, items are sorted by it internally so same-folder
// items are always consecutive (required by the stack algorithm).
// When filter is non-empty, falls back to a flat filtered list.
// Returns true if the selection changed this frame.
// ---------------------------------------------------------------------------
static std::vector<std::string> SplitPath(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t slash = s.find('/', start);
        if (slash == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

static bool ComputeModelApproxCollision(const std::string& relPath,
                                        ModelShape& outBox,
                                        ModelShape& outSphere) {
    if (relPath.empty()) return false;

    std::vector<std::array<glm::vec3, 3>> tris;
    rco::renderer::ExtractMeshTriangles(ResolveClientAsset(relPath), tris);
    if (tris.empty()) return false;

    glm::vec3 bmin(FLT_MAX, FLT_MAX, FLT_MAX);
    glm::vec3 bmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const auto& tri : tris) {
        for (const auto& v : tri) {
            bmin = glm::min(bmin, v);
            bmax = glm::max(bmax, v);
        }
    }

    glm::vec3 size = bmax - bmin;
    if (size.x < 0.001f) size.x = 1.f;
    if (size.y < 0.001f) size.y = 1.f;
    if (size.z < 0.001f) size.z = 1.f;
    glm::vec3 center = (bmin + bmax) * 0.5f;

    outBox = {};
    outBox.type = 0;
    outBox.offset_x = center.x;
    outBox.offset_y = center.y;
    outBox.offset_z = center.z;
    outBox.size_x = size.x;
    outBox.size_y = size.y;
    outBox.size_z = size.z;

    std::vector<glm::vec2> pointsXZ;
    pointsXZ.reserve(tris.size() * 3);
    for (const auto& tri : tris) {
        pointsXZ.push_back({tri[0].x, tri[0].z});
        pointsXZ.push_back({tri[1].x, tri[1].z});
        pointsXZ.push_back({tri[2].x, tri[2].z});
    }

    glm::vec2 centerXZ = {(bmin.x + bmax.x) * 0.5f, (bmin.z + bmax.z) * 0.5f};
    std::vector<float> dists;
    dists.reserve(pointsXZ.size());
    for (const auto& p : pointsXZ)
        dists.push_back(glm::length(p - centerXZ));
    std::sort(dists.begin(), dists.end());

    float radiusMax = dists.empty() ? 0.f : dists.back();
    float radiusP97 = radiusMax;
    if (!dists.empty()) {
        size_t idx = (size_t)(0.97f * (float)(dists.size() - 1));
        if (idx >= dists.size()) idx = dists.size() - 1;
        radiusP97 = dists[idx];
    }

    // If there is a strong outlier spike, prefer a robust percentile radius.
    float radius = radiusMax;
    if (radiusP97 > 0.001f && radiusMax > radiusP97 * 1.15f)
        radius = radiusP97;

    if (radius < 0.001f)
        radius = std::max(size.x, size.z) * 0.5f;
    if (radius < 0.001f)
        radius = 0.5f;

    float sphereY = center.y;
    float height = bmax.y - bmin.y;
    if (height >= radius * 2.f)
        sphereY = bmin.y + radius;

    outSphere = {};
    outSphere.type = 1;
    outSphere.offset_x = centerXZ.x;
    outSphere.offset_y = sphereY;
    outSphere.offset_z = centerXZ.y;
    outSphere.size_x = radius;
    outSphere.size_y = 0.f;
    outSphere.size_z = 0.f;
    return true;
}

static const char* ModelShapeTypeName(int type) {
    switch (type) {
    case 0: return "Box";
    case 1: return "Sphere";
    case 2: return "Mesh";
    case 3: return "Wedge";
    default: return "Unknown";
    }
}

template<typename T>
static bool DrawFolderList(
    const std::vector<T>& items,
    int& sel,
    const char* filter,
    const char* child_id,
    float width,
    std::function<void(int)> on_select,
    std::function<std::string(int)> tree_key = nullptr,
    std::function<void(const std::string&)> on_folder_context = nullptr)
{
    auto toLower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c); return s;
    };

    bool changed = false;
    ImGui::BeginChild(child_id, {width, 0}, true);
    const bool filtering = filter && filter[0] != '\0';

    if (filtering) {
        const std::string filt_lo = toLower(std::string(filter));
        for (int i = 0; i < (int)items.size(); ++i) {
            if (toLower(items[i].name).find(filt_lo) == std::string::npos) continue;
            char label[256];
            std::snprintf(label, sizeof(label), "%s##%s%d", items[i].name.c_str(), child_id, i);
            if (ImGui::Selectable(label, sel == i)) {
                sel = i; on_select(i); changed = true;
            }
        }
    } else {
        // When tree_key is provided, use it for hierarchy; otherwise fall back to name.
        auto key_ = [&](int i) -> std::string {
            return tree_key ? tree_key(i) : items[i].name;
        };

        // Build a sorted render order by tree_key so same-folder items are always
        // consecutive. The stack algorithm requires this: if a folder appears
        // non-consecutively it gets opened twice with the same ImGui ID → conflict.
        // For callers without tree_key the items are already sorted by name (DB
        // ORDER BY), so the identity permutation is used and cost is zero.
        std::vector<int> order((int)items.size());
        std::iota(order.begin(), order.end(), 0);
        if (tree_key) {
            std::vector<std::string> keys((int)items.size());
            for (int i = 0; i < (int)items.size(); ++i) keys[i] = key_(i);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                return keys[a] < keys[b];
            });
        }

        // Pre-compute the selected item's folder path so we can auto-open ancestors.
        std::string sel_folder;
        if (sel >= 0 && sel < (int)items.size()) {
            std::string k = key_(sel);
            size_t slash = k.rfind('/');
            if (slash != std::string::npos)
                sel_folder = k.substr(0, slash);
        }

        // Stack-based traversal: cur_path tracks open folder segments; is_open
        // tracks whether each ImGui::TreeNode is currently expanded.
        std::vector<std::string> cur_path;
        std::vector<bool>        is_open;

        for (int j = 0; j < (int)items.size(); ++j) {
            int i = order[j];
            auto segs = SplitPath(key_(i));
            // All but the last segment form the folder path; last is the item label.
            std::vector<std::string> item_folder(segs.begin(), segs.end() - 1);
            const std::string& item_base = segs.back();

            // Find how many leading segments are shared with the current stack.
            int common = 0;
            while (common < (int)cur_path.size() && common < (int)item_folder.size()
                   && cur_path[common] == item_folder[common])
                ++common;

            // Pop nodes that are no longer on the current path.
            for (int d = (int)cur_path.size() - 1; d >= common; --d)
                if (is_open[d]) ImGui::TreePop();
            cur_path.resize(common);
            is_open.resize(common);

            // Push new folder nodes.
            for (int d = common; d < (int)item_folder.size(); ++d) {
                // If the immediate parent is closed, virtualize this level as closed
                // without calling TreeNodeEx. The pop loop already guards with
                // if(is_open[d]), so skipping the call keeps TreePop balanced.
                // Propagates automatically: d+1 will see is_open[d]=false and also skip.
                bool parent_open = (d == 0) || is_open[d - 1];
                if (!parent_open) {
                    cur_path.push_back(item_folder[d]);
                    is_open.push_back(false);
                    continue;
                }

                // Build the full path up to this depth for the ImGui ID.
                std::string full_path;
                for (int k = 0; k <= d; ++k) {
                    if (k) full_path += '/';
                    full_path += item_folder[k];
                }
                // Auto-open if this folder is an ancestor of the selected item.
                bool is_anc = !sel_folder.empty() &&
                    (sel_folder == full_path ||
                     (sel_folder.size() > full_path.size() &&
                      sel_folder[full_path.size()] == '/' &&
                      sel_folder.substr(0, full_path.size()) == full_path));
                char node_id[256];
                std::snprintf(node_id, sizeof(node_id), "%s##f_%s_%s",
                              item_folder[d].c_str(), child_id, full_path.c_str());
                ImGui::SetNextItemOpen(is_anc, ImGuiCond_Once);
                bool open = ImGui::TreeNodeEx(node_id, ImGuiTreeNodeFlags_SpanFullWidth);
                if (on_folder_context) {
                    // Strip leading ## so the popup ID has no embedded separator.
                    const char* cid = child_id;
                    while (*cid == '#') ++cid;
                    char popup_id[320];
                    std::snprintf(popup_id, sizeof(popup_id),
                                  "fctx_%s_%s", cid, full_path.c_str());
                    if (ImGui::BeginPopupContextItem(popup_id)) {
                        if (ImGui::MenuItem("Delete Folder")) {
                            on_folder_context(full_path);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                }
                cur_path.push_back(item_folder[d]);
                is_open.push_back(open);
            }

            // Render the item only when all ancestor folders are open.
            bool all_open = true;
            for (bool o : is_open) if (!o) { all_open = false; break; }
            if (all_open) {
                char label[256];
                std::snprintf(label, sizeof(label), "%s##%s%d", item_base.c_str(), child_id, i);
                if (ImGui::Selectable(label, sel == i)) {
                    sel = i; on_select(i); changed = true;
                }
            }
        }

        // Close any folders still on the stack.
        for (int d = (int)cur_path.size() - 1; d >= 0; --d)
            if (is_open[d]) ImGui::TreePop();
    }

    ImGui::EndChild();
    return changed;
}

// ---------------------------------------------------------------------------
// Slot names
// ---------------------------------------------------------------------------

static const char* kSlotNames[] = {
    "Body",
    "Hair",
    "Helm",
    "Chest",
    "Hands",
    "Belt",
    "Legs",
    "Feet",
    "Weapon",
    "Shield",
    "Attachment",
};
static constexpr int kSlotCount = (int)(sizeof(kSlotNames) / sizeof(kSlotNames[0]));

const char* ActorSlotName(int slot) {
    if (slot < 0 || slot >= kSlotCount) return "?";
    return kSlotNames[slot];
}

// ---------------------------------------------------------------------------
// material_map serialization — "k1=v1;k2=v2"
// ---------------------------------------------------------------------------

std::string SerializeMaterialMap(const std::unordered_map<std::string, std::string>& m) {
    std::string out;
    bool first = true;
    for (const auto& [k, v] : m) {
        if (k.empty() || v.empty()) continue;
        if (!first) out.push_back(';');
        first = false;
        out += k; out.push_back('='); out += v;
    }
    return out;
}

std::unordered_map<std::string, std::string> ParseMaterialMap(const std::string& s) {
    std::unordered_map<std::string, std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t semi = s.find(';', i);
        size_t end  = (semi == std::string::npos) ? s.size() : semi;
        size_t eq   = s.find('=', i);
        if (eq != std::string::npos && eq < end) {
            std::string k = s.substr(i, eq - i);
            std::string v = s.substr(eq + 1, end - eq - 1);
            if (!k.empty() && !v.empty()) out[std::move(k)] = std::move(v);
        }
        i = (semi == std::string::npos) ? s.size() : semi + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Table setup
// ---------------------------------------------------------------------------

void MediaTab::EnsureTables(sqlite3* db) {
    const char* sql[] = {
        "CREATE TABLE IF NOT EXISTS media_models ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT    NOT NULL DEFAULT 'New Model',"
        "  file_path TEXT    NOT NULL DEFAULT '',"
        "  scale     REAL    NOT NULL DEFAULT 1.0"
        ")",

        "CREATE TABLE IF NOT EXISTS media_materials ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name        TEXT    NOT NULL DEFAULT 'New Material',"
        "  albedo_path TEXT    NOT NULL DEFAULT '',"
        "  normal_path TEXT    NOT NULL DEFAULT '',"
        "  orm_path    TEXT    NOT NULL DEFAULT '',"
        "  albedo_r    REAL    NOT NULL DEFAULT 0.72,"
        "  albedo_g    REAL    NOT NULL DEFAULT 0.68,"
        "  albedo_b    REAL    NOT NULL DEFAULT 0.60,"
        "  roughness   REAL    NOT NULL DEFAULT 0.5,"
        "  metallic    REAL    NOT NULL DEFAULT 0.0"
        ")",

        "CREATE TABLE IF NOT EXISTS media_anim_clips ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name          TEXT    NOT NULL DEFAULT 'New Clip',"
        "  source_path   TEXT    NOT NULL DEFAULT '',"
        "  clip_override TEXT    NOT NULL DEFAULT '',"
        "  start_frame   INTEGER NOT NULL DEFAULT 0,"
        "  end_frame     INTEGER NOT NULL DEFAULT -1,"
        "  fps           REAL    NOT NULL DEFAULT 30.0"
        ")",

        "CREATE TABLE IF NOT EXISTS media_actor_defs ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT    NOT NULL DEFAULT 'New Actor'"
        ")",

        "CREATE TABLE IF NOT EXISTS media_actor_meshes ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  actor_def_id INTEGER NOT NULL,"
        "  slot         INTEGER NOT NULL DEFAULT 0,"
        "  model_id     INTEGER NOT NULL DEFAULT 0,"
        "  material_id  INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(actor_def_id) REFERENCES media_actor_defs(id)"
        ")",

        "CREATE TABLE IF NOT EXISTS media_actor_anims ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  actor_def_id INTEGER NOT NULL,"
        "  action       TEXT    NOT NULL DEFAULT 'Idle',"
        "  clip_id      INTEGER NOT NULL DEFAULT 0,"
        "  loop         INTEGER NOT NULL DEFAULT 1,"
        "  speed        REAL    NOT NULL DEFAULT 1.0,"
        "  blend_in     REAL    NOT NULL DEFAULT 0.15,"
        "  return_to    TEXT    NOT NULL DEFAULT '',"
        "  priority     INTEGER NOT NULL DEFAULT 0,"
        "  FOREIGN KEY(actor_def_id) REFERENCES media_actor_defs(id)"
        ")",

        "CREATE TABLE IF NOT EXISTS media_anim_events ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  clip_id    INTEGER NOT NULL,"
        "  frame      INTEGER NOT NULL DEFAULT 0,"
        "  event_type TEXT    NOT NULL DEFAULT 'sfx',"
        "  payload    TEXT    NOT NULL DEFAULT '',"
        "  FOREIGN KEY(clip_id) REFERENCES media_anim_clips(id) ON DELETE CASCADE"
        ")",

        // Animation vocabulary tree (Phase A.1). MediaTab reads this
        // independently (same definition as SettingsTab) so the actor anim
        // editor's action combo works even if Settings hasn't run yet.
        "CREATE TABLE IF NOT EXISTS anim_vocabulary ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT NOT NULL UNIQUE,"
        "  parent_id INTEGER NOT NULL DEFAULT 0"
        ")",

        // Socket vocabulary (B2). Flat list; read independently here so the
        // socket combo works without SettingsTab having run first.
        "CREATE TABLE IF NOT EXISTS socket_vocabulary ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE"
        ")",

        // Actor def socket bindings (B3a).
        "CREATE TABLE IF NOT EXISTS actor_def_sockets ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  actor_def_id INTEGER NOT NULL,"
        "  socket_name  TEXT    NOT NULL DEFAULT '',"
        "  bone_name    TEXT    NOT NULL DEFAULT '',"
        "  offset_pos_x REAL    NOT NULL DEFAULT 0.0,"
        "  offset_pos_y REAL    NOT NULL DEFAULT 0.0,"
        "  offset_pos_z REAL    NOT NULL DEFAULT 0.0,"
        "  offset_rot_x REAL    NOT NULL DEFAULT 0.0,"
        "  offset_rot_y REAL    NOT NULL DEFAULT 0.0,"
        "  offset_rot_z REAL    NOT NULL DEFAULT 0.0,"
        "  offset_scale REAL    NOT NULL DEFAULT 1.0"
        ")",
    };
    for (const char* s : sql)
        sqlite3_exec(db, s, nullptr, nullptr, nullptr);

    // Additive migrations (safe no-ops when the column already exists).
    // Stores the per-model aiMaterial → media_material name mapping as
    // "blinn1=ID01;blinn2=ID02" so multi-part models retain their texture
    // hookups across sessions.
    sqlite3_exec(db,
        "ALTER TABLE media_models ADD COLUMN material_map TEXT NOT NULL DEFAULT ''",
        nullptr, nullptr, nullptr);

    // Per-model UV transform — applied on top of the engine's automatic
    // KHR_texture_transform detection when an export omits or under-applies it.
    // Defaults preserve UVs unchanged.
    const char* modelUvColumns[] = {
        "ALTER TABLE media_models ADD COLUMN uv_offset_x REAL NOT NULL DEFAULT 0.0",
        "ALTER TABLE media_models ADD COLUMN uv_offset_y REAL NOT NULL DEFAULT 0.0",
        "ALTER TABLE media_models ADD COLUMN uv_scale_x  REAL NOT NULL DEFAULT 1.0",
        "ALTER TABLE media_models ADD COLUMN uv_scale_y  REAL NOT NULL DEFAULT 1.0",
    };
    for (const char* s : modelUvColumns)
        sqlite3_exec(db, s, nullptr, nullptr, nullptr);

    // Additive migrations for media_anim_clips new columns (V11).
    const char* clipColumns[] = {
        "ALTER TABLE media_anim_clips ADD COLUMN start_frame INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE media_anim_clips ADD COLUMN end_frame   INTEGER NOT NULL DEFAULT -1",
        "ALTER TABLE media_anim_clips ADD COLUMN fps         REAL    NOT NULL DEFAULT 30.0",
    };
    for (const char* s : clipColumns)
        sqlite3_exec(db, s, nullptr, nullptr, nullptr);

    // Additive migrations for media_actor_anims new columns (V11).
    const char* animColumns[] = {
        "ALTER TABLE media_actor_anims ADD COLUMN loop      INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE media_actor_anims ADD COLUMN speed     REAL    NOT NULL DEFAULT 1.0",
        "ALTER TABLE media_actor_anims ADD COLUMN blend_in  REAL    NOT NULL DEFAULT 0.15",
        "ALTER TABLE media_actor_anims ADD COLUMN return_to TEXT    NOT NULL DEFAULT ''",
        "ALTER TABLE media_actor_anims ADD COLUMN priority  INTEGER NOT NULL DEFAULT 0",
    };
    for (const char* s : animColumns)
        sqlite3_exec(db, s, nullptr, nullptr, nullptr);

    // Actor-def gameplay defaults (mirrors server migrateV8). Each ALTER is
    // a no-op when the column is already present; errors are ignored on
    // purpose so this runs unconditionally every launch.
    const char* adefColumns[] = {
        "ALTER TABLE media_actor_defs ADD COLUMN scale                  REAL    NOT NULL DEFAULT 1.0",
        "ALTER TABLE media_actor_defs ADD COLUMN yaw_offset             REAL    NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN y_offset               REAL    NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN default_name           TEXT    NOT NULL DEFAULT ''",
        "ALTER TABLE media_actor_defs ADD COLUMN default_race           TEXT    NOT NULL DEFAULT ''",
        "ALTER TABLE media_actor_defs ADD COLUMN default_class          TEXT    NOT NULL DEFAULT ''",
        "ALTER TABLE media_actor_defs ADD COLUMN default_level          INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE media_actor_defs ADD COLUMN default_hp             INTEGER NOT NULL DEFAULT 100",
        "ALTER TABLE media_actor_defs ADD COLUMN default_ep             INTEGER NOT NULL DEFAULT 100",
        "ALTER TABLE media_actor_defs ADD COLUMN default_aggressiveness INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN default_aggro_range    REAL    NOT NULL DEFAULT 8.0",
        "ALTER TABLE media_actor_defs ADD COLUMN default_attack_range   REAL    NOT NULL DEFAULT 2.0",
        "ALTER TABLE media_actor_defs ADD COLUMN default_respawn_ms     INTEGER NOT NULL DEFAULT 30000",
        "ALTER TABLE media_actor_defs ADD COLUMN is_playable            INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN is_mountable           INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN is_interactive         INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN loot_table_id          INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE media_actor_defs ADD COLUMN initial_spawn_id       INTEGER NOT NULL DEFAULT 0",
    };
    for (const char* s : adefColumns)
        sqlite3_exec(db, s, nullptr, nullptr, nullptr);

    // Player spawn points — also created by zone_scene.cpp EnsureTables, but
    // must exist here too so FetchAll can query it before any zone is loaded.
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS player_spawns ("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name      TEXT NOT NULL DEFAULT '',"
        "  area_name TEXT NOT NULL DEFAULT '',"
        "  x         REAL NOT NULL DEFAULT 0,"
        "  y         REAL NOT NULL DEFAULT 0,"
        "  z         REAL NOT NULL DEFAULT 0,"
        "  yaw       REAL NOT NULL DEFAULT 0"
        ")",
        nullptr, nullptr, nullptr);

    // Collision shapes table (migrateV16-equivalent for the GUE).
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS media_model_shapes ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  model_id INTEGER NOT NULL DEFAULT 0,"
        "  type     INTEGER NOT NULL DEFAULT 0,"
        "  offset_x REAL NOT NULL DEFAULT 0,"
        "  offset_y REAL NOT NULL DEFAULT 0,"
        "  offset_z REAL NOT NULL DEFAULT 0,"
        "  size_x   REAL NOT NULL DEFAULT 1,"
        "  size_y   REAL NOT NULL DEFAULT 1,"
        "  size_z   REAL NOT NULL DEFAULT 1,"
        "  detail_a REAL NOT NULL DEFAULT 0,"
        "  detail_b REAL NOT NULL DEFAULT 0"
        ")",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "CREATE INDEX IF NOT EXISTS idx_model_shapes ON media_model_shapes(model_id)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE media_model_shapes ADD COLUMN detail_a REAL NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
    sqlite3_exec(db,
        "ALTER TABLE media_model_shapes ADD COLUMN detail_b REAL NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);

    // Normal map intensity per terrain material (no-op when column already exists).
    sqlite3_exec(db,
        "ALTER TABLE media_materials ADD COLUMN normal_strength REAL NOT NULL DEFAULT 2.5",
        nullptr, nullptr, nullptr);

    // Black cutout flag (no-op when column already exists).
    sqlite3_exec(db,
        "ALTER TABLE media_materials ADD COLUMN black_cutout INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);

    // Black cutout flag per model — propagated to embedded submesh SSBO entries
    // via Model::ApplyBlackCutout so the deferred pass discards near-black pixels.
    sqlite3_exec(db,
        "ALTER TABLE media_models ADD COLUMN black_cutout INTEGER NOT NULL DEFAULT 0",
        nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Fetch all tables
// ---------------------------------------------------------------------------

static std::string colText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

void MediaTab::LoadAnimVocabNames(sqlite3* db) {
    anim_vocab_names_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT name FROM anim_vocabulary ORDER BY name",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        anim_vocab_names_.push_back(colText(stmt, 0));
    }
    sqlite3_finalize(stmt);
}

bool MediaTab::VocabContains(const std::string& name) const {
    for (const auto& n : anim_vocab_names_) {
        if (n == name) return true;
    }
    return false;
}

void MediaTab::LoadSocketVocabNames(sqlite3* db) {
    socket_vocab_names_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT name FROM socket_vocabulary ORDER BY name",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        socket_vocab_names_.push_back(colText(stmt, 0));
    }
    sqlite3_finalize(stmt);
}

void MediaTab::FetchAll(sqlite3* db) {
    EnsureTables(db);

    models_.clear();
    if (preview_mat_tex_) { glDeleteTextures(1, &preview_mat_tex_); preview_mat_tex_ = 0; }
    preview_mat_tex_id_ = -1;
    materials_.clear();
    clips_.clear();
    actor_defs_.clear();

    LoadAnimVocabNames(db);

    sqlite3_stmt* stmt = nullptr;

    // Models
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, file_path, scale, material_map,"
        "       uv_offset_x, uv_offset_y, uv_scale_x, uv_scale_y, black_cutout"
        "  FROM media_models ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaModel m;
            m.id           = sqlite3_column_int   (stmt, 0);
            m.name         = colText              (stmt, 1);
            m.file_path    = colText              (stmt, 2);
            m.scale        = (float)sqlite3_column_double(stmt, 3);
            m.material_map = ParseMaterialMap(colText(stmt, 4));
            m.uv_offset_x  = (float)sqlite3_column_double(stmt, 5);
            m.uv_offset_y  = (float)sqlite3_column_double(stmt, 6);
            m.uv_scale_x   = (float)sqlite3_column_double(stmt, 7);
            m.uv_scale_y   = (float)sqlite3_column_double(stmt, 8);
            m.black_cutout = sqlite3_column_int(stmt, 9) != 0;
            models_.push_back(std::move(m));
        }
        sqlite3_finalize(stmt);
    }

    // Materials
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, albedo_path, normal_path, orm_path,"
        "       albedo_r, albedo_g, albedo_b, roughness, metallic, normal_strength, black_cutout"
        " FROM media_materials ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaMaterial m;
            m.id              = sqlite3_column_int(stmt, 0);
            m.name            = colText(stmt, 1);
            m.albedo_path     = colText(stmt, 2);
            m.normal_path     = colText(stmt, 3);
            m.orm_path        = colText(stmt, 4);
            m.albedo_r        = (float)sqlite3_column_double(stmt, 5);
            m.albedo_g        = (float)sqlite3_column_double(stmt, 6);
            m.albedo_b        = (float)sqlite3_column_double(stmt, 7);
            m.roughness       = (float)sqlite3_column_double(stmt, 8);
            m.metallic        = (float)sqlite3_column_double(stmt, 9);
            m.normal_strength = (float)sqlite3_column_double(stmt, 10);
            m.black_cutout    = sqlite3_column_int(stmt, 11) != 0;
            materials_.push_back(std::move(m));
        }
        sqlite3_finalize(stmt);
    }

    // Anim Clips
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, source_path, clip_override, start_frame, end_frame, fps"
        " FROM media_anim_clips ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            MediaAnimClip c;
            c.id            = sqlite3_column_int(stmt, 0);
            c.name          = colText(stmt, 1);
            c.source_path   = colText(stmt, 2);
            c.clip_override = colText(stmt, 3);
            c.start_frame   = sqlite3_column_int(stmt, 4);
            c.end_frame     = sqlite3_column_int(stmt, 5);
            c.fps           = (float)sqlite3_column_double(stmt, 6);
            clips_.push_back(std::move(c));
        }
        sqlite3_finalize(stmt);
    }

    // Actor Defs
    if (sqlite3_prepare_v2(db,
        "SELECT id, name, scale, yaw_offset, y_offset,"
        "       default_name, default_race, default_class,"
        "       default_level, default_hp, default_ep,"
        "       default_aggressiveness, default_aggro_range, default_attack_range,"
        "       default_respawn_ms, is_playable, is_mountable, is_interactive, loot_table_id,"
        "       initial_spawn_id"
        " FROM media_actor_defs ORDER BY name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorDef d;
            d.id                      = sqlite3_column_int(stmt, 0);
            d.name                    = colText(stmt, 1);
            d.scale                   = (float)sqlite3_column_double(stmt, 2);
            if (d.scale <= 0.f) d.scale = 1.f;  // guard pre-migration rows
            d.yaw_offset              = (float)sqlite3_column_double(stmt, 3);
            d.y_offset                = (float)sqlite3_column_double(stmt, 4);
            d.default_name            = colText(stmt, 5);
            d.default_race            = colText(stmt, 6);
            d.default_class           = colText(stmt, 7);
            d.default_level           = sqlite3_column_int(stmt, 8);
            d.default_hp              = sqlite3_column_int(stmt, 9);
            d.default_ep              = sqlite3_column_int(stmt, 10);
            d.default_aggressiveness  = sqlite3_column_int(stmt, 11);
            d.default_aggro_range     = (float)sqlite3_column_double(stmt, 12);
            d.default_attack_range    = (float)sqlite3_column_double(stmt, 13);
            d.default_respawn_ms      = sqlite3_column_int(stmt, 14);
            d.is_playable             = sqlite3_column_int(stmt, 15) != 0;
            d.is_mountable            = sqlite3_column_int(stmt, 16) != 0;
            d.is_interactive          = sqlite3_column_int(stmt, 17) != 0;
            d.loot_table_id           = sqlite3_column_int(stmt, 18);
            d.initial_spawn_id        = sqlite3_column_int(stmt, 19);
            actor_defs_.push_back(std::move(d));
        }
        sqlite3_finalize(stmt);
    }

    // Player spawns list (for Initial Spawn combo on playable actor defs)
    player_spawns_.clear();
    if (sqlite3_prepare_v2(db,
        "SELECT id, area_name, name FROM player_spawns ORDER BY area_name, name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PlayerSpawnOption ps;
            ps.id        = sqlite3_column_int(stmt, 0);
            ps.area_name = colText(stmt, 1);
            ps.name      = colText(stmt, 2);
            player_spawns_.push_back(std::move(ps));
        }
        sqlite3_finalize(stmt);
    }

    // Actor mesh slots
    if (sqlite3_prepare_v2(db,
        "SELECT id, actor_def_id, slot, model_id, material_id"
        " FROM media_actor_meshes ORDER BY actor_def_id, slot",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorMeshSlot s;
            s.id           = sqlite3_column_int(stmt, 0);
            s.actor_def_id = sqlite3_column_int(stmt, 1);
            s.slot         = sqlite3_column_int(stmt, 2);
            s.model_id     = sqlite3_column_int(stmt, 3);
            s.material_id  = sqlite3_column_int(stmt, 4);
            for (auto& d : actor_defs_) {
                if (d.id == s.actor_def_id) { d.mesh_slots.push_back(s); break; }
            }
        }
        sqlite3_finalize(stmt);
    }

    // Per-submesh material overrides: attach to matching ActorMeshSlot entries.
    if (sqlite3_prepare_v2(db,
        "SELECT actor_mesh_id, ai_material_name, material_id"
        " FROM actor_def_submesh_materials",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int        mesh_id = sqlite3_column_int (stmt, 0);
            const char* ai_nm  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            int        mat_id  = sqlite3_column_int (stmt, 2);
            for (auto& d : actor_defs_)
                for (auto& s : d.mesh_slots)
                    if (s.id == mesh_id) {
                        s.submesh_materials[ai_nm ? ai_nm : ""] = mat_id;
                        goto next_submesh_row;
                    }
            next_submesh_row:;
        }
        sqlite3_finalize(stmt);
    }

    LoadDropListOptions(db);

    // Actor anim map — also pulls each backing clip's source_path + clip_override
    // so the Actor Def editor can edit them inline without visiting the
    // Anim Clips registry.
    if (sqlite3_prepare_v2(db,
        "SELECT id, actor_def_id, action, clip_id, loop, speed, blend_in, return_to, priority"
        " FROM media_actor_anims ORDER BY actor_def_id, action",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorAnimMap a;
            a.id           = sqlite3_column_int(stmt, 0);
            a.actor_def_id = sqlite3_column_int(stmt, 1);
            a.action       = colText(stmt, 2);
            a.clip_id      = sqlite3_column_int(stmt, 3);
            a.loop         = sqlite3_column_int(stmt, 4) != 0;
            a.speed        = (float)sqlite3_column_double(stmt, 5);
            a.blend_in     = (float)sqlite3_column_double(stmt, 6);
            a.return_to    = colText(stmt, 7);
            a.priority     = sqlite3_column_int(stmt, 8);
            // Denormalize the backing clip's fields into the map row.
            for (const auto& c : clips_) {
                if (c.id == a.clip_id) {
                    a.source_path   = c.source_path;
                    a.clip_override = c.clip_override;
                    a.start_frame   = c.start_frame;
                    a.end_frame     = c.end_frame;
                    break;
                }
            }
            for (auto& d : actor_defs_) {
                if (d.id == a.actor_def_id) { d.anim_map.push_back(a); break; }
            }
        }
        sqlite3_finalize(stmt);
    }

    // Actor socket bindings (B3a)
    if (sqlite3_prepare_v2(db,
        "SELECT id, actor_def_id, socket_name, bone_name,"
        "       offset_pos_x, offset_pos_y, offset_pos_z,"
        "       offset_rot_x, offset_rot_y, offset_rot_z, offset_scale"
        " FROM actor_def_sockets ORDER BY actor_def_id, id",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ActorDefSocket s;
            s.id           = sqlite3_column_int(stmt, 0);
            s.actor_def_id = sqlite3_column_int(stmt, 1);
            s.socket_name  = colText(stmt, 2);
            s.bone_name    = colText(stmt, 3);
            s.offset_pos_x = (float)sqlite3_column_double(stmt, 4);
            s.offset_pos_y = (float)sqlite3_column_double(stmt, 5);
            s.offset_pos_z = (float)sqlite3_column_double(stmt, 6);
            s.offset_rot_x = (float)sqlite3_column_double(stmt, 7);
            s.offset_rot_y = (float)sqlite3_column_double(stmt, 8);
            s.offset_rot_z = (float)sqlite3_column_double(stmt, 9);
            s.offset_scale = (float)sqlite3_column_double(stmt, 10);
            for (auto& d : actor_defs_) {
                if (d.id == s.actor_def_id) { d.socket_bindings.push_back(s); break; }
            }
        }
        sqlite3_finalize(stmt);
    }

    LoadSocketVocabNames(db);

    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Loaded: %d models, %d materials, %d clips, %d actor defs",
                  (int)models_.size(), (int)materials_.size(),
                  (int)clips_.size(),  (int)actor_defs_.size());
}

void MediaTab::ReloadPlayerSpawns(sqlite3* db) {
    player_spawns_.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, area_name, name FROM player_spawns ORDER BY area_name, name",
        -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PlayerSpawnOption ps;
            ps.id        = sqlite3_column_int(stmt, 0);
            ps.area_name = colText(stmt, 1);
            ps.name      = colText(stmt, 2);
            player_spawns_.push_back(std::move(ps));
        }
        sqlite3_finalize(stmt);
    }
}

void MediaTab::LoadDropListOptions(sqlite3* db) {
    drop_list_options_.clear();

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, name FROM loot_tables WHERE enabled=1 ORDER BY id",
        -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        drop_list_options_.push_back({id, text ? text : ""});
    }
    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// CRUD — Models
// ---------------------------------------------------------------------------

// Write a sidecar `<glb_path>.uv` next to the model file so the engine
// (Model::Load) can apply the same UV transform on the client side without
// having to read the GUE database. Format: 4 floats separated by spaces:
//   "<offset_x> <offset_y> <scale_x> <scale_y>"
// On defaults (offset=0,0 scale=1,1) the file is removed instead of written so
// untouched models leave no clutter.
static void WriteSidecarUV(const std::string& model_relpath,
                           float ox, float oy, float sx, float sy) {
    if (model_relpath.empty()) return;
    // Resolve same way the runtime does (see asset_path.h).
    std::string full = (model_relpath.size() > 1 && model_relpath[1] == ':')
                       ? model_relpath
                       : ("../client/" + model_relpath);
    std::filesystem::path p(full);
    p += ".uv";
    const bool is_default =
        ox == 0.f && oy == 0.f && sx == 1.f && sy == 1.f;
    std::error_code ec;
    if (is_default) {
        std::filesystem::remove(p, ec);
        return;
    }
    if (FILE* f = std::fopen(p.string().c_str(), "w")) {
        std::fprintf(f, "%.6f %.6f %.6f %.6f\n", ox, oy, sx, sy);
        std::fclose(f);
    }
}

void MediaTab::SaveModel(sqlite3* db, MediaModel& m) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    const std::string mapSerialized = SerializeMaterialMap(m.material_map);
    if (m.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_models (name, file_path, scale, material_map,"
            "                          uv_offset_x, uv_offset_y, uv_scale_x, uv_scale_y,"
            "                          black_cutout)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_models SET name=?, file_path=?, scale=?, material_map=?,"
            "                        uv_offset_x=?, uv_offset_y=?, uv_scale_x=?, uv_scale_y=?,"
            "                        black_cutout=?"
            " WHERE id=?", -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save model error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text  (stmt, 1, m.name.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 2, m.file_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, m.scale);
    sqlite3_bind_text  (stmt, 4, mapSerialized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, m.uv_offset_x);
    sqlite3_bind_double(stmt, 6, m.uv_offset_y);
    sqlite3_bind_double(stmt, 7, m.uv_scale_x);
    sqlite3_bind_double(stmt, 8, m.uv_scale_y);
    sqlite3_bind_int   (stmt, 9, m.black_cutout ? 1 : 0);
    if (m.id != 0) sqlite3_bind_int(stmt, 10, m.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save model error: %s", sqlite3_errmsg(db));
    } else {
        if (m.id == 0) m.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        ++media_revision_;  // material_map / UV edits need to reach Zone/Client
        WriteSidecarUV(m.file_path, m.uv_offset_x, m.uv_offset_y,
                                    m.uv_scale_x,  m.uv_scale_y);
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved model '%s' (id=%d).", m.name.c_str(), m.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteModel(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_models WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
    selModel_  = -1;
}

// ---------------------------------------------------------------------------
// CRUD — Collision Shapes
// ---------------------------------------------------------------------------

void MediaTab::LoadShapesForModel(sqlite3* db, int model_id) {
    model_shapes_.clear();
    sel_shape_       = -1;
    shapes_model_id_ = model_id;
    dirty_shape_     = false;
    new_shape_       = false;

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT id, model_id, type, offset_x, offset_y, offset_z, size_x, size_y, size_z, detail_a, detail_b"
        " FROM media_model_shapes WHERE model_id=? ORDER BY id",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, model_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ModelShape s;
        s.id       = sqlite3_column_int   (stmt, 0);
        s.model_id = sqlite3_column_int   (stmt, 1);
        s.type     = sqlite3_column_int   (stmt, 2);
        s.offset_x = (float)sqlite3_column_double(stmt, 3);
        s.offset_y = (float)sqlite3_column_double(stmt, 4);
        s.offset_z = (float)sqlite3_column_double(stmt, 5);
        s.size_x   = (float)sqlite3_column_double(stmt, 6);
        s.size_y   = (float)sqlite3_column_double(stmt, 7);
        s.size_z   = (float)sqlite3_column_double(stmt, 8);
        s.detail_a = (float)sqlite3_column_double(stmt, 9);
        s.detail_b = (float)sqlite3_column_double(stmt, 10);
        model_shapes_.push_back(s);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::SaveModelShape(sqlite3* db, ModelShape& s) {
    if (s.type == 2) {
        if (s.detail_a <= 0.f) s.detail_a = 100.f;
        if (s.detail_a < 0.1f) s.detail_a = 0.1f;
        if (s.detail_a > 100.f) s.detail_a = 100.f;
    } else if (s.type == 3) {
        if (s.detail_a <= 0.f) s.detail_a = 1.f;
        if (s.detail_a < 1.f) s.detail_a = 1.f;
        if (s.detail_a > 16.f) s.detail_a = 16.f;
    } else {
        s.detail_a = 0.f;
        s.detail_b = 0.f;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (s.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_model_shapes"
            " (model_id, type, offset_x, offset_y, offset_z, size_x, size_y, size_z, detail_a, detail_b)"
            " VALUES (?,?,?,?,?,?,?,?,?,?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_model_shapes SET"
            " model_id=?, type=?, offset_x=?, offset_y=?, offset_z=?, size_x=?, size_y=?, size_z=?, detail_a=?, detail_b=?"
            " WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Save shape error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int   (stmt, 1, s.model_id);
    sqlite3_bind_int   (stmt, 2, s.type);
    sqlite3_bind_double(stmt, 3, s.offset_x);
    sqlite3_bind_double(stmt, 4, s.offset_y);
    sqlite3_bind_double(stmt, 5, s.offset_z);
    sqlite3_bind_double(stmt, 6, s.size_x);
    sqlite3_bind_double(stmt, 7, s.size_y);
    sqlite3_bind_double(stmt, 8, s.size_z);
    sqlite3_bind_double(stmt, 9, s.detail_a);
    sqlite3_bind_double(stmt, 10, s.detail_b);
    if (s.id != 0) sqlite3_bind_int(stmt, 11, s.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Save shape error: %s", sqlite3_errmsg(db));
    } else {
        if (s.id == 0) s.id = (int)sqlite3_last_insert_rowid(db);
        LoadShapesForModel(db, s.model_id);
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved shape id=%d.", s.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteModelShape(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_model_shapes WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    LoadShapesForModel(db, shapes_model_id_);
}

// ---------------------------------------------------------------------------
// CRUD — Materials
// ---------------------------------------------------------------------------

void MediaTab::SaveMaterial(sqlite3* db, MediaMaterial& m) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (m.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_materials"
            " (name, albedo_path, normal_path, orm_path,"
            "  albedo_r, albedo_g, albedo_b, roughness, metallic, normal_strength, black_cutout)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_materials SET"
            " name=?, albedo_path=?, normal_path=?, orm_path=?,"
            " albedo_r=?, albedo_g=?, albedo_b=?, roughness=?, metallic=?, normal_strength=?, black_cutout=?"
            " WHERE id=?", -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save material error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text  (stmt,  1, m.name.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt,  2, m.albedo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt,  3, m.normal_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt,  4, m.orm_path.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt,  5, m.albedo_r);
    sqlite3_bind_double(stmt,  6, m.albedo_g);
    sqlite3_bind_double(stmt,  7, m.albedo_b);
    sqlite3_bind_double(stmt,  8, m.roughness);
    sqlite3_bind_double(stmt,  9, m.metallic);
    sqlite3_bind_double(stmt, 10, m.normal_strength);
    sqlite3_bind_int   (stmt, 11, m.black_cutout ? 1 : 0);
    if (m.id != 0) sqlite3_bind_int(stmt, 12, m.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save material error: %s", sqlite3_errmsg(db));
    } else {
        if (m.id == 0) m.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        materialsDirtyForPreview_ = true;  // re-apply by-name on next Models preview
        ++media_revision_;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved material '%s' (id=%d).", m.name.c_str(), m.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteMaterial(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_materials WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
    materialsDirtyForPreview_ = true;
    ++media_revision_;
    selMat_    = -1;
}

// ---------------------------------------------------------------------------
// CRUD — Anim Clips
// ---------------------------------------------------------------------------

void MediaTab::SaveAnimClip(sqlite3* db, MediaAnimClip& c) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (c.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_anim_clips"
            " (name, source_path, clip_override, start_frame, end_frame, fps)"
            " VALUES (?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_anim_clips"
            " SET name=?, source_path=?, clip_override=?, start_frame=?, end_frame=?, fps=?"
            " WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save clip error: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text  (stmt, 1, c.name.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 2, c.source_path.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 3, c.clip_override.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (stmt, 4, c.start_frame);
    sqlite3_bind_int   (stmt, 5, c.end_frame);
    sqlite3_bind_double(stmt, 6, c.fps);
    if (c.id != 0) sqlite3_bind_int(stmt, 7, c.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save clip error: %s", sqlite3_errmsg(db));
    } else {
        if (c.id == 0) c.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved clip '%s' (id=%d).", c.name.c_str(), c.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteAnimClip(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_anim_clips WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
    selClip_   = -1;
}

// ---------------------------------------------------------------------------
// CRUD — Actor Defs + children
// ---------------------------------------------------------------------------

void MediaTab::SaveActorDef(sqlite3* db, ActorDef& d) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (d.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_actor_defs"
            " (name, scale, yaw_offset, y_offset, default_name, default_race, default_class,"
            "  default_level, default_hp, default_ep,"
            "  default_aggressiveness, default_aggro_range, default_attack_range,"
            "  default_respawn_ms, is_playable, is_mountable, is_interactive, loot_table_id,"
            "  initial_spawn_id)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_actor_defs SET"
            "  name=?, scale=?, yaw_offset=?, y_offset=?,"
            "  default_name=?, default_race=?, default_class=?,"
            "  default_level=?, default_hp=?, default_ep=?,"
            "  default_aggressiveness=?, default_aggro_range=?, default_attack_range=?,"
            "  default_respawn_ms=?, is_playable=?, is_mountable=?, is_interactive=?,"
            "  loot_table_id=?, initial_spawn_id=?"
            " WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save actor def error: %s", sqlite3_errmsg(db));
        return;
    }
    int i = 1;
    sqlite3_bind_text  (stmt, i++, d.name.c_str(),          -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, i++, d.scale);
    sqlite3_bind_double(stmt, i++, d.yaw_offset);
    sqlite3_bind_double(stmt, i++, d.y_offset);
    sqlite3_bind_text  (stmt, i++, d.default_name.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, i++, d.default_race.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, i++, d.default_class.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int   (stmt, i++, d.default_level);
    sqlite3_bind_int   (stmt, i++, d.default_hp);
    sqlite3_bind_int   (stmt, i++, d.default_ep);
    sqlite3_bind_int   (stmt, i++, d.default_aggressiveness);
    sqlite3_bind_double(stmt, i++, d.default_aggro_range);
    sqlite3_bind_double(stmt, i++, d.default_attack_range);
    sqlite3_bind_int   (stmt, i++, d.default_respawn_ms);
    sqlite3_bind_int   (stmt, i++, d.is_playable    ? 1 : 0);
    sqlite3_bind_int   (stmt, i++, d.is_mountable   ? 1 : 0);
    sqlite3_bind_int   (stmt, i++, d.is_interactive ? 1 : 0);
    sqlite3_bind_int   (stmt, i++, d.loot_table_id);
    sqlite3_bind_int   (stmt, i++, d.initial_spawn_id);
    if (d.id != 0) sqlite3_bind_int(stmt, i++, d.id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save actor def error: %s", sqlite3_errmsg(db));
    } else {
        if (d.id == 0) d.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Saved actor def '%s' (id=%d).", d.name.c_str(), d.id);
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DuplicateActorDef(sqlite3* db, int sourceId) {
    // Find the source def in the cached list.
    const ActorDef* src = nullptr;
    for (const auto& d : actor_defs_) {
        if (d.id == sourceId) { src = &d; break; }
    }
    if (!src) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Duplicate: actor def id=%d not found.", sourceId);
        return;
    }

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    // Clone the def row (including scale + gameplay defaults).
    ActorDef copy = *src;
    copy.id   = 0;
    copy.name = src->name + " (copy)";
    SaveActorDef(db, copy);
    if (copy.id == 0) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Duplicate failed: could not insert new actor def row.");
        return;
    }

    // Clone mesh slots under the new def id.
    for (const auto& s : src->mesh_slots) {
        ActorMeshSlot ns = s;
        ns.id           = 0;
        ns.actor_def_id = copy.id;
        SaveMeshSlot(db, ns);
    }
    // Clone anim mappings. Clear clip_id so SaveAnimMap creates a fresh
    // backing media_anim_clips row for the copy — otherwise editing an
    // animation on the duplicate would mutate the source's clip.
    for (const auto& a : src->anim_map) {
        ActorAnimMap na = a;
        na.id           = 0;
        na.actor_def_id = copy.id;
        na.clip_id      = 0;
        SaveAnimMap(db, na);
    }

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    // Refetch so the list shows the new def, then select it.
    FetchAll(db);
    for (int i = 0; i < (int)actor_defs_.size(); ++i) {
        if (actor_defs_[i].id == copy.id) {
            selActorDef_   = i;
            editActorDef_  = actor_defs_[i];
            dirtyActorDef_ = false;
            newActorDef_   = false;
            break;
        }
    }
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Duplicated '%s' → '%s' (id=%d).",
                  src->name.c_str(), copy.name.c_str(), copy.id);
}

void MediaTab::DeleteActorDef(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_prepare_v2(db, "DELETE FROM media_actor_meshes WHERE actor_def_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM media_actor_anims WHERE actor_def_id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM media_actor_defs WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    needFetch_   = true;
    selActorDef_ = -1;
}

void MediaTab::SaveMeshSlot(sqlite3* db, ActorMeshSlot& s) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (s.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_actor_meshes (actor_def_id, slot, model_id, material_id)"
            " VALUES (?, ?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_actor_meshes SET slot=?, model_id=?, material_id=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) return;

    if (s.id == 0) {
        sqlite3_bind_int(stmt, 1, s.actor_def_id);
        sqlite3_bind_int(stmt, 2, s.slot);
        sqlite3_bind_int(stmt, 3, s.model_id);
        sqlite3_bind_int(stmt, 4, s.material_id);
    } else {
        sqlite3_bind_int(stmt, 1, s.slot);
        sqlite3_bind_int(stmt, 2, s.model_id);
        sqlite3_bind_int(stmt, 3, s.material_id);
        sqlite3_bind_int(stmt, 4, s.id);
    }
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (ok) {
        if (s.id == 0) s.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
    }
    sqlite3_finalize(stmt);

    {
        std::string mat_name = "(none/embedded)";
        for (const auto& mm : materials_)
            if (mm.id == s.material_id) { mat_name = mm.name + " albedo='" + mm.albedo_path + "'"; break; }
        std::fprintf(stderr,
            "[actor-mat] SaveMeshSlot: slot=%s model_id=%d material_id=%d (%s) -> %s\n",
            ActorSlotName(s.slot), s.model_id, s.material_id,
            mat_name.c_str(), ok ? "SAVED" : "DB ERROR");
    }

    // Replace per-submesh material overrides (DELETE + INSERT, idempotent).
    if (s.id != 0) {
        sqlite3_stmt* del = nullptr;
        sqlite3_prepare_v2(db,
            "DELETE FROM actor_def_submesh_materials WHERE actor_mesh_id=?",
            -1, &del, nullptr);
        sqlite3_bind_int(del, 1, s.id);
        sqlite3_step(del);
        sqlite3_finalize(del);

        for (const auto& [ai_name, mat_id] : s.submesh_materials) {
            if (mat_id == 0 || ai_name.empty()) continue;
            sqlite3_stmt* ins = nullptr;
            sqlite3_prepare_v2(db,
                "INSERT OR REPLACE INTO actor_def_submesh_materials"
                " (actor_mesh_id, ai_material_name, material_id) VALUES (?, ?, ?)",
                -1, &ins, nullptr);
            sqlite3_bind_int (ins, 1, s.id);
            sqlite3_bind_text(ins, 2, ai_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (ins, 3, mat_id);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
    }
}

void MediaTab::DeleteMeshSlot(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_actor_meshes WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
}

void MediaTab::SaveAnimMap(sqlite3* db, ActorAnimMap& a) {
    // Upsert the backing media_anim_clips row first, so the map row always
    // points at a valid clip. We auto-manage the clip so the user only sees
    // the Actor Def editor (action + source + clip_override), never the
    // Anim Clips registry.
    {
        sqlite3_stmt* cs = nullptr;
        if (a.clip_id == 0) {
            if (sqlite3_prepare_v2(db,
                "INSERT INTO media_anim_clips (name, source_path, clip_override, start_frame, end_frame)"
                " VALUES (?, ?, ?, ?, ?)",
                -1, &cs, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(cs, 1, a.action.c_str(),        -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cs, 2, a.source_path.c_str(),   -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cs, 3, a.clip_override.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int (cs, 4, a.start_frame);
                sqlite3_bind_int (cs, 5, a.end_frame);
                if (sqlite3_step(cs) == SQLITE_DONE)
                    a.clip_id = (int)sqlite3_last_insert_rowid(db);
                sqlite3_finalize(cs);
            }
        } else {
            if (sqlite3_prepare_v2(db,
                "UPDATE media_anim_clips SET name=?, source_path=?, clip_override=?,"
                " start_frame=?, end_frame=? WHERE id=?",
                -1, &cs, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(cs, 1, a.action.c_str(),        -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cs, 2, a.source_path.c_str(),   -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(cs, 3, a.clip_override.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int (cs, 4, a.start_frame);
                sqlite3_bind_int (cs, 5, a.end_frame);
                sqlite3_bind_int (cs, 6, a.clip_id);
                sqlite3_step(cs);
                sqlite3_finalize(cs);
            }
        }
    }

    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (a.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_actor_anims"
            " (actor_def_id, action, clip_id, loop, speed, blend_in, return_to, priority)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_actor_anims"
            " SET action=?, clip_id=?, loop=?, speed=?, blend_in=?, return_to=?, priority=?"
            " WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) return;

    if (a.id == 0) {
        sqlite3_bind_int   (stmt, 1, a.actor_def_id);
        sqlite3_bind_text  (stmt, 2, a.action.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 3, a.clip_id);
        sqlite3_bind_int   (stmt, 4, a.loop ? 1 : 0);
        sqlite3_bind_double(stmt, 5, a.speed);
        sqlite3_bind_double(stmt, 6, a.blend_in);
        sqlite3_bind_text  (stmt, 7, a.return_to.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 8, a.priority);
    } else {
        sqlite3_bind_text  (stmt, 1, a.action.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 2, a.clip_id);
        sqlite3_bind_int   (stmt, 3, a.loop ? 1 : 0);
        sqlite3_bind_double(stmt, 4, a.speed);
        sqlite3_bind_double(stmt, 5, a.blend_in);
        sqlite3_bind_text  (stmt, 6, a.return_to.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 7, a.priority);
        sqlite3_bind_int   (stmt, 8, a.id);
    }
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        if (a.id == 0) a.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteAnimMap(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_actor_anims WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
}

// ---------------------------------------------------------------------------
// Actor Def Socket CRUD (B3a)
// ---------------------------------------------------------------------------

void MediaTab::SaveActorDefSocket(sqlite3* db, ActorDefSocket& s) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (s.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO actor_def_sockets"
            " (actor_def_id, socket_name, bone_name,"
            "  offset_pos_x, offset_pos_y, offset_pos_z,"
            "  offset_rot_x, offset_rot_y, offset_rot_z, offset_scale)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE actor_def_sockets SET"
            "  socket_name=?, bone_name=?,"
            "  offset_pos_x=?, offset_pos_y=?, offset_pos_z=?,"
            "  offset_rot_x=?, offset_rot_y=?, offset_rot_z=?,"
            "  offset_scale=?"
            " WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save socket error: %s", sqlite3_errmsg(db));
        return;
    }
    int i = 1;
    if (s.id == 0) sqlite3_bind_int(stmt, i++, s.actor_def_id);
    sqlite3_bind_text  (stmt, i++, s.socket_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, i++, s.bone_name.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, i++, s.offset_pos_x);
    sqlite3_bind_double(stmt, i++, s.offset_pos_y);
    sqlite3_bind_double(stmt, i++, s.offset_pos_z);
    sqlite3_bind_double(stmt, i++, s.offset_rot_x);
    sqlite3_bind_double(stmt, i++, s.offset_rot_y);
    sqlite3_bind_double(stmt, i++, s.offset_rot_z);
    sqlite3_bind_double(stmt, i++, s.offset_scale);
    if (s.id != 0) sqlite3_bind_int(stmt, i++, s.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save socket error: %s", sqlite3_errmsg(db));
    } else {
        if (s.id == 0) s.id = (int)sqlite3_last_insert_rowid(db);
        needFetch_ = true;
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Saved socket.");
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteActorDefSocket(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM actor_def_sockets WHERE id=?",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    needFetch_ = true;
}

// ---------------------------------------------------------------------------
// Animation Events CRUD
// ---------------------------------------------------------------------------

void MediaTab::LoadEventsForClip(sqlite3* db, int clip_id) {
    clip_events_.clear();
    if (clip_id <= 0) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
        "SELECT id, clip_id, frame, event_type, payload"
        " FROM media_anim_events WHERE clip_id = ? ORDER BY frame",
        -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, clip_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MediaAnimEvent ev;
        ev.id         = sqlite3_column_int(stmt, 0);
        ev.clip_id    = sqlite3_column_int(stmt, 1);
        ev.frame      = sqlite3_column_int(stmt, 2);
        ev.event_type = colText(stmt, 3);
        ev.payload    = colText(stmt, 4);
        clip_events_.push_back(std::move(ev));
    }
    sqlite3_finalize(stmt);
}

void MediaTab::SaveAnimEvent(sqlite3* db, MediaAnimEvent& e) {
    sqlite3_stmt* stmt = nullptr;
    int rc;
    if (e.id == 0) {
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO media_anim_events (clip_id, frame, event_type, payload)"
            " VALUES (?, ?, ?, ?)",
            -1, &stmt, nullptr);
    } else {
        rc = sqlite3_prepare_v2(db,
            "UPDATE media_anim_events SET frame=?, event_type=?, payload=? WHERE id=?",
            -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save event error: %s", sqlite3_errmsg(db));
        return;
    }
    if (e.id == 0) {
        sqlite3_bind_int (stmt, 1, e.clip_id);
        sqlite3_bind_int (stmt, 2, e.frame);
        sqlite3_bind_text(stmt, 3, e.event_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, e.payload.c_str(),    -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int (stmt, 1, e.frame);
        sqlite3_bind_text(stmt, 2, e.event_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, e.payload.c_str(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 4, e.id);
    }
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        if (e.id == 0) e.id = (int)sqlite3_last_insert_rowid(db);
    } else {
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Save event error: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
}

void MediaTab::DeleteAnimEvent(sqlite3* db, int id) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM media_anim_events WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------

static bool InputString(const char* id, std::string& s, size_t maxLen = 256) {
    std::vector<char> buf(maxLen);
    std::strncpy(buf.data(), s.c_str(), maxLen - 1);
    buf[maxLen - 1] = 0;
    if (ImGui::InputText(id, buf.data(), maxLen)) {
        s = buf.data();
        return true;
    }
    return false;
}

// Sanitize a user-supplied name into a filesystem-safe folder fragment:
// lowercase, alphanumerics and underscores only. Used by Models + Materials
// editors so single-file imports land under assets/<kind>/<name>/ instead
// of flattening under assets/<kind>/.
static std::string SafeFolderName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') out.push_back(char(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            out.push_back(c);
        else if (!out.empty() && out.back() != '_')
            out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// Path input field with a native "Browse..." button. Picking a file outside
// of dist/client/assets/ copies it into assets/<target_subdir>/; already-in-tree
// files are referenced directly. Returns true if the path changed.
static bool PathField(const char* label, std::string& path,
                      const char* filter_label,
                      const char* filter_exts,
                      const char* target_subdir) {
    bool changed = false;
    // Reserve space on the right for the button.
    float btnW = 36.f;
    float avail = ImGui::GetContentRegionAvail().x;
    // Use a reasonable width for the text box, leaving room for button + label.
    ImGui::SetNextItemWidth(avail - btnW - 140.f);
    if (InputString(label, path, 512)) changed = true;
    ImGui::SameLine(0.f, 4.f);
    std::string btnId = std::string("...##br_") + label;
    if (ImGui::Button(btnId.c_str(), {btnW, 0})) {
        std::string picked = gue::PickAndImportAsset(filter_label, filter_exts, target_subdir);
        if (!picked.empty()) {
            path = picked;
            changed = true;
        }
    }
    return changed;
}

// Combo for picking from a list of (id, name) pairs; value is the id.
// Wraps ui::SearchableComboId so every picker in this file gets a
// filter-as-you-type search box — essential for lists of 100+ items.
static bool ComboId(const char* label, int& currentId,
                    const std::vector<std::pair<int, std::string>>& items,
                    const char* emptyLabel = "(none)") {
    return ui::SearchableComboId(label, currentId, items, emptyLabel);
}

static std::vector<std::pair<int, std::string>>
    idNameList(const std::vector<MediaModel>& v) {
    std::vector<std::pair<int, std::string>> out;
    for (auto& m : v) out.push_back({m.id, m.name});
    return out;
}
static std::vector<std::pair<int, std::string>>
    idNameList(const std::vector<MediaMaterial>& v) {
    std::vector<std::pair<int, std::string>> out;
    for (auto& m : v) out.push_back({m.id, m.name});
    return out;
}
static std::vector<std::pair<int, std::string>>
    idNameList(const std::vector<MediaAnimClip>& v) {
    std::vector<std::pair<int, std::string>> out;
    for (auto& c : v) out.push_back({c.id, c.name});
    return out;
}

// ---------------------------------------------------------------------------
// Lazy-init the preview viewport (needs a live GL context + shared Engine/
// Pipeline from GUE main; built on first use).
// ---------------------------------------------------------------------------
static void EnsurePreview(std::unique_ptr<PreviewViewport>& pv, bool& ok,
                          ::rco::renderer::Engine* engine,
                          ::rco::renderer::Pipeline* pipeline) {
    if (pv) return;
    if (!engine || !pipeline) {
        ok = false;
        return;
    }
    pv = std::make_unique<PreviewViewport>();
    pv->Init(engine, pipeline);
    ok = true;
}

// ---------------------------------------------------------------------------
// Models sub-tab
// ---------------------------------------------------------------------------

void MediaTab::DrawModels(sqlite3* db) {
    EnsurePreview(preview_, preview_init_ok_, engine_, pipeline_);

    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Model")) {
        pendingModel_ = {};
        newModel_ = true;
        selModel_ = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Import Model Files..."))    ImportFilesBatch(db);
    ImGui::SameLine();
    if (ImGui::Button("Import Models (folder)...")) ImportFolderTree(db);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::SetNextItemWidth(240);
    ImGui::InputText("##mdl_filter", filterModel_, sizeof(filterModel_));
    ImGui::SameLine(); ImGui::TextDisabled("filter");
    DrawFolderList(models_, selModel_, filterModel_, "##mdl_list", 240,
        [&](int i) { editModel_ = models_[i]; dirtyModel_ = false; newModel_ = false; },
        nullptr,
        [&](const std::string& fp) { OpenFolderDeleteModal_(db, fp, true); });
    ImGui::SameLine();

    // Middle column: properties (fixed width).
    float total_w   = ImGui::GetContentRegionAvail().x;
    float preview_w = std::max(280.f, total_w * 0.45f);
    float props_w   = total_w - preview_w - 8.f;

    ImGui::BeginChild("##mdl_edit", {props_w, 0}, true);
    // Per-model subfolder so side-files (textures referenced by .gltf, .mtl,
    // etc.) stay grouped with the mesh file instead of flattening under
    // assets/models/. Folder name derived from the model's Name field.
    auto modelSubdir = [](const std::string& name) {
        std::string f = SafeFolderName(name);
        return f.empty() ? std::string("models")
                         : std::string("models/") + f;
    };

    if (newModel_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Model");
        ImGui::Separator();
        InputString("Name", pendingModel_.name);
        {
            std::string sub = modelSubdir(pendingModel_.name);
            PathField("File Path", pendingModel_.file_path,
                      "3D Model", "glb,fbx,obj,dae,b3d,gltf", sub.c_str());
        }
        ImGui::InputFloat("Scale", &pendingModel_.scale, 0.1f, 1.f, "%.2f");
        ImGui::TextDisabled("Click [...] to pick a file. If it's outside dist/client/assets/,");
        ImGui::TextDisabled("it's copied into assets/models/<name>/ automatically.");
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveModel(db, pendingModel_); newModel_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newModel_ = false;
    } else if (selModel_ >= 0 && selModel_ < (int)models_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editModel_.id, editModel_.name.c_str());
        ImGui::Separator();
        if (InputString("Name", editModel_.name)) dirtyModel_ = true;
        {
            std::string sub = modelSubdir(editModel_.name);
            if (PathField("File Path", editModel_.file_path,
                          "3D Model", "glb,fbx,obj,dae,b3d,gltf", sub.c_str()))
                dirtyModel_ = true;
        }
        if (ImGui::InputFloat("Scale", &editModel_.scale, 0.1f, 1.f, "%.2f"))
            dirtyModel_ = true;
        if (preview_ && preview_->CurrentPath() == editModel_.file_path) {
            glm::vec3 bmin = preview_->GetModel().BoundsMin();
            glm::vec3 bmax = preview_->GetModel().BoundsMax();
            if (bmax.x > bmin.x) {
                glm::vec3 sz = (bmax - bmin) * editModel_.scale;
                ImGui::TextDisabled("Size: %.2f x %.2f x %.2f  (W x H x D)", sz.x, sz.y, sz.z);
            }
        }
        if (ImGui::Checkbox("Black = transparent (cutout)##mc", &editModel_.black_cutout)) {
            dirtyModel_ = true;
            if (preview_) preview_->SetStaticBlackCutout(editModel_.black_cutout);
        }
        ImGui::TextDisabled("[...] imports automatically into assets/models/<name>/ if needed.");
        ImGui::Spacing();

        // Check whether the on-viewport UV sliders have a non-identity delta
        // pending; the Save button must enable for those edits even when no
        // other field changed.
        float uv_slider_ox = 0, uv_slider_oy = 0, uv_slider_sx = 1, uv_slider_sy = 1;
        if (preview_) preview_->GetUVTransform(uv_slider_ox, uv_slider_oy,
                                                uv_slider_sx, uv_slider_sy);
        const bool uv_dirty =
            !(uv_slider_ox == 0.f && uv_slider_oy == 0.f
              && uv_slider_sx == 1.f && uv_slider_sy == 1.f);

        ImGui::BeginDisabled(!dirtyModel_ && !uv_dirty);
        if (ImGui::Button("Save")) {
            // Consolidate the live slider delta with whatever UV transform the
            // engine actually applied to the loaded VBO (could be a previous
            // sidecar OR the file's own KHR_texture_transform).  Composing
            // against the persisted DB values would lose the KHR baseline on
            // the very first save and introduce visible misalignment.
            //   u_final = (u * baseline_S + baseline_O) * delta_S + delta_O
            //           = u * (baseline_S * delta_S)
            //               + (baseline_O * delta_S + delta_O)
            if (uv_dirty && preview_) {
                glm::vec2 base_o = preview_->GetModel().EffectiveUVOffset();
                glm::vec2 base_s = preview_->GetModel().EffectiveUVScale();
                editModel_.uv_offset_x = base_o.x * uv_slider_sx + uv_slider_ox;
                editModel_.uv_offset_y = base_o.y * uv_slider_sy + uv_slider_oy;
                editModel_.uv_scale_x  = base_s.x * uv_slider_sx;
                editModel_.uv_scale_y  = base_s.y * uv_slider_sy;
            }
            SaveModel(db, editModel_);
            dirtyModel_ = false;
            if (uv_dirty && preview_) {
                preview_->SetUVTransform(0.f, 0.f, 1.f, 1.f);  // delta consumed
                preview_->ReloadCurrent();                      // pick up sidecar
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            editModel_ = models_[selModel_];
            dirtyModel_ = false;
            if (preview_) preview_->SetUVTransform(0.f, 0.f, 1.f, 1.f);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) DeleteModel(db, editModel_.id);
        ImGui::PopStyleColor();
        // Surface the persisted UV transform inline + offer a one-click reset
        // so the user can wipe a bad transform without manual DB / file edits.
        const bool uv_persisted =
            editModel_.uv_offset_x != 0.f || editModel_.uv_offset_y != 0.f
         || editModel_.uv_scale_x  != 1.f || editModel_.uv_scale_y  != 1.f;
        if (uv_persisted) {
            ImGui::SameLine();
            ImGui::TextDisabled("UV: O=(%.3f,%.3f) S=(%.3f,%.3f)",
                editModel_.uv_offset_x, editModel_.uv_offset_y,
                editModel_.uv_scale_x,  editModel_.uv_scale_y);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear UV")) {
                editModel_.uv_offset_x = 0.f; editModel_.uv_offset_y = 0.f;
                editModel_.uv_scale_x  = 1.f; editModel_.uv_scale_y  = 1.f;
                SaveModel(db, editModel_);
                if (selModel_ >= 0 && selModel_ < (int)models_.size())
                    models_[selModel_] = editModel_;
                if (preview_) {
                    preview_->SetUVTransform(0.f, 0.f, 1.f, 1.f);
                    preview_->ReloadCurrent();
                }
            }
        }

        // ── Collision Shapes ─────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored({1.f, 0.85f, 0.4f, 1.f}, "Collision Shapes");
        ImGui::TextDisabled("Box/Wedge: W/H/D = full extents. Sphere: Radius = size_x. Mesh: Face Budget%%.");

        if (shapes_model_id_ != editModel_.id)
            LoadShapesForModel(db, editModel_.id);

        ModelShape autoFitBox{};
        ModelShape autoFitSphere{};
        bool hasAutoFit = ComputeModelApproxCollision(editModel_.file_path, autoFitBox, autoFitSphere);
        auto sanitizeShapeDetails = [](ModelShape& s) {
            if (s.type == 2) {
                if (s.detail_a <= 0.f) s.detail_a = 100.f;
                if (s.detail_a < 0.1f) s.detail_a = 0.1f;
                if (s.detail_a > 100.f) s.detail_a = 100.f;
            } else if (s.type == 3) {
                if (s.detail_a <= 0.f) s.detail_a = 1.f;
                if (s.detail_a < 1.f) s.detail_a = 1.f;
                if (s.detail_a > 16.f) s.detail_a = 16.f;
            }
        };

        for (int i = 0; i < (int)model_shapes_.size(); ++i) {
            const auto& s = model_shapes_[i];
            char label[64];
            std::snprintf(label, sizeof(label), "[%d] %s##sh%d",
                          s.id, ModelShapeTypeName(s.type), i);
            if (ImGui::Selectable(label, sel_shape_ == i)) {
                sel_shape_   = i;
                edit_shape_  = model_shapes_[i];
                dirty_shape_ = false;
                new_shape_   = false;
            }
        }

        if (ImGui::Button("+ Add Shape")) {
            new_shape_          = true;
            sel_shape_          = -1;
            pending_shape_      = {};
            pending_shape_.model_id = editModel_.id;
            if (hasAutoFit) {
                pending_shape_.type = 0;
                pending_shape_.offset_x = autoFitBox.offset_x;
                pending_shape_.offset_y = autoFitBox.offset_y;
                pending_shape_.offset_z = autoFitBox.offset_z;
                pending_shape_.size_x   = autoFitBox.size_x;
                pending_shape_.size_y   = autoFitBox.size_y;
                pending_shape_.size_z   = autoFitBox.size_z;
            }
            sanitizeShapeDetails(pending_shape_);
            dirty_shape_        = false;
        }

        if (new_shape_) {
            ImGui::Separator();
            ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Shape");
            static const char* kTypes[] = { "Box", "Sphere", "Mesh", "Wedge" };
            ImGui::Combo("Type##ns", &pending_shape_.type, kTypes, 4);
            sanitizeShapeDetails(pending_shape_);
            if (hasAutoFit && pending_shape_.type != 2) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Fit to Model##ns")) {
                    const ModelShape& src = (pending_shape_.type == 1) ? autoFitSphere : autoFitBox;
                    pending_shape_.offset_x = src.offset_x;
                    pending_shape_.offset_y = src.offset_y;
                    pending_shape_.offset_z = src.offset_z;
                    pending_shape_.size_x   = src.size_x;
                    pending_shape_.size_y   = src.size_y;
                    pending_shape_.size_z   = src.size_z;
                }
            }
            if (pending_shape_.type == 0) {
                ImGui::DragFloat3("Offset XYZ##ns", &pending_shape_.offset_x, 0.02f);
                ImGui::DragFloat3("Size W/H/D##ns", &pending_shape_.size_x, 0.02f);
            } else if (pending_shape_.type == 1) {
                ImGui::DragFloat3("Offset XYZ##ns", &pending_shape_.offset_x, 0.02f);
                ImGui::DragFloat("Radius##ns", &pending_shape_.size_x, 0.02f, 0.01f, 10000.f, "%.3f");
                if (pending_shape_.size_x < 0.01f) pending_shape_.size_x = 0.01f;
            } else if (pending_shape_.type == 3) {
                ImGui::DragFloat3("Offset XYZ##ns", &pending_shape_.offset_x, 0.02f);
                ImGui::DragFloat3("Size W/H/D##ns", &pending_shape_.size_x, 0.02f);
                ImGui::SameLine();
                if (ImGui::SmallButton("Flip Slope##ns")) {
                    float sx = std::abs(pending_shape_.size_x);
                    if (sx < 0.01f) sx = 1.f;
                    pending_shape_.size_x = pending_shape_.size_x >= 0.f ? -sx : sx;
                }
                int subdiv = (int)std::lround(pending_shape_.detail_a);
                if (subdiv < 1) subdiv = 1;
                if (subdiv > 16) subdiv = 16;
                if (ImGui::SliderInt("Subdivisions##ns", &subdiv, 1, 16))
                    pending_shape_.detail_a = (float)subdiv;
                ImGui::TextDisabled("Wedge direction follows Size X sign (+/-).");
            } else if (pending_shape_.type == 2) {
                if (pending_shape_.detail_a < 0.1f) pending_shape_.detail_a = 100.f;
                ImGui::SliderFloat("Face Budget %%##ns", &pending_shape_.detail_a, 0.1f, 100.f, "%.1f%%");
                ImGui::TextDisabled("Lower values simplify collision mesh for this shape.");
            } else {
                ImGui::TextDisabled("Uses the full model geometry.");
            }
            ImGui::Spacing();
            if (ImGui::Button("Create##ns")) {
                SaveModelShape(db, pending_shape_);
                new_shape_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##ns")) new_shape_ = false;
        } else if (sel_shape_ >= 0 && sel_shape_ < (int)model_shapes_.size()) {
            ImGui::Separator();
            static const char* kTypes[] = { "Box", "Sphere", "Mesh", "Wedge" };
            if (ImGui::Combo("Type##es", &edit_shape_.type, kTypes, 4))
                dirty_shape_ = true;
            sanitizeShapeDetails(edit_shape_);
            if (hasAutoFit && edit_shape_.type != 2) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Fit to Model##es")) {
                    const ModelShape& src = (edit_shape_.type == 1) ? autoFitSphere : autoFitBox;
                    edit_shape_.offset_x = src.offset_x;
                    edit_shape_.offset_y = src.offset_y;
                    edit_shape_.offset_z = src.offset_z;
                    edit_shape_.size_x   = src.size_x;
                    edit_shape_.size_y   = src.size_y;
                    edit_shape_.size_z   = src.size_z;
                    dirty_shape_ = true;
                }
            }
            if (edit_shape_.type == 0) {
                if (ImGui::DragFloat3("Offset XYZ##es", &edit_shape_.offset_x, 0.02f))
                    dirty_shape_ = true;
                if (ImGui::DragFloat3("Size W/H/D##es", &edit_shape_.size_x, 0.02f))
                    dirty_shape_ = true;
            } else if (edit_shape_.type == 1) {
                if (ImGui::DragFloat3("Offset XYZ##es", &edit_shape_.offset_x, 0.02f))
                    dirty_shape_ = true;
                if (ImGui::DragFloat("Radius##es", &edit_shape_.size_x, 0.02f, 0.01f, 10000.f, "%.3f"))
                    dirty_shape_ = true;
                if (edit_shape_.size_x < 0.01f) {
                    edit_shape_.size_x = 0.01f;
                    dirty_shape_ = true;
                }
            } else if (edit_shape_.type == 3) {
                if (ImGui::DragFloat3("Offset XYZ##es", &edit_shape_.offset_x, 0.02f))
                    dirty_shape_ = true;
                if (ImGui::DragFloat3("Size W/H/D##es", &edit_shape_.size_x, 0.02f))
                    dirty_shape_ = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("Flip Slope##es")) {
                    float sx = std::abs(edit_shape_.size_x);
                    if (sx < 0.01f) sx = 1.f;
                    edit_shape_.size_x = edit_shape_.size_x >= 0.f ? -sx : sx;
                    dirty_shape_ = true;
                }
                int subdiv = (int)std::lround(edit_shape_.detail_a);
                if (subdiv < 1) subdiv = 1;
                if (subdiv > 16) subdiv = 16;
                if (ImGui::SliderInt("Subdivisions##es", &subdiv, 1, 16)) {
                    edit_shape_.detail_a = (float)subdiv;
                    dirty_shape_ = true;
                }
                ImGui::TextDisabled("Wedge direction follows Size X sign (+/-).");
            } else if (edit_shape_.type == 2) {
                if (edit_shape_.detail_a < 0.1f) edit_shape_.detail_a = 100.f;
                if (ImGui::SliderFloat("Face Budget %%##es", &edit_shape_.detail_a, 0.1f, 100.f, "%.1f%%"))
                    dirty_shape_ = true;
                ImGui::TextDisabled("Lower values simplify collision mesh for this shape.");
            } else {
                ImGui::TextDisabled("Uses the full model geometry.");
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty_shape_);
            if (ImGui::Button("Save##es")) {
                SaveModelShape(db, edit_shape_);
                dirty_shape_ = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Revert##es")) {
                edit_shape_  = model_shapes_[sel_shape_];
                dirty_shape_ = false;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
            if (ImGui::Button("Delete##es")) {
                DeleteModelShape(db, edit_shape_.id);
                sel_shape_ = -1;
            }
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("Select a model, or click \"New Model\".");
    }
    ImGui::EndChild();  // end of ##mdl_edit

    // --- Right column: 3D preview ---
    ImGui::SameLine();
    ImGui::BeginChild("##mdl_preview", {0, 0}, true);
    if (!preview_init_ok_) {
        ImGui::TextDisabled("Preview unavailable (shader load failed).");
    } else {
        const MediaModel* show = nullptr;
        if (newModel_ && !pendingModel_.file_path.empty())       show = &pendingModel_;
        else if (selModel_ >= 0 && selModel_ < (int)models_.size()) show = &editModel_;

        if (show) {
            ImGui::Checkbox("Show Collision Overlay", &show_collision_preview_);
            preview_->SetActorScale(show->scale);
            bool path_changed = preview_->CurrentPath() != show->file_path;
            if (path_changed) {
                preview_->LoadModel(show->file_path);
                preview_->SetStaticBlackCutout(show->black_cutout);
                // Seed the in-memory mapping with whatever the DB persisted
                // for this model so the preview comes back textured after
                // reopening the GUE.
                if (preview_material_names_model_ != show->file_path) {
                    previewAiToMedia_ = show->material_map;
                    preview_material_names_model_ = show->file_path;
                }
                // Sync the preview's on-screen UV sliders with the persisted
                // values from the DB so the sidecar's transform is shown as
                // "0/0/1/1" (already baked in) and the user can fine-tune
                // further deltas on top.
                preview_->SetUVTransform(0.f, 0.f, 1.f, 1.f);
            }

            auto buildLookups = [&]() {
                // Index media_materials by name for fast override lookup.
                std::unordered_map<std::string, const MediaMaterial*> byName;
                for (const auto& m : materials_) byName[m.name] = &m;

                // For every distinct aiMaterial name in the loaded model,
                // resolve which media_material to use: user's explicit
                // override first, otherwise same-name match, otherwise none.
                std::vector<PreviewViewport::MaterialLookup> out;
                for (const auto& ai_name : preview_->MaterialNames()) {
                    const MediaMaterial* mm = nullptr;
                    auto it = previewAiToMedia_.find(ai_name);
                    if (it != previewAiToMedia_.end()) {
                        auto nit = byName.find(it->second);
                        if (nit != byName.end()) mm = nit->second;
                    }
                    if (!mm) {
                        auto nit = byName.find(ai_name);
                        if (nit != byName.end()) mm = nit->second;
                    }
                    if (!mm) continue;

                    // Use ai_name as the lookup key so ApplyMaterialsByName
                    // matches the submesh's material_name correctly.
                    PreviewViewport::MaterialLookup l;
                    l.name       = ai_name;
                    l.albedo_rel = mm->albedo_path;
                    l.normal_rel = mm->normal_path;
                    l.orm_rel    = mm->orm_path;
                    out.push_back(std::move(l));
                }
                return out;
            };

            if (path_changed || materialsDirtyForPreview_) {
                preview_->ApplyMaterialsFromMedia(buildLookups());
                materialsDirtyForPreview_ = false;
            }
            {
                std::vector<ModelShape> previewShapes = model_shapes_;
                if (new_shape_) {
                    previewShapes.push_back(pending_shape_);
                } else if (sel_shape_ >= 0 && sel_shape_ < (int)previewShapes.size()) {
                    previewShapes[(size_t)sel_shape_] = edit_shape_;
                }

                std::vector<PreviewViewport::CollisionShape> visShapes;
                visShapes.reserve(previewShapes.size());
                for (const auto& s : previewShapes) {
                    PreviewViewport::CollisionShape cs;
                    cs.type     = s.type;
                    cs.offset_x = s.offset_x;
                    cs.offset_y = s.offset_y;
                    cs.offset_z = s.offset_z;
                    cs.size_x   = s.size_x;
                    cs.size_y   = s.size_y;
                    cs.size_z   = s.size_z;
                    cs.detail_a = s.detail_a;
                    cs.detail_b = s.detail_b;
                    visShapes.push_back(cs);
                }
                preview_->SetCollisionShapes(visShapes);
                preview_->SetCollisionPreviewVisible(show_collision_preview_);
            }
            preview_->DrawImGui();

            // ── Texture diagnostic ───────────────────────────────────────
            {
                const auto& mdl = preview_->GetModel();
                if (mdl.IsLoaded()) {
                    int missing = 0;
                    for (const auto& m : mdl.meshes())
                        if (!m.tex_albedo) ++missing;
                    if (missing > 0) {
                        ImGui::Separator();
                        ImGui::TextColored({1.f, 0.65f, 0.1f, 1.f},
                            "No embedded texture (%d/%d submesh(es) missing albedo).",
                            missing, (int)mdl.meshes().size());
                        ImGui::TextDisabled(
                            "Possible causes:\n"
                            "  1. Textures are external files — use 'Import Folder' to copy them alongside the mesh.\n"
                            "  2. Unreal export: enable 'Embed Textures' option (or check Texture Export Method = PNG).\n"
                            "  3. Assimp can't find the texture key — assign a Material below as a workaround.");
                    }
                }
            }


            // ── Material mapping UI ──────────────────────────────────────
            auto ai_names = preview_->MaterialNames();
            if (!ai_names.empty()) {
                ImGui::Separator();
                ImGui::TextColored({0.6f, 0.85f, 1.f, 1.f}, "Material mapping");
                ImGui::TextDisabled("Link each material slot inside the model to an "
                                    "imported media_material.");

                bool mapping_changed = false;
                for (const auto& ai : ai_names) {
                    ImGui::PushID(ai.c_str());
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%s", ai.c_str());
                    ImGui::SameLine(140.f);

                    auto it = previewAiToMedia_.find(ai);
                    std::string current = (it != previewAiToMedia_.end()) ? it->second : ai;
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::BeginCombo("##map", current.c_str())) {
                        if (ImGui::Selectable("(auto — match by name)",
                                              it == previewAiToMedia_.end())) {
                            previewAiToMedia_.erase(ai);
                            mapping_changed = true;
                        }
                        for (const auto& m : materials_) {
                            bool sel = (current == m.name);
                            if (ImGui::Selectable(m.name.c_str(), sel)) {
                                previewAiToMedia_[ai] = m.name;
                                mapping_changed = true;
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                if (mapping_changed) {
                    preview_->ApplyMaterialsFromMedia(buildLookups());

                    // Persist to the model row so the mapping survives
                    // GUE restarts and is available to Actor Def / Zones
                    // rendering without re-editing.
                    if (selModel_ >= 0 && selModel_ < (int)models_.size()) {
                        editModel_.material_map = previewAiToMedia_;
                        models_[selModel_].material_map = previewAiToMedia_;
                        SaveModel(db, editModel_);
                    }
                }
            }
        } else {
            preview_->SetCollisionShapes({});
            preview_->SetCollisionPreviewVisible(false);
            preview_->Clear();
            ImGui::TextDisabled("No model selected.");
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Materials sub-tab
// ---------------------------------------------------------------------------

static bool DrawMaterialFields(MediaMaterial& m) {
    bool changed = false;
    if (InputString("Name", m.name)) changed = true;

    // Route texture imports into a per-material subfolder so every material's
    // albedo/normal/ORM stay grouped on disk (matches user expectation:
    // "importar texturas que ja ficam em pastas separadas").
    std::string folder = SafeFolderName(m.name);
    std::string subdir = folder.empty() ? std::string("textures")
                                        : std::string("textures/") + folder;

    if (PathField("Albedo", m.albedo_path, "Texture", "png,jpg,jpeg,tga,bmp", subdir.c_str()))
        changed = true;
    if (PathField("Normal", m.normal_path, "Texture", "png,jpg,jpeg,tga,bmp", subdir.c_str()))
        changed = true;
    if (PathField("ORM",    m.orm_path,    "Texture", "png,jpg,jpeg,tga,bmp", subdir.c_str()))
        changed = true;

    ImGui::Separator();
    ImGui::TextUnformatted("PBR Factors");
    float col[3] = {m.albedo_r, m.albedo_g, m.albedo_b};
    if (ImGui::ColorEdit3("Albedo Tint", col)) {
        m.albedo_r = col[0]; m.albedo_g = col[1]; m.albedo_b = col[2];
        changed = true;
    }
    if (ImGui::SliderFloat("Roughness", &m.roughness, 0.f, 1.f)) changed = true;
    if (ImGui::SliderFloat("Metallic",  &m.metallic,  0.f, 1.f)) changed = true;

    ImGui::Separator();
    ImGui::TextUnformatted("Terrain");
    if (ImGui::SliderFloat("Normal Strength", &m.normal_strength, 0.f, 6.f, "%.2f")) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Intensity of the normal map when this material is used on terrain.\n"
            "1.0 = physically correct. 2-3 = recommended for most assets.\n"
            "Higher values exaggerate bump detail; 0 = flat surface.");

    ImGui::Separator();
    ImGui::TextUnformatted("Cutout");
    if (ImGui::Checkbox("Black Cutout##bco", &m.black_cutout)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Discard pixels where the albedo luminance is near pure black.\n"
            "Use for hair, foliage, or fences with a black alpha mask convention.\n"
            "Adjust the global threshold in Settings \xe2\x86\x92 Rendering.");

    ImGui::TextDisabled("ORM = Occlusion(R) / Roughness(G) / Metallic(B) packed texture.");
    return changed;
}

void MediaTab::DrawMaterials(sqlite3* db) {
    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Material")) {
        pendingMat_ = {};
        newMat_  = true;
        selMat_  = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Import PBR Material (folder)...")) {
        std::string folder = PickFolder("Pick a folder with PBR textures");
        if (!folder.empty()) {
            importGroups_.clear();
            importSourceFolder_ = folder;
            if (ScanTextureFolder(folder, importGroups_) && !importGroups_.empty()) {
                // Default output subdir = source folder's basename, sanitized.
                std::string base = folder;
                size_t slash = base.find_last_of("/\\");
                if (slash != std::string::npos) base = base.substr(slash + 1);
                for (char& c : base) if (c == ' ' || c == '-') c = '_';
                std::strncpy(importSubdir_, base.c_str(), sizeof(importSubdir_) - 1);
                importSubdir_[sizeof(importSubdir_) - 1] = 0;
                showImportDlg_ = true;
            } else {
                showPbrScanFailDlg_ = true;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import Texture(s)...")) ImportLooseTextures_(db);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    // ── PBR scan-failed warning modal ─────────────────────────────────────
    if (showPbrScanFailDlg_) ImGui::OpenPopup("PBR Scan Failed");
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({480.f, 0.f}, ImGuiCond_Appearing);
    }
    if (ImGui::BeginPopupModal("PBR Scan Failed", &showPbrScanFailDlg_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored({1.f, 0.6f, 0.2f, 1.f}, "No PBR groups found in:");
        ImGui::TextDisabled("  %s", importSourceFolder_.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("The scanner looks for role keywords in filenames:");
        ImGui::BulletText("Albedo / BaseColor / Base_color / Diffuse");
        ImGui::BulletText("Normal / Normal_OpenGL / Normal_DirectX");
        ImGui::BulletText("Roughness / Metalness / Metallic / AO / AmbientOcclusion");
        ImGui::BulletText("Opacity / Alpha");
        ImGui::Spacing();
        ImGui::TextUnformatted("Each group needs at least an Albedo file.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("For loose images without PBR roles, use \"Import Texture(s)...\" instead.");
        ImGui::Spacing();
        if (ImGui::Button("OK", {120.f, 0.f})) {
            showPbrScanFailDlg_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Import-folder modal dialog ────────────────────────────────────────
    if (showImportDlg_) ImGui::OpenPopup("Import PBR Material");
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({640.f, 0.f}, ImGuiCond_Appearing);
    }
    if (ImGui::BeginPopupModal("Import PBR Material", &showImportDlg_,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Source: %s", importSourceFolder_.c_str());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputText("Target subfolder (under assets/materials/)##ti",
                         importSubdir_, sizeof(importSubdir_));
        ImGui::Checkbox("Pack AO+Rough+Metal into ORM (recommended)", &importOpts_.pack_orm);
        ImGui::Checkbox("Flip Y channel on Normal_DirectX",            &importOpts_.flip_normal_dx);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Detected groups:");
        ImGui::Spacing();

        if (ImGui::BeginTable("##ti_groups", 8,
                ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Use");
            ImGui::TableSetupColumn("Prefix / name");
            ImGui::TableSetupColumn("Albedo");
            ImGui::TableSetupColumn("Normal");
            ImGui::TableSetupColumn("Rough");
            ImGui::TableSetupColumn("Metal");
            ImGui::TableSetupColumn("AO");
            ImGui::TableSetupColumn("Alpha");
            ImGui::TableHeadersRow();

            auto cell = [](bool have, const char* extra = "") {
                if (have) ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "✓%s", extra);
                else      ImGui::TextDisabled("—");
            };

            for (size_t i = 0; i < importGroups_.size(); ++i) {
                auto& g = importGroups_[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID((int)i);
                ImGui::Checkbox("##on", &g.enabled);
                ImGui::TableSetColumnIndex(1);
                char name[96];
                std::strncpy(name, g.material_name.c_str(), 95); name[95] = 0;
                ImGui::SetNextItemWidth(150.f);
                if (ImGui::InputText("##mn", name, sizeof(name))) g.material_name = name;
                ImGui::TableSetColumnIndex(2);  cell(!g.albedo_src.empty());
                ImGui::TableSetColumnIndex(3);  cell(!g.normal_src.empty(), g.normal_is_dx ? " DX" : "");
                ImGui::TableSetColumnIndex(4);  cell(!g.roughness_src.empty());
                ImGui::TableSetColumnIndex(5);  cell(!g.metallic_src.empty());
                ImGui::TableSetColumnIndex(6);  cell(!g.ao_src.empty());
                ImGui::TableSetColumnIndex(7);  cell(!g.alpha_src.empty());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();

        int nEnabled = 0;
        for (const auto& g : importGroups_) if (g.enabled) ++nEnabled;
        if (ImGui::Button("Import##ti")) {
            importOpts_.target_subdir = importSubdir_;
            importOpts_.source_root   = importSourceFolder_;
            std::fprintf(stderr, "[tex-import] starting: %zu group(s) src='%s' sub='%s'\n",
                         importGroups_.size(), importSourceFolder_.c_str(), importSubdir_);
            int done = 0;
            for (auto& g : importGroups_) {
                std::fprintf(stderr, "[tex-import] group '%s' enabled=%d albedo='%s'\n",
                             g.material_name.c_str(), (int)g.enabled, g.albedo_src.c_str());
                if (!g.enabled) continue;
                if (!ImportTextureGroup(g, importOpts_)) continue;
                MediaMaterial m;
                m.name        = g.material_name;
                m.albedo_path = g.albedo_rel;
                m.normal_path = g.normal_rel;
                m.orm_path    = g.orm_rel;   // empty when pack_orm=false
                SaveMaterial(db, m);
                ++done;
            }
            std::snprintf(statusMsg_, sizeof(statusMsg_),
                          "Imported %d material%s.", done, done == 1 ? "" : "s");
            needFetch_ = true;
            showImportDlg_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d group%s enabled", nEnabled, nEnabled == 1 ? "" : "s");
        ImGui::SameLine(0, 12.f);
        if (ImGui::Button("Cancel##ti")) {
            showImportDlg_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextItemWidth(260);
    ImGui::InputText("##mat_filter", filterMat_, sizeof(filterMat_));
    ImGui::SameLine(); ImGui::TextDisabled("filter");
    DrawFolderList(materials_, selMat_, filterMat_, "##mat_list", 260,
        [&](int i) { editMat_ = materials_[i]; dirtyMat_ = false; newMat_ = false; },
        [&](int i) -> std::string {
            const std::string& p = materials_[i].albedo_path;
            if (p.empty()) return materials_[i].name;
            std::string parent = std::filesystem::path(p).parent_path().generic_string();
            static const std::string kTex = "assets/textures/";
            static const std::string kMat = "assets/materials/";
            if (parent.rfind(kTex, 0) == 0) parent = parent.substr(kTex.size());
            else if (parent.rfind(kMat, 0) == 0) parent = parent.substr(kMat.size());
            if (parent.empty()) return materials_[i].name;
            return parent + "/" + materials_[i].name;
        },
        [&](const std::string& fp) { OpenFolderDeleteModal_(db, fp, false); });
    ImGui::SameLine();

    ImGui::BeginChild("##mat_edit", {0, 0}, true);
    if (newMat_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Material");
        ImGui::Separator();
        DrawMaterialFields(pendingMat_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveMaterial(db, pendingMat_); newMat_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newMat_ = false;
    } else if (selMat_ >= 0 && selMat_ < (int)materials_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editMat_.id, editMat_.name.c_str());
        ImGui::Separator();
        if (DrawMaterialFields(editMat_)) dirtyMat_ = true;
        ImGui::Spacing();

        // ── Albedo texture preview ─────────────────────────────────────────
        if (editMat_.id != preview_mat_tex_id_) {
            if (preview_mat_tex_) { glDeleteTextures(1, &preview_mat_tex_); preview_mat_tex_ = 0; }
            preview_mat_tex_id_ = editMat_.id;
            if (!editMat_.albedo_path.empty()) {
                std::string abs = ResolveClientAsset(editMat_.albedo_path);
                int w = 0, h = 0, ch = 0;
                unsigned char* px = stbi_load(abs.c_str(), &w, &h, &ch, 4);
                if (px) {
                    glGenTextures(1, &preview_mat_tex_);
                    glBindTexture(GL_TEXTURE_2D, preview_mat_tex_);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, px);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    stbi_image_free(px);
                }
            }
        }
        if (preview_mat_tex_)
            ImGui::Image((ImTextureID)(intptr_t)preview_mat_tex_, ImVec2(256.f, 256.f));
        else if (!editMat_.albedo_path.empty())
            ImGui::TextDisabled("(preview load failed)");
        else
            ImGui::TextDisabled("(no albedo texture)");
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirtyMat_);
        if (ImGui::Button("Save")) { SaveMaterial(db, editMat_); dirtyMat_ = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editMat_ = materials_[selMat_]; dirtyMat_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) DeleteMaterial(db, editMat_.id);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Select a material, or click \"New Material\".");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Anim Clips sub-tab
// ---------------------------------------------------------------------------

static bool DrawClipFields(MediaAnimClip& c) {
    bool changed = false;
    if (InputString("Name", c.name)) changed = true;
    if (PathField("Source", c.source_path, "Animation File",
                  "fbx,glb,gltf,dae,b3d", "anims")) changed = true;
    if (InputString("Clip Override", c.clip_override)) changed = true;
    ImGui::TextDisabled("Source empty = clip is embedded in the actor's Body model.");
    ImGui::TextDisabled("Clip Override empty = use first clip inside the source file.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Frame Range & Timing");
    if (ImGui::InputInt("Start Frame", &c.start_frame)) changed = true;
    if (c.start_frame < 0) c.start_frame = 0;

    if (ImGui::InputInt("End Frame", &c.end_frame)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("-1 = play to end of file (Scenario A).\n"
                          "Set a positive frame number to slice (Scenario B).");
    ImGui::SameLine();
    ImGui::TextDisabled("(-1 = to end)");

    if (ImGui::InputFloat("FPS", &c.fps, 1.f, 5.f, "%.1f")) changed = true;
    if (c.fps < 0.1f) c.fps = 0.1f;
    return changed;
}

void MediaTab::DrawAnimClips(sqlite3* db) {
    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Clip")) {
        pendingClip_ = {};
        newClip_  = true;
        selClip_  = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::SetNextItemWidth(260);
    ImGui::InputText("##clip_filter", filterClip_, sizeof(filterClip_));
    ImGui::SameLine(); ImGui::TextDisabled("filter");
    DrawFolderList(clips_, selClip_, filterClip_, "##clip_list", 260,
        [&](int i) { editClip_ = clips_[i]; dirtyClip_ = false; newClip_ = false;
                     events_loaded_for_clip_ = -1; });
    ImGui::SameLine();

    ImGui::BeginChild("##clip_edit", {0, 0}, true);
    if (newClip_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Animation Clip");
        ImGui::Separator();
        DrawClipFields(pendingClip_);
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveAnimClip(db, pendingClip_); newClip_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newClip_ = false;
    } else if (selClip_ >= 0 && selClip_ < (int)clips_.size()) {
        ImGui::Text("Editing: [id=%d] %s", editClip_.id, editClip_.name.c_str());
        ImGui::Separator();
        if (DrawClipFields(editClip_)) dirtyClip_ = true;
        ImGui::Spacing();

        ImGui::BeginDisabled(!dirtyClip_);
        if (ImGui::Button("Save")) { SaveAnimClip(db, editClip_); dirtyClip_ = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) { editClip_ = clips_[selClip_]; dirtyClip_ = false; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete")) DeleteAnimClip(db, editClip_.id);
        ImGui::PopStyleColor();

        // --- Animation Events section ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Animation Events");
        ImGui::TextDisabled("Frame markers that fire gameplay callbacks (hitbox, sfx, vfx, footstep).");

        // Reload events when the selected clip changes.
        if (events_loaded_for_clip_ != editClip_.id) {
            LoadEventsForClip(db, editClip_.id);
            events_loaded_for_clip_ = editClip_.id;
            sel_event_              = -1;
        }

        ImGui::BeginChild("##events_list", ImVec2(0, 120), true);
        for (int i = 0; i < (int)clip_events_.size(); ++i) {
            const auto& ev = clip_events_[i];
            bool selected = (sel_event_ == i);
            char elabel[128];
            std::snprintf(elabel, sizeof(elabel), "[frame %d] %s", ev.frame, ev.event_type.c_str());
            if (ImGui::Selectable(elabel, selected)) {
                sel_event_   = i;
                edit_event_  = ev;
                dirty_event_ = false;
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("+ Add Event")) {
            MediaAnimEvent new_ev;
            new_ev.clip_id     = editClip_.id;
            new_ev.frame       = 0;
            new_ev.event_type  = "sfx";
            new_ev.payload     = "";
            SaveAnimEvent(db, new_ev);
            LoadEventsForClip(db, editClip_.id);
            sel_event_ = (int)clip_events_.size() - 1;
            if (sel_event_ >= 0) edit_event_ = clip_events_[sel_event_];
            dirty_event_ = false;
        }

        if (sel_event_ >= 0 && sel_event_ < (int)clip_events_.size()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
            if (ImGui::Button("Delete Event")) {
                DeleteAnimEvent(db, edit_event_.id);
                LoadEventsForClip(db, editClip_.id);
                sel_event_   = -1;
                dirty_event_ = false;
            }
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::TextUnformatted("Edit event:");
            if (ImGui::InputInt("Frame##ev", &edit_event_.frame)) dirty_event_ = true;
            if (edit_event_.frame < 0) edit_event_.frame = 0;

            static const char* kEventTypes[] = {"sfx", "vfx", "hitbox", "footstep"};
            int evt_idx = 0;
            for (int i = 0; i < 4; ++i)
                if (edit_event_.event_type == kEventTypes[i]) { evt_idx = i; break; }
            if (ImGui::Combo("Type##ev", &evt_idx, kEventTypes, 4)) {
                edit_event_.event_type = kEventTypes[evt_idx];
                dirty_event_ = true;
            }

            char payload_buf[512];
            std::strncpy(payload_buf, edit_event_.payload.c_str(), sizeof(payload_buf) - 1);
            payload_buf[sizeof(payload_buf) - 1] = '\0';
            if (ImGui::InputText("Payload (JSON)##ev", payload_buf, sizeof(payload_buf))) {
                edit_event_.payload = payload_buf;
                dirty_event_        = true;
            }
            ImGui::TextDisabled("e.g.: {\"radius\":1.5,\"damage_mult\":1.0} for hitbox");

            ImGui::BeginDisabled(!dirty_event_);
            if (ImGui::Button("Save Event")) {
                clip_events_[sel_event_] = edit_event_;
                SaveAnimEvent(db, edit_event_);
                dirty_event_ = false;
            }
            ImGui::EndDisabled();
        }
    } else {
        ImGui::TextDisabled("Select a clip, or click \"New Clip\".");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Actor Defs sub-tab
// ---------------------------------------------------------------------------

static const char* lookupName(const std::vector<std::pair<int, std::string>>& v, int id,
                              const char* fallback) {
    for (auto& p : v) if (p.first == id) return p.second.c_str();
    return fallback;
}

void MediaTab::DrawActorDefs(sqlite3* db) {
    if (!prefs_loaded_) LoadPrefs_();
    EnsurePreview(preview_, preview_init_ok_, engine_, pipeline_);
    LoadDropListOptions(db);

    auto modelList = idNameList(models_);
    auto matList   = idNameList(materials_);
    auto dropLists = drop_list_options_;

    if (ImGui::Button("Refresh")) needFetch_ = true;
    ImGui::SameLine();
    if (ImGui::Button("New Actor Def")) {
        pendingActorDef_ = {};
        newActorDef_ = true;
        selActorDef_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", statusMsg_);
    ImGui::Separator();

    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##ad_filter", filterActorDef_, sizeof(filterActorDef_));
    ImGui::SameLine(); ImGui::TextDisabled("filter");
    DrawFolderList(actor_defs_, selActorDef_, filterActorDef_, "##ad_list", 220,
        [&](int i) { editActorDef_ = actor_defs_[i]; dirtyActorDef_ = false; newActorDef_ = false; });
    ImGui::SameLine();

    float ad_total_w = ImGui::GetContentRegionAvail().x;
    if (ad_preview_w_ <= 0.f)
        ad_preview_w_ = std::max(280.f, ad_total_w * 0.40f);  // first-frame init
    float ad_props_w = ad_total_w - ad_preview_w_ - 8.f;

    ImGui::BeginChild("##ad_edit", {ad_props_w, 0}, true);

    if (newActorDef_) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "New Actor Def");
        ImGui::Separator();
        InputString("Name", pendingActorDef_.name);
        ImGui::TextDisabled("Create the actor def first, then open it to add mesh slots and animations.");
        ImGui::Spacing();
        if (ImGui::Button("Create")) { SaveActorDef(db, pendingActorDef_); newActorDef_ = false; }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) newActorDef_ = false;
    }
    else if (selActorDef_ >= 0 && selActorDef_ < (int)actor_defs_.size()) {
        ActorDef& d = editActorDef_;
        ImGui::Text("Editing: [id=%d] %s", d.id, d.name.c_str());
        ImGui::Separator();

        if (InputString("Name", d.name)) dirtyActorDef_ = true;

        // Scale — drives the preview live; multiplies each mesh slot's
        // model scale at render/spawn time. 0.5 = filhote, 2.0 = pai grandão.
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("Scale", &d.scale, 0.1f, 5.f, "%.2fx"))
            dirtyActorDef_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("1x##sc_reset")) { d.scale = 1.f; dirtyActorDef_ = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Multiplies the model's import-scale.\n"
                              "Same model → filhote (0.5) and pai grandão (2.0).");

        ImGui::SetNextItemWidth(200);
        if (ImGui::InputFloat("Yaw Offset°##yo", &d.yaw_offset, 45.f, 90.f, "%.0f°"))
            dirtyActorDef_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("0°##yo_reset")) { d.yaw_offset = 0.f; dirtyActorDef_ = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("180°##yo_flip")) { d.yaw_offset = 180.f; dirtyActorDef_ = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extra Y-axis rotation (degrees) baked into this actor def.\n"
                              "Use 180 to fix a model that was exported facing backwards.\n"
                              "Applied after world yaw, so the model always faces correctly.");

        ImGui::SetNextItemWidth(200);
        if (ImGui::InputFloat("Y Offset##yoff", &d.y_offset, 0.1f, 0.5f, "%.2f m"))
            dirtyActorDef_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("0##yoff_reset")) { d.y_offset = 0.f; dirtyActorDef_ = true; }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Vertical offset (metres) added to the model's position.\n"
                              "Use a positive value to lift a model that appears underground.");

        ImGui::Spacing();

        // Gameplay defaults — copied into npc_spawns when this actor is
        // placed in a zone. Leave empty/zero to force the user to fill in.
        if (ImGui::CollapsingHeader("Gameplay defaults",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (InputString("Default name",  d.default_name))  dirtyActorDef_ = true;
            if (InputString("Default race",  d.default_race))  dirtyActorDef_ = true;
            if (InputString("Default class", d.default_class)) dirtyActorDef_ = true;

            ImGui::SetNextItemWidth(120);
            if (ImGui::InputInt("Level##lvl", &d.default_level))
                dirtyActorDef_ = true;
            ImGui::SameLine(0, 16.f);
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("HP##hp", &d.default_hp)) dirtyActorDef_ = true;
            ImGui::SameLine(0, 16.f);
            ImGui::SetNextItemWidth(100);
            if (ImGui::InputInt("EP##ep", &d.default_ep)) dirtyActorDef_ = true;

            static const char* kAggroNames[] = {
                "Passive", "Defensive", "Aggressive", "Dialog only"
            };
            ImGui::SetNextItemWidth(150);
            if (ImGui::Combo("Aggressiveness",
                             &d.default_aggressiveness, kAggroNames, 4))
                dirtyActorDef_ = true;

            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("Aggro range##ar",
                                  &d.default_aggro_range, 0.f, 0.f, "%.1f"))
                dirtyActorDef_ = true;
            ImGui::SameLine(0, 16.f);
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputFloat("Attack range##atk",
                                  &d.default_attack_range, 0.f, 0.f, "%.1f"))
                dirtyActorDef_ = true;

            ImGui::SetNextItemWidth(140);
            if (ImGui::InputInt("Respawn (ms)##rsp", &d.default_respawn_ms,
                                1000, 5000))
                dirtyActorDef_ = true;
            ImGui::SameLine();
            ImGui::TextDisabled("(0 = permanent death)");

            if (ImGui::Checkbox("Playable",   &d.is_playable))    dirtyActorDef_ = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Mountable",  &d.is_mountable))   dirtyActorDef_ = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Has dialog", &d.is_interactive)) dirtyActorDef_ = true;

            if (d.is_playable) {
                std::vector<std::pair<int,std::string>> spawnList;
                spawnList.reserve(player_spawns_.size());
                for (auto& ps : player_spawns_)
                    spawnList.emplace_back(ps.id, ps.area_name + " / " + ps.name);
                if (ui::SearchableComboId("Initial Spawn##ispawn", d.initial_spawn_id,
                                          spawnList, "(none - default spawn)"))
                    dirtyActorDef_ = true;
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Loot");
            if (ComboId("Drop List", d.loot_table_id, dropLists, "(none)"))
                dirtyActorDef_ = true;
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!dirtyActorDef_);
        if (ImGui::Button("Save")) {
            SaveActorDef(db, d);
            for (auto& slot : d.mesh_slots) SaveMeshSlot(db, slot);
            dirtyActorDef_ = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            editActorDef_  = actor_defs_[selActorDef_];
            dirtyActorDef_ = false;
        }
        ImGui::SameLine();
        // Duplicate — clones def + mesh slots + anim map under a "(copy)"
        // name. Core plug-and-play move for making skin variants of the
        // same model (Red Knight / Blue Knight), or level variants.
        if (ImGui::Button("Duplicate")) {
            DuplicateActorDef(db, d.id);
            // DuplicateActorDef already refetched + selected the new def,
            // but the outer frame still holds a stale iterator to d. Bail
            // out of this frame — next frame draws the new def cleanly.
            ImGui::EndChild();
            return;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f});
        if (ImGui::Button("Delete Def")) DeleteActorDef(db, d.id);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();

        // --- Mesh Slots section ---
        ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Mesh Slots");
        ImGui::BeginChild("##meshes", {0, 220}, true);

        if (ImGui::BeginTable("##slot_tbl", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Model");
            ImGui::TableSetupColumn("Material");
            ImGui::TableSetupColumn("##edit", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("##del",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < d.mesh_slots.size(); ++i) {
                ActorMeshSlot& s = d.mesh_slots[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ActorSlotName(s.slot));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    lookupName(modelList, s.model_id, "(none)"));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(
                    s.material_id == 0 ? "(embedded)" :
                    lookupName(matList, s.material_id, "(?)"));

                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("Edit")) {
                    // Inline-edit: put into a popup.
                    ImGui::OpenPopup("edit_slot");
                }
                if (ImGui::BeginPopup("edit_slot")) {
                    if (ImGui::Combo("Slot", &s.slot, kSlotNames, kSlotCount))
                        dirtyActorDef_ = true;
                    if (ComboId("Model", s.model_id, modelList))
                        dirtyActorDef_ = true;
                    ImGui::Separator();
                    ImGui::TextUnformatted("Global material (all parts):");
                    if (ComboId("##mat_global", s.material_id, matList, "(embedded)"))
                        dirtyActorDef_ = true;

                    // Per-part (submesh) material section.
                    // Uses material names from the preview when its model matches
                    // this slot; otherwise shows only already-saved entries.
                    std::vector<std::string> part_names;
                    static int s_last_logged_slot_id = -1;
                    const bool should_log_diag = (s_last_logged_slot_id != s.id);
                    {
                        const MediaModel* slot_mdl = nullptr;
                        for (const auto& mm : models_)
                            if (mm.id == s.model_id) { slot_mdl = &mm; break; }

                        const bool has_slot_mdl   = slot_mdl != nullptr;
                        const bool has_preview     = preview_ != nullptr;
                        const bool path_nonempty   = has_slot_mdl && !slot_mdl->file_path.empty();
                        const std::string cur_path = (has_preview && preview_)
                                                     ? preview_->CurrentPath() : "(no preview)";
                        const bool paths_match     = has_preview && path_nonempty &&
                                                     cur_path == slot_mdl->file_path;

                        // Log once per popup open (throttled by slot id change).
                        if (should_log_diag) {
                            s_last_logged_slot_id = s.id;
                            std::fprintf(stderr,
                                "[per-part-diag] popup open: slot_id=%d model_id=%d\n"
                                "  slot_mdl found=%d  path='%s'\n"
                                "  preview_=%p  preview.CurrentPath='%s'\n"
                                "  path_nonempty=%d  paths_match=%d (decides branch)\n",
                                s.id, s.model_id,
                                (int)has_slot_mdl,
                                slot_mdl ? slot_mdl->file_path.c_str() : "(null)",
                                (void*)preview_.get(),
                                cur_path.c_str(),
                                (int)path_nonempty, (int)paths_match);
                        }

                        const bool preview_matches = paths_match;
                        if (preview_matches) {
                            part_names = preview_->MaterialNames();
                            if (should_log_diag) {
                                std::fprintf(stderr,
                                    "[per-part-diag]   BRANCH=preview  MaterialNames count=%zu [",
                                    part_names.size());
                                for (size_t pi = 0; pi < part_names.size(); ++pi)
                                    std::fprintf(stderr, "%s%s", pi ? ", " : "",
                                                 part_names[pi].c_str());
                                std::fprintf(stderr, "]\n");
                            }
                        } else {
                            for (const auto& [nm, _] : s.submesh_materials)
                                part_names.push_back(nm);
                            if (should_log_diag) {
                                std::fprintf(stderr,
                                    "[per-part-diag]   BRANCH=saved_keys  saved entries=%zu [",
                                    part_names.size());
                                for (size_t pi = 0; pi < part_names.size(); ++pi)
                                    std::fprintf(stderr, "%s%s", pi ? ", " : "",
                                                 part_names[pi].c_str());
                                std::fprintf(stderr, "]\n");
                            }
                        }
                    }
                    if (!part_names.empty()) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Per-part materials:");
                        for (const auto& part : part_names) {
                            ImGui::PushID(part.c_str());
                            int cur_id = 0;
                            auto it = s.submesh_materials.find(part);
                            if (it != s.submesh_materials.end()) cur_id = it->second;
                            ImGui::SetNextItemWidth(160.f);
                            if (ComboId(part.c_str(), cur_id, matList, "(model default)")) {
                                s.submesh_materials[part] = cur_id;
                                dirtyActorDef_ = true;
                            }
                            ImGui::PopID();
                        }
                    } else {
                        if (should_log_diag) {
                            std::fprintf(stderr,
                                "[per-part-diag]   SECTION=SKIPPED (part_names empty)\n");
                        }
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Save##es")) {
                        SaveMeshSlot(db, s);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close##es")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                ImGui::PushID((int)(i + 10000));
                if (ImGui::SmallButton("Del")) DeleteMeshSlot(db, s.id);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Add slot:");
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("##nslot", &newSlotSlot_, kSlotNames, kSlotCount);
        ImGui::SameLine();

        int newModelId = newSlotModelIdx_ >= 0 && newSlotModelIdx_ < (int)modelList.size()
                       ? modelList[newSlotModelIdx_].first : 0;
        int newMatId   = newSlotMatIdx_   >= 0 && newSlotMatIdx_   < (int)matList.size()
                       ? matList[newSlotMatIdx_].first : 0;

        ImGui::SetNextItemWidth(160);
        if (ComboId("##nmdl", newModelId, modelList)) {
            for (int k = 0; k < (int)modelList.size(); ++k)
                if (modelList[k].first == newModelId) { newSlotModelIdx_ = k; break; }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ComboId("##nmat", newMatId, matList, "(embedded)")) {
            newSlotMatIdx_ = -1;
            for (int k = 0; k < (int)matList.size(); ++k)
                if (matList[k].first == newMatId) { newSlotMatIdx_ = k; break; }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Slot")) {
            if (newModelId == 0) {
                std::snprintf(statusMsg_, sizeof(statusMsg_),
                              "Pick a model before adding a slot.");
            } else {
                ActorMeshSlot s;
                s.actor_def_id = d.id;
                s.slot         = newSlotSlot_;
                s.model_id     = newModelId;
                s.material_id  = newMatId;
                SaveMeshSlot(db, s);
                // reset picker so user sees the action completed
                newSlotModelIdx_ = -1;
                newSlotMatIdx_   = -1;
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::Separator();

        // --- Animations section (inline: action + source file + clip name + playback) ---
        // No more visiting an Anim Clips registry — each row carries the
        // full definition of the animation it needs.  Leave source_path
        // empty to use a clip embedded in the model.
        ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Animations");
        ImGui::BeginChild("##anims", {0, ad_anim_h_}, true);

        if (ImGui::BeginTable("##anim_tbl", 11,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("Action",    ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Source file");
            ImGui::TableSetupColumn("Clip name", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Start Fr",  ImGuiTableColumnFlags_WidthFixed,  60);
            ImGui::TableSetupColumn("End Fr",    ImGuiTableColumnFlags_WidthFixed,  60);
            ImGui::TableSetupColumn("Loop",      ImGuiTableColumnFlags_WidthFixed,  40);
            ImGui::TableSetupColumn("Speed",     ImGuiTableColumnFlags_WidthFixed,  60);
            ImGui::TableSetupColumn("Blend In",  ImGuiTableColumnFlags_WidthFixed,  65);
            ImGui::TableSetupColumn("Return To", ImGuiTableColumnFlags_WidthFixed,  90);
            ImGui::TableSetupColumn("Priority",  ImGuiTableColumnFlags_WidthFixed,  60);
            ImGui::TableSetupColumn("##ops",     ImGuiTableColumnFlags_WidthFixed,  90);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < d.anim_map.size(); ++i) {
                ActorAnimMap& a = d.anim_map[i];
                ImGui::PushID((int)(i + 20000));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ui::SearchableComboString("##act", a.action, anim_vocab_names_, "(none)");
                if (!a.action.empty() && !VocabContains(a.action)) {
                    ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.f}, "(not in vocabulary)");
                }

                ImGui::TableNextColumn();
                PathField("##src", a.source_path, "Animation", "glb,fbx,dae,b3d", "anims");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                InputString("##co", a.clip_override, 64);

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputInt("##sf", &a.start_frame);
                if (a.start_frame < 0) a.start_frame = 0;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputInt("##ef", &a.end_frame);
                if (a.end_frame < -1) a.end_frame = -1;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("-1 = play to end of clip");

                ImGui::TableNextColumn();
                ImGui::Checkbox("##loop", &a.loop);

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputFloat("##spd", &a.speed, 0.f, 0.f, "%.2f");
                if (a.speed < 0.01f) a.speed = 0.01f;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputFloat("##bi", &a.blend_in, 0.f, 0.f, "%.2f");
                if (a.blend_in < 0.f) a.blend_in = 0.f;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                InputString("##rt", a.return_to, 64);

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputInt("##pri", &a.priority);
                if (a.priority < 0)  a.priority = 0;
                if (a.priority > 99) a.priority = 99;

                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Save")) {
                    if (a.action.empty()) {
                        std::snprintf(statusMsg_, sizeof(statusMsg_),
                                      "Animation needs an action name.");
                    } else {
                        SaveAnimMap(db, a);
                    }
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, {0.55f, 0.1f, 0.1f, 1.f});
                if (ImGui::SmallButton("Del")) DeleteAnimMap(db, a.id);
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("+ Add animation")) {
            ActorAnimMap a;
            a.actor_def_id  = d.id;
            a.action        = "Idle";
            a.source_path   = "";
            a.clip_override = "";
            a.loop          = true;
            a.speed         = 1.f;
            a.blend_in      = 0.15f;
            a.return_to     = "";
            a.priority      = 0;
            SaveAnimMap(db, a);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Standard Actions")) {
            struct StdAction { const char* action; bool loop; int priority; const char* return_to; };
            static const StdAction kStdActions[] = {
                {"Idle",   true,  0,  ""},
                {"Walk",   true,  1,  ""},
                {"Run",    true,  1,  ""},
                {"Jump",   false, 2,  "Idle"},
                {"Attack", false, 2,  "Idle"},
                {"Cast",   false, 2,  "Idle"},
                {"Hit",    false, 3,  "Idle"},
                {"Death",  false, 99, ""},
            };
            for (const auto& sa : kStdActions) {
                bool exists = false;
                for (const auto& am : d.anim_map)
                    if (am.action == sa.action) { exists = true; break; }
                if (!exists) {
                    ActorAnimMap new_am;
                    new_am.actor_def_id = d.id;
                    new_am.action       = sa.action;
                    new_am.clip_id      = 0;
                    new_am.loop         = sa.loop;
                    new_am.priority     = sa.priority;
                    new_am.return_to    = sa.return_to;
                    new_am.speed        = 1.f;
                    new_am.blend_in     = 0.15f;
                    SaveAnimMap(db, new_am);
                }
            }
            FetchAll(db);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Leave Source empty for a clip embedded in the Body model.");

        ImGui::EndChild(); // end ##anims

        // Horizontal splitter — drag to resize the Animations table height.
        ImGui::InvisibleButton("##anim_hsplit", {-1.f, 6.f});
        if (ImGui::IsItemActive()) {
            ad_anim_h_ += ImGui::GetIO().MouseDelta.y;
            ad_anim_h_  = std::clamp(ad_anim_h_, 120.f, 800.f);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemDeactivated()) SavePrefs_();

        ImGui::Spacing();
        ImGui::Separator();

        // --- Socket Bindings section (B3a) ---
        ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Sockets");
        ImGui::SameLine();
        ImGui::TextDisabled("(socket → bone + offset. No preview: adjust numbers, test in-game)");
        ImGui::BeginChild("##sockets", {0, 200}, true);

        // Bone names from the currently loaded preview model (slot 0).
        std::vector<std::string> bone_names;
        if (preview_ && preview_init_ok_)
            bone_names = preview_->GetModel().BoneNames();
        if (bone_names.empty())
            ImGui::TextDisabled("Bone list unavailable — ensure a mesh slot is loaded in the preview.");

        if (ImGui::BeginTable("##sock_tbl", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableSetupColumn("Socket",   ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Bone",     ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("Pos X/Y/Z");
            ImGui::TableSetupColumn("Rot X/Y/Z");
            ImGui::TableSetupColumn("Scale",    ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("##sksave", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("##skdel",  ImGuiTableColumnFlags_WidthFixed, 35);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < d.socket_bindings.size(); ++i) {
                ActorDefSocket& s = d.socket_bindings[i];
                ImGui::PushID((int)(i + 60000));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ui::SearchableComboString("##sksock", s.socket_name, socket_vocab_names_, "(none)");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ui::SearchableComboString("##skbone", s.bone_name, bone_names, "(none)");

                ImGui::TableNextColumn();
                {
                    float pos[3] = {s.offset_pos_x, s.offset_pos_y, s.offset_pos_z};
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat3("##skpos", pos, 0.01f, -100.f, 100.f, "%.2f")) {
                        s.offset_pos_x = pos[0]; s.offset_pos_y = pos[1]; s.offset_pos_z = pos[2];
                    }
                }

                ImGui::TableNextColumn();
                {
                    float rot[3] = {s.offset_rot_x, s.offset_rot_y, s.offset_rot_z};
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat3("##skrot", rot, 1.f, -180.f, 180.f, "%.0f")) {
                        s.offset_rot_x = rot[0]; s.offset_rot_y = rot[1]; s.offset_rot_z = rot[2];
                    }
                }

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::DragFloat("##skscl", &s.offset_scale, 0.01f, 0.01f, 10.f, "%.2f");

                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Save##sk")) {
                    SaveActorDefSocket(db, s);
                }

                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Button, {0.55f, 0.1f, 0.1f, 1.f});
                if (ImGui::SmallButton("Del##sk")) {
                    DeleteActorDefSocket(db, s.id);
                }
                ImGui::PopStyleColor();

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if (ImGui::Button("+ Add Socket")) {
            ActorDefSocket s;
            s.actor_def_id = d.id;
            s.socket_name  = socket_vocab_names_.empty() ? "" : socket_vocab_names_.front();
            SaveActorDefSocket(db, s);
        }

        ImGui::EndChild(); // end ##sockets
    } else {
        ImGui::TextDisabled("Select an actor def, or click \"New Actor Def\".");
    }

    ImGui::EndChild();  // end of ##ad_edit

    // Vertical splitter — drag to resize the props/preview split.
    // {0,0} in BeginChild takes the full remaining height, so -1.f works here.
    ImGui::SameLine();
    ImGui::InvisibleButton("##vsplit", {8.f, -1.f});
    if (ImGui::IsItemActive()) {
        ad_preview_w_ -= ImGui::GetIO().MouseDelta.x;
        ad_preview_w_  = std::clamp(ad_preview_w_, 280.f, ad_total_w - 200.f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemDeactivated()) SavePrefs_();

    // --- Right column: 3D preview of the composed actor ---
    ImGui::SameLine();
    ImGui::BeginChild("##ad_preview", {0, 0}, true);
    if (!preview_init_ok_) {
        ImGui::TextDisabled("Preview unavailable (shader load failed).");
    } else if (selActorDef_ >= 0 && selActorDef_ < (int)actor_defs_.size()) {
        // Resolve the Body slot mesh + material from the edit state.
        const ActorMeshSlot* body = nullptr;
        for (auto& s : editActorDef_.mesh_slots) {
            if (s.slot == 0) { body = &s; break; }
        }
        if (!body && !editActorDef_.mesh_slots.empty())
            body = &editActorDef_.mesh_slots.front();

        if (!body) {
            preview_->Clear();
            ImGui::TextDisabled("Add a Body slot to see the actor.");
        } else {
            const MediaModel* mdl = nullptr;
            for (auto& m : models_) if (m.id == body->model_id) { mdl = &m; break; }

            if (!mdl || mdl->file_path.empty()) {
                preview_->Clear();
                ImGui::TextDisabled("Body slot points to an invalid model.");
            } else {
                bool model_changed = preview_->CurrentPath() != mdl->file_path;
                if (model_changed) {
                    preview_->LoadModel(mdl->file_path);
                    preview_last_material_id_ = -1;    // force re-apply below
                    preview_last_model_id_    = -1;    // force re-apply mapping
                }

                // Apply the Model's persisted per-aiMaterial mapping first
                // so multi-part models (e.g. 44-submesh dwarf) show up with
                // correct textures. This only runs when the model or the
                // materials list changes — OverrideMaterial below is
                // deliberately gated the same way.
                // FIX1: include per-part map changes in the re-apply condition.
                const bool apply_map_needed =
                    preview_last_model_id_ != mdl->id || materialsDirtyForPreview_ ||
                    body->submesh_materials != preview_last_submesh_materials_;
                if (apply_map_needed) {
                    // 1. Per-actor-def submesh materials (body slot) — highest priority.
                    if (!body->submesh_materials.empty()) {
                        std::vector<PreviewViewport::SubmeshMaterialEntry> entries;
                        for (const auto& [ai_name, mat_id] : body->submesh_materials) {
                            if (mat_id == 0) continue;
                            const MediaMaterial* mm = nullptr;
                            for (const auto& m : materials_)
                                if (m.id == mat_id) { mm = &m; break; }
                            if (!mm) continue;
                            PreviewViewport::SubmeshMaterialEntry e;
                            e.ai_name    = ai_name;
                            e.albedo_rel = mm->albedo_path;
                            e.normal_rel = mm->normal_path;
                            e.orm_rel    = mm->orm_path;
                            e.albedo_r   = mm->albedo_r;
                            e.albedo_g   = mm->albedo_g;
                            e.albedo_b   = mm->albedo_b;
                            e.roughness    = mm->roughness;
                            e.metallic     = mm->metallic;
                            e.black_cutout = mm->black_cutout;
                            entries.push_back(std::move(e));
                        }
                        std::fprintf(stderr, "[preview-mat] PATH=per-part  entries=%zu\n",
                                     entries.size());
                        for (const auto& e : entries)
                            std::fprintf(stderr, "[preview-mat]   part='%s' albedo='%s'\n",
                                         e.ai_name.c_str(), e.albedo_rel.c_str());
                        preview_->OverrideMaterialsByName(entries);
                    } else if (!mdl->material_map.empty()) {
                        // 2. Fallback: model-level material_map (legacy).
                        std::unordered_map<std::string, const MediaMaterial*> byName;
                        for (const auto& mm : materials_) byName[mm.name] = &mm;

                        std::vector<PreviewViewport::MaterialLookup> lookups;
                        for (const auto& ai_name : preview_->MaterialNames()) {
                            auto mit = mdl->material_map.find(ai_name);
                            const MediaMaterial* mm = nullptr;
                            if (mit != mdl->material_map.end()) {
                                auto nit = byName.find(mit->second);
                                if (nit != byName.end()) mm = nit->second;
                            }
                            if (!mm) continue;
                            PreviewViewport::MaterialLookup l;
                            l.name       = ai_name;
                            l.albedo_rel = mm->albedo_path;
                            l.normal_rel = mm->normal_path;
                            l.orm_rel    = mm->orm_path;
                            lookups.push_back(std::move(l));
                        }
                        preview_->ApplyMaterialsFromMedia(lookups);
                    }
                    preview_last_submesh_materials_ = body->submesh_materials;
                    materialsDirtyForPreview_ = false;
                }
                preview_last_model_id_ = mdl->id;

                // FIX2: global material_id override only applies when there are no
                // per-part overrides — otherwise it would clobber the per-part work.
                if (body->submesh_materials.empty()) {
                    const MediaMaterial* mat = nullptr;
                    for (auto& mm : materials_)
                        if (mm.id == body->material_id) { mat = &mm; break; }

                    const bool material_changed = (body->material_id != preview_last_material_id_);
                    if (mat && material_changed) {
                        std::fprintf(stderr,
                            "[preview-mat] PATH=global  calling OverrideMaterial (id %d->%d)"
                            "  albedo='%s'\n",
                            preview_last_material_id_, mat->id, mat->albedo_path.c_str());
                        preview_->OverrideMaterial(mat->albedo_path, mat->normal_path,
                                                   mat->orm_path,
                                                   mat->albedo_r, mat->albedo_g, mat->albedo_b,
                                                   mat->roughness, mat->metallic,
                                                   mat->black_cutout);
                        preview_last_material_id_ = mat->id;
                    } else if (!mat && body->material_id != 0 && material_changed) {
                        std::fprintf(stderr,
                            "[preview-mat] PATH=global  material_id=%d NOT FOUND"
                            " (list size=%zu)\n",
                            body->material_id, materials_.size());
                    }
                } else {
                    // Per-part active: keep preview_last_material_id_ in sync so
                    // switching back to global mode doesn't use a stale baseline.
                    preview_last_material_id_ = body->material_id;
                }
                // Live scale preview — cheap to set every frame; lets the
                // user drag the Scale slider and see the result immediately.
                preview_->SetActorScale(editActorDef_.scale);
                preview_->SetCollisionShapes({});
                preview_->SetCollisionPreviewVisible(false);

                // Build action entries so the preview dropdown lists actions
                // (Idle, Walk…) not raw clip names. action_index carries the
                // position in anim_map; callbacks resolve it at click time via
                // editActorDef_ (stable member) — robust to any future realloc.
                {
                    std::vector<AnimActionEntry> anim_entries;
                    anim_entries.reserve(editActorDef_.anim_map.size());
                    for (int ai = 0; ai < (int)editActorDef_.anim_map.size(); ++ai) {
                        const auto& a = editActorDef_.anim_map[ai];
                        AnimActionEntry e;
                        e.action        = a.action;
                        e.source_path   = a.source_path;
                        e.clip_override = a.clip_override;
                        e.loop          = a.loop;
                        e.action_index  = ai;
                        e.start_frame   = a.start_frame;
                        e.end_frame     = a.end_frame;
                        anim_entries.push_back(std::move(e));
                    }
                    preview_->SetAnimActions(
                        std::move(anim_entries),
                        [this](int idx, int frame) {
                            if (idx >= 0 && idx < (int)editActorDef_.anim_map.size())
                                editActorDef_.anim_map[idx].start_frame = frame;
                        },
                        [this](int idx, int frame) {
                            if (idx >= 0 && idx < (int)editActorDef_.anim_map.size())
                                editActorDef_.anim_map[idx].end_frame = frame;
                        }
                    );
                }

                preview_->DrawImGui();
            }
        }
    } else {
        preview_->Clear();
        ImGui::TextDisabled("No actor def selected.");
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Main Draw
// ---------------------------------------------------------------------------

// Classify a file extension (without leading dot, lowercased) into the kind
// of asset the batch importer should treat it as. Returns nullptr for
// unknown extensions.
static const char* ClassifyAsset(const std::string& ext) {
    static const char* kModel[]   = {"glb", "fbx", "obj", "gltf", "dae", "b3d"};
    static const char* kTexture[] = {"png", "jpg", "jpeg", "tga", "bmp", "ktx", "ktx2"};
    for (const char* e : kModel)   if (ext == e) return "model";
    for (const char* e : kTexture) if (ext == e) return "texture";
    return nullptr;
}

static std::string LowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return e;
}

void MediaTab::ImportFilesBatch(sqlite3* db) {
    auto picks = gue::PickMultipleFiles("Models",
                                        "glb,fbx,obj,gltf,dae,b3d");
    if (picks.empty()) return;

    int nModels = 0, nSkipped = 0;

    for (const auto& abs : picks) {
        std::filesystem::path p(abs);
        std::string ext    = LowerExt(p);
        std::string base   = p.stem().string();
        std::string folder = SafeFolderName(base);
        if (folder.empty()) folder = "asset";

        const char* kind = ClassifyAsset(ext);
        if (!kind || std::strcmp(kind, "model") != 0) { ++nSkipped; continue; }

        std::string sub = "models/" + folder;
        std::string rel = gue::ImportAbsolutePath(abs, sub.c_str());
        if (rel.empty()) { ++nSkipped; continue; }
        MediaModel m;
        m.name      = base;
        m.file_path = rel;
        m.scale     = 1.f;
        SaveModel(db, m);
        ++nModels;
    }

    ++media_revision_;
    needFetch_ = true;
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Imported %d model(s)%s.",
                  nModels, nSkipped ? " (some skipped)" : "");
}

void MediaTab::ImportLooseTextures_(sqlite3* db) {
    namespace fs = std::filesystem;

    std::string srcFolder = gue::PickFolder("Select texture folder to import");
    if (srcFolder.empty()) return;

    std::error_code ec;
    fs::path src = fs::canonical(srcFolder, ec);
    if (ec) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Texture import failed: invalid path.");
        return;
    }

    std::string folderName = SafeFolderName(src.filename().string());
    if (folderName.empty()) folderName = "textures";

    fs::path cwd     = fs::current_path();
    fs::path dstRoot = cwd.parent_path() / "client" / "assets" / "textures" / folderName;
    const std::string relBase = "assets/textures/" + folderName + "/";

    int done = 0, skipped = 0;
    for (auto& entry : fs::recursive_directory_iterator(src, ec)) {
        if (ec || !entry.is_regular_file()) { ec.clear(); continue; }

        fs::path fp  = entry.path();
        std::string ext = LowerExt(fp);
        if (ext != "png" && ext != "jpg" && ext != "jpeg" &&
            ext != "tga" && ext != "bmp") continue;

        fs::path rel = fs::relative(fp, src, ec);
        if (ec) { ec.clear(); ++skipped; continue; }

        // guard against ".." escape (same as PBR import)
        std::string rs = rel.generic_string();
        if (rs.rfind("..", 0) == 0 || rs.find("/../") != std::string::npos) {
            ++skipped; continue;
        }

        fs::path dst = dstRoot / rel;
        fs::create_directories(dst.parent_path(), ec);
        if (ec) { ec.clear(); ++skipped; continue; }
        fs::copy_file(fp, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { ec.clear(); ++skipped; continue; }

        MediaMaterial m;
        m.name        = fp.stem().string();
        m.albedo_path = relBase + rs;
        SaveMaterial(db, m);
        ++done;
    }

    ++media_revision_;
    needFetch_ = true;
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Imported %d texture(s) as simple material%s.",
                  done, skipped ? " (some skipped)" : "");
}

void MediaTab::ImportFolderTree(sqlite3* db) {
    std::string srcFolder = gue::PickFolder("Select folder to import");
    if (srcFolder.empty()) return;

    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path src = fs::canonical(srcFolder, ec);
    if (ec) {
        std::snprintf(statusMsg_, sizeof(statusMsg_), "Folder import failed: invalid path.");
        return;
    }

    std::string folderName = src.filename().string();
    if (folderName.empty()) folderName = "imported";

    // Destination root: dist/tools/../client/assets/models/<folderName>/
    fs::path assetsModels = fs::current_path() / ".." / "client" / "assets" / "models" / folderName;

    int nModels = 0, nCopied = 0, nSkipped = 0;

    for (auto& entry : fs::recursive_directory_iterator(src, ec)) {
        if (ec || !entry.is_regular_file()) { ec.clear(); continue; }

        fs::path filePath = entry.path();
        std::string ext   = LowerExt(filePath);

        fs::path rel = fs::relative(filePath, src, ec);
        if (ec) { ec.clear(); ++nSkipped; continue; }

        fs::path dst = assetsModels / rel;
        fs::create_directories(dst.parent_path(), ec);
        ec.clear();
        fs::copy_file(filePath, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { ec.clear(); ++nSkipped; continue; }
        ++nCopied;

        const char* kind = ClassifyAsset(ext);
        if (kind && std::strcmp(kind, "model") == 0) {
            // DB path is relative to assets/: models/<folderName>/<rel>
            std::string dbPath = "assets/models/" + folderName + "/" + rel.generic_string();
            // Build a folder-qualified name that mirrors the imported folder
            // structure on disk: "<rootFolder>/<subdir>/<filename>".
            std::string relDir = rel.parent_path().generic_string();
            std::string stem   = filePath.stem().string();
            std::string inner  = relDir.empty() ? stem : (relDir + "/" + stem);
            MediaModel m;
            m.name      = folderName + "/" + inner;
            m.file_path = dbPath;
            m.scale     = 1.f;
            SaveModel(db, m);
            ++nModels;
        }
    }

    ++media_revision_;
    needFetch_ = true;
    std::snprintf(statusMsg_, sizeof(statusMsg_),
                  "Imported '%s': %d model(s) registered, %d file(s) copied, %d skipped.",
                  folderName.c_str(), nModels, nCopied, nSkipped);
}

// ---------------------------------------------------------------------------
// Folder bulk-delete — reference detection helpers (file-static)
// ---------------------------------------------------------------------------

// Returns human-readable descriptions of every place model (id, file_path) is used.
// Checks: media_actor_meshes, game_settings.default_drop_model_id, item_templates.model_path.
static std::vector<std::string> ModelUsages(sqlite3* db, int id,
                                             const std::string& file_path)
{
    std::vector<std::string> uses;
    sqlite3_stmt* stmt = nullptr;

    // a) Actor def mesh slots
    sqlite3_prepare_v2(db,
        "SELECT ad.name, am.slot"
        " FROM media_actor_meshes am"
        " JOIN media_actor_defs ad ON ad.id = am.actor_def_id"
        " WHERE am.model_id = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* n = (const char*)sqlite3_column_text(stmt, 0);
        int slot = sqlite3_column_int(stmt, 1);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Actor Def '%s' (slot %s)",
                      n ? n : "?", ActorSlotName(slot));
        uses.push_back(buf);
    }
    sqlite3_finalize(stmt); stmt = nullptr;

    // b) Global default drop model (stored as text in game_settings)
    sqlite3_prepare_v2(db,
        "SELECT value FROM game_settings WHERE key='default_drop_model_id'",
        -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = (const char*)sqlite3_column_text(stmt, 0);
        if (v && std::to_string(id) == v)
            uses.push_back("Default Drop Model (Settings)");
    }
    sqlite3_finalize(stmt); stmt = nullptr;

    // c) Items — linked by file_path string, NOT by model id
    if (!file_path.empty()) {
        sqlite3_prepare_v2(db,
            "SELECT name FROM item_templates WHERE model_path = ?",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* n = (const char*)sqlite3_column_text(stmt, 0);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Item '%s'", n ? n : "?");
            uses.push_back(buf);
        }
        sqlite3_finalize(stmt);
    }

    return uses;
}

// Returns human-readable descriptions of every place material (id) is used.
static std::vector<std::string> MaterialUsages(sqlite3* db, int id)
{
    std::vector<std::string> uses;
    sqlite3_stmt* stmt = nullptr;

    sqlite3_prepare_v2(db,
        "SELECT ad.name, am.slot"
        " FROM media_actor_meshes am"
        " JOIN media_actor_defs ad ON ad.id = am.actor_def_id"
        " WHERE am.material_id = ?",
        -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* n = (const char*)sqlite3_column_text(stmt, 0);
        int slot = sqlite3_column_int(stmt, 1);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Actor Def '%s' (slot %s)",
                      n ? n : "?", ActorSlotName(slot));
        uses.push_back(buf);
    }
    sqlite3_finalize(stmt);

    return uses;
}

// Removes a single file identified by its DB-relative path (e.g. "assets/models/...").
// Resolves via ResolveClientAsset and verifies the canonical path is under assets/ before
// removing. Returns true on success or when the file is already absent.
static bool SafeRemoveAsset(const std::string& db_rel_path)
{
    if (db_rel_path.empty()) return true;

    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path assets_root = fs::canonical(
        fs::current_path().parent_path() / "client" / "assets", ec);
    if (ec) {
        std::fprintf(stderr, "[delete-folder] cannot resolve assets_root: %s\n",
                     ec.message().c_str());
        return false;
    }

    std::string resolved = ResolveClientAsset(db_rel_path); // "../client/assets/..."
    fs::path target = fs::canonical(resolved, ec);
    if (ec) return true; // file already absent — treat as success

    // Containment guard: canonical target must be strictly under assets_root.
    std::string root_s   = assets_root.generic_string();
    std::string target_s = target.generic_string();
    if (target_s.size() <= root_s.size() ||
        target_s.rfind(root_s, 0) != 0 ||
        target_s[root_s.size()] != '/') {
        std::fprintf(stderr,
                     "[delete-folder] GUARD blocked '%s' — not under assets/\n",
                     target_s.c_str());
        return false;
    }

    fs::remove(target, ec);
    if (ec) {
        std::fprintf(stderr, "[delete-folder] remove failed '%s': %s\n",
                     target_s.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Folder bulk-delete — MediaTab methods
// ---------------------------------------------------------------------------

void MediaTab::OpenFolderDeleteModal_(sqlite3* db,
                                      const std::string& folder_path,
                                      bool is_model)
{
    folderDeleteLabel_   = folder_path;
    folderDeleteIsModel_ = is_model;
    folderDeleteItems_.clear();

    sqlite3_stmt* stmt = nullptr;

    if (is_model) {
        // Model names embed their hierarchy (e.g. "Actors/Animals/Rat").
        // folder_path is the folder prefix in name-space (e.g. "Actors/Animals").
        std::string pattern = folder_path + "/%";
        sqlite3_prepare_v2(db,
            "SELECT id, name, file_path FROM media_models WHERE name LIKE ?",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FolderDeleteEntry e;
            e.id        = sqlite3_column_int(stmt, 0);
            auto* n     = sqlite3_column_text(stmt, 1);
            auto* fp    = sqlite3_column_text(stmt, 2);
            e.name      = n  ? (const char*)n  : "";
            e.file_path = fp ? (const char*)fp : "";
            e.used_by   = ModelUsages(db, e.id, e.file_path);
            folderDeleteItems_.push_back(std::move(e));
        }
        sqlite3_finalize(stmt);
    } else {
        // Material folder_path is the tree-key prefix after stripping the
        // "assets/textures/" or "assets/materials/" prefix (e.g. "Mixing").
        // Search both prefixes since imports may land under either one.
        std::string pat_tex = "assets/textures/"  + folder_path + "/%";
        std::string pat_mat = "assets/materials/" + folder_path + "/%";
        sqlite3_prepare_v2(db,
            "SELECT id, name, albedo_path, normal_path, orm_path"
            " FROM media_materials"
            " WHERE albedo_path LIKE ? OR albedo_path LIKE ?",
            -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, pat_tex.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, pat_mat.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            FolderDeleteEntry e;
            e.id          = sqlite3_column_int(stmt, 0);
            auto* n       = sqlite3_column_text(stmt, 1);
            auto* ap      = sqlite3_column_text(stmt, 2);
            auto* np      = sqlite3_column_text(stmt, 3);
            auto* op      = sqlite3_column_text(stmt, 4);
            e.name        = n  ? (const char*)n  : "";
            e.albedo_path = ap ? (const char*)ap : "";
            e.normal_path = np ? (const char*)np : "";
            e.orm_path    = op ? (const char*)op : "";
            e.used_by     = MaterialUsages(db, e.id);
            folderDeleteItems_.push_back(std::move(e));
        }
        sqlite3_finalize(stmt);
    }

    showFolderDeleteDlg_ = true;
    // ImGui::OpenPopup is called from DrawFolderDeleteModal_ (outside BeginChild scope).
}

void MediaTab::DrawFolderDeleteModal_(sqlite3* db)
{
    // Open the popup exactly once when the flag is set (pattern: flag → OpenPopup → clear).
    // BeginPopupContextItem queues the flag while inside a BeginChild; we open the modal
    // here, in the parent-window scope, one frame later.
    if (showFolderDeleteDlg_) {
        ImGui::OpenPopup("Delete Folder##fdlg");
        showFolderDeleteDlg_ = false;
    }

    ImGui::SetNextWindowSize({540.f, 440.f}, ImGuiCond_Appearing);
    bool modal_open = true;
    if (!ImGui::BeginPopupModal("Delete Folder##fdlg", &modal_open,
                                ImGuiWindowFlags_NoResize))
        return;

    int deletable = 0, blocked = 0;
    for (const auto& e : folderDeleteItems_) {
        if (e.used_by.empty()) ++deletable;
        else                   ++blocked;
    }

    ImGui::TextColored({1.f, 0.7f, 0.2f, 1.f},
                       "Folder: %s", folderDeleteLabel_.c_str());
    ImGui::Text("%d asset(s) found.", (int)folderDeleteItems_.size());
    ImGui::Separator();

    // ── Deletable list ──────────────────────────────────────────────────────
    if (deletable > 0) {
        ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f},
                           "Will be deleted (%d, not in use):", deletable);
        ImGui::BeginChild("##fdlg_del", {0.f, 100.f}, true);
        for (const auto& e : folderDeleteItems_)
            if (e.used_by.empty())
                ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("(no deletable assets — all are in use)");
    }

    // ── Blocked list ────────────────────────────────────────────────────────
    if (blocked > 0) {
        ImGui::Spacing();
        ImGui::TextColored({1.f, 0.4f, 0.2f, 1.f},
                           "BLOCKED — in use, will NOT be deleted (%d):", blocked);
        ImGui::BeginChild("##fdlg_blk", {0.f, 130.f}, true);
        for (const auto& e : folderDeleteItems_) {
            if (e.used_by.empty()) continue;
            ImGui::TextUnformatted(e.name.c_str());
            for (const auto& u : e.used_by)
                ImGui::TextDisabled("  \xe2\x86\x92 %s", u.c_str()); // → arrow (UTF-8)
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // ── Confirm / Cancel ────────────────────────────────────────────────────
    ImGui::BeginDisabled(deletable == 0);
    char btn[64];
    std::snprintf(btn, sizeof(btn), "Delete %d unused", deletable);
    if (ImGui::Button(btn)) {
        for (const auto& e : folderDeleteItems_) {
            if (!e.used_by.empty()) continue; // blocked — never touched

            if (folderDeleteIsModel_) {
                // 1. Remove orphaned collision shapes
                sqlite3_stmt* s = nullptr;
                sqlite3_prepare_v2(db,
                    "DELETE FROM media_model_shapes WHERE model_id=?",
                    -1, &s, nullptr);
                sqlite3_bind_int(s, 1, e.id);
                sqlite3_step(s);
                sqlite3_finalize(s);
                // 2. Remove model row
                s = nullptr;
                sqlite3_prepare_v2(db,
                    "DELETE FROM media_models WHERE id=?",
                    -1, &s, nullptr);
                sqlite3_bind_int(s, 1, e.id);
                sqlite3_step(s);
                sqlite3_finalize(s);
                // 3. Remove physical file (with containment guard)
                SafeRemoveAsset(e.file_path);
            } else {
                // 1. Remove material row
                sqlite3_stmt* s = nullptr;
                sqlite3_prepare_v2(db,
                    "DELETE FROM media_materials WHERE id=?",
                    -1, &s, nullptr);
                sqlite3_bind_int(s, 1, e.id);
                sqlite3_step(s);
                sqlite3_finalize(s);
                // 2. Remove each texture file individually (guard on each)
                SafeRemoveAsset(e.albedo_path);
                SafeRemoveAsset(e.normal_path);
                SafeRemoveAsset(e.orm_path);
            }
        }
        std::snprintf(statusMsg_, sizeof(statusMsg_),
                      "Deleted %d asset(s) from '%s'. %d blocked (still in use).",
                      deletable, folderDeleteLabel_.c_str(), blocked);
        needFetch_                = true;
        materialsDirtyForPreview_ = true;
        ++media_revision_;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

void MediaTab::Draw(sqlite3* db) {
    if (needFetch_) {
        // Before refetch, remember which actor def is currently being edited
        // by id. After refetch, re-select it and resync editActorDef_ — otherwise
        // edits to child tables (mesh_slots, anim_map) wouldn't show up in the
        // UI until the user manually re-clicked the def in the list.
        int keepActorDefId = -1;
        if (selActorDef_ >= 0 && selActorDef_ < (int)actor_defs_.size())
            keepActorDefId = actor_defs_[selActorDef_].id;

        FetchAll(db);
        needFetch_ = false;

        if (keepActorDefId > 0) {
            selActorDef_ = -1;
            for (int i = 0; i < (int)actor_defs_.size(); ++i) {
                if (actor_defs_[i].id == keepActorDefId) {
                    selActorDef_  = i;
                    editActorDef_ = actor_defs_[i];
                    break;
                }
            }
        }
    }

    if (ImGui::BeginTabBar("##media_subtabs")) {
        if (ImGui::BeginTabItem("Models"))     { DrawModels    (db); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Materials"))  { DrawMaterials (db); ImGui::EndTabItem(); }
        // "Anim Clips" sub-tab is hidden: animations are now managed inline
        // inside each Actor Def (action + source file + clip name). The
        // media_anim_clips table is still used under the hood by SaveAnimMap.
        if (ImGui::BeginTabItem("Actor Defs")) { DrawActorDefs (db); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    // Modal lives outside the tab bar so OpenPopup + BeginPopupModal share the
    // same parent-window scope (BeginPopupContextItem inside BeginChild queues
    // the flag; we open and render the popup here, one frame later).
    DrawFolderDeleteModal_(db);
}

// ---------------------------------------------------------------------------
// UI preference persistence (gue_prefs.txt, sibling to the exe in dist/tools/)
// Format: two lines
//   preview_w <float>
//   anim_h    <float>
// Missing keys or out-of-range values fall back to defaults quietly.
// ---------------------------------------------------------------------------

void MediaTab::LoadPrefs_() {
    prefs_loaded_ = true;
    FILE* f = std::fopen("gue_prefs.txt", "r");
    if (!f) return;
    char key[32];
    float val;
    while (std::fscanf(f, "%31s %f", key, &val) == 2) {
        if (std::strcmp(key, "preview_w") == 0)
            ad_preview_w_ = std::clamp(val, 280.f, 4000.f);
        else if (std::strcmp(key, "anim_h") == 0)
            ad_anim_h_ = std::clamp(val, 120.f, 800.f);
    }
    std::fclose(f);
}

void MediaTab::SavePrefs_() const {
    FILE* f = std::fopen("gue_prefs.txt", "w");
    if (!f) return;
    std::fprintf(f, "preview_w %.2f\n", ad_preview_w_);
    std::fprintf(f, "anim_h    %.2f\n", ad_anim_h_);
    std::fclose(f);
}

} // namespace gue
