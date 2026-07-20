#pragma once

#include "../preview_viewport.h"
#include "../texture_importer.h"

#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <memory>
#include <sqlite3.h>

namespace gue {

// ---------------------------------------------------------------------------
// Raw asset entries
// ---------------------------------------------------------------------------

// Collision primitive attached to a model. Type 0 = box (size_x/y/z = W/H/D),
// type 1 = sphere (size_x = radius, size_y/z ignored), type 2 = mesh
// (full geometry), type 3 = wedge/ramp (size_x/y/z = W/H/D).
struct ModelShape {
    int   id       = 0;
    int   model_id = 0;
    int   type     = 0;
    float offset_x = 0.f, offset_y = 0.f, offset_z = 0.f;
    float size_x   = 1.f, size_y   = 1.f, size_z   = 1.f;
    // Generic extra params:
    // - Mesh (type 2): detail_a = face budget percent [0.1..100]
    // - Wedge (type 3): detail_a = subdivisions [1..16]
    float detail_a = 0.f;
    float detail_b = 0.f;
};

// ---------------------------------------------------------------------------
// Bone retargeting — canonical slots (Unreal/Unity "Humanoid Rig" style)
// ---------------------------------------------------------------------------

// Fixed vocabulary of ~19 humanoid retargeting slots. A compile-time
// constant, not a DB table — mirrored in server/internal/db/db.go's
// CanonicalBoneSlots (same "small fixed vocabulary duplicated per language"
// pattern as kSlotTypes/ActorSlot). Used to populate both the per-model
// "Bone Slots" editor and the global "Bone Conventions" editor with
// identical rows, so a slot name always means the same thing everywhere.
inline const char* const kCanonicalBoneSlots[] = {
    "Hips", "Spine", "Chest", "Neck", "Head",
    "Clavicle_L", "Clavicle_R",
    "UpperArm_L", "UpperArm_R",
    "Forearm_L", "Forearm_R",
    "Hand_L", "Hand_R",
    "Thigh_L", "Thigh_R",
    "Calf_L", "Calf_R",
    "Foot_L", "Foot_R",
};
constexpr int kCanonicalBoneSlotCount =
    (int)(sizeof(kCanonicalBoneSlots) / sizeof(kCanonicalBoneSlots[0]));

// Mirrors one row in model_bone_slots — PER MODEL: which of this model's own
// bones plays a given canonical slot. Empty internal_bone_name = slot not
// applicable to this model (e.g. a model with no separate Chest bone).
struct ModelBoneSlot {
    int         id       = 0;
    int         model_id = 0;
    std::string slot;               // one of kCanonicalBoneSlots
    std::string internal_bone_name; // "" = unset; else a name from BoneNames()
};

// Mirrors one row in external_bone_conventions — GLOBAL, not scoped to any
// model: how a naming convention (e.g. "Mixamo") labels a given canonical
// slot. Editable in the GUE so future conventions can be added as data.
struct ExternalBoneConvention {
    int         id = 0;
    std::string convention_name; // e.g. "Mixamo"
    std::string slot;            // one of kCanonicalBoneSlots
    std::string external_name;   // e.g. "mixamorig:LeftArm"
};

// Mirrors one row in model_retarget_offsets — a calibratable rotation offset
// (quaternion, x,y,z,w) that corrects for axis-convention differences between
// this model's rig and one external convention's rig, for one slot. Scoped by
// (model_id, slot, convention_name): unlike ModelBoneSlot, the offset
// genuinely depends on WHICH external convention is being reconciled (a
// different tool's rig could need a different correction for the exact same
// bone). Defaults to identity (0,0,0,1) — see BuildBoneRetargetOffsetMap in
// db.go and the retargeting formula in shared/renderer/src/model.cpp for the
// documented "identity offset == old behavior, unchanged" invariant.
struct ModelRetargetOffset {
    int         id = 0;
    int         model_id = 0;
    std::string slot;            // one of kCanonicalBoneSlots
    std::string convention_name; // e.g. "Mixamo"
    float       x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

struct MediaModel {
    int         id = 0;
    std::string name;
    std::string file_path;  // relative to project root, e.g. "client/assets/models/human.glb"
    float       scale = 1.f;
    // aiMaterial name (from the mesh file) → media_materials.name.
    // Persisted as "k1=v1;k2=v2" in the DB column `material_map`.
    std::unordered_map<std::string, std::string> material_map;

    // Per-model UV transform applied on top of the engine's automatic
    // KHR_texture_transform detection. Useful when the source DCC's exporter
    // omits the transform or applies a slightly off correction (Unreal does
    // both depending on version). Defaults preserve UVs unchanged.
    float uv_offset_x = 0.f;
    float uv_offset_y = 0.f;
    float uv_scale_x  = 1.f;
    float uv_scale_y  = 1.f;
    bool  black_cutout = false;  // discard near-black pixels (hair, foliage, fences)
};

// Serialize / parse the per-model material map column.
std::string SerializeMaterialMap(const std::unordered_map<std::string, std::string>& m);
std::unordered_map<std::string, std::string> ParseMaterialMap(const std::string& s);

struct MediaMaterial {
    int         id = 0;
    std::string name;
    std::string albedo_path;
    std::string normal_path;
    std::string orm_path;
    float       albedo_r       = 0.72f;
    float       albedo_g       = 0.68f;
    float       albedo_b       = 0.60f;
    float       roughness      = 0.5f;
    float       metallic       = 0.f;
    float       normal_strength = 2.5f;  // normal map intensity for terrain triplanar blend
    bool        black_cutout   = false;  // discard near-black pixels (hair, foliage, fences)
};

struct MediaAnimClip {
    int         id = 0;
    std::string name;            // friendly display name, e.g. "Walk"
    std::string source_path;     // "" = embedded in the actor's model; else separate FBX/GLB
    std::string clip_override;   // override clip name inside source file ("" = use file's own)
    int         start_frame = 0;
    int         end_frame   = -1;  // -1 = play to end of file
    float       fps         = 30.f;
};

// ---------------------------------------------------------------------------
// Actor definition (composition of meshes + anim map)
// ---------------------------------------------------------------------------

enum ActorSlot : int {
    SlotBody       = 0,
    SlotHair       = 1,
    SlotHelm       = 2,
    SlotChest      = 3,
    SlotHands      = 4,
    SlotBelt       = 5,
    SlotLegs       = 6,
    SlotFeet       = 7,
    SlotWeapon     = 8,
    SlotShield     = 9,
    SlotAttachment = 10,
};

struct ActorMeshSlot {
    int id           = 0;
    int actor_def_id = 0;
    int slot         = 0; // ActorSlot value
    int model_id     = 0;
    int material_id  = 0; // 0 = use model's embedded material

    // Per-aiMaterial-name material overrides for this slot.
    // key = material_name from the model file (e.g. "ID01", "Body_Mat")
    // value = media_materials.id; 0 = no override (use model default)
    std::unordered_map<std::string, int> submesh_materials;

    // Rigid bone attachment (only meaningful for slot != 0/Body). Fixed at
    // authoring time here in the GUE — NOT the item/equipment socket system
    // (ActorDefSocket above, consumed by the Items tab / B5 weapon-in-hand).
    // Empty bone_name = legacy behaviour: this slot is not loaded/drawn by
    // the preview or the client beyond slot 0, same as before this field
    // existed.
    std::string bone_name;
    float offset_pos_x = 0.f, offset_pos_y = 0.f, offset_pos_z = 0.f;
    float offset_rot_x = 0.f, offset_rot_y = 0.f, offset_rot_z = 0.f;
    float offset_scale = 1.f;
};

struct ActorAnimMap {
    int         id           = 0;
    int         actor_def_id = 0;
    std::string action;   // "Idle", "Walk", "Attack", "Death", ...
    int         clip_id   = 0;

    // Denormalized view of the backing media_anim_clips row — copied in on
    // fetch and written back on save so the UI can edit action + source +
    // clip inline without the user ever seeing the clips registry. Empty
    // source_path means "embedded in the actor's Body model".
    std::string source_path;
    std::string clip_override;
    int         start_frame = 0;   // 0 = start of clip (default, Mixamo behaviour unchanged)
    int         end_frame   = -1;  // -1 = play to end of clip

    // Playback metadata
    bool        loop      = true;
    float       speed     = 1.f;
    float       blend_in  = 0.15f;
    std::string return_to;       // "" = no automatic return; else action name to return to
    int         priority  = 0;   // higher wins on conflict
};

// ---------------------------------------------------------------------------
// Animation Events (frame markers per clip)
// ---------------------------------------------------------------------------

struct MediaAnimEvent {
    int         id         = 0;
    int         clip_id    = 0;
    int         frame      = 0;
    std::string event_type = "sfx";   // "sfx", "vfx", "hitbox", "footstep", or custom
    std::string payload;              // free JSON interpreted by the client handler
};

// Socket mapping for an actor def (Arco B / B3a).
// socket_name and bone_name are TEXT (not FK) — renaming in the vocab doesn't break rows.
struct ActorDefSocket {
    int         id           = 0;
    int         actor_def_id = 0;
    std::string socket_name;          // from socket_vocabulary
    std::string bone_name;            // literal bone name from the model
    float       offset_pos_x = 0.f;
    float       offset_pos_y = 0.f;
    float       offset_pos_z = 0.f;
    float       offset_rot_x = 0.f;  // euler degrees
    float       offset_rot_y = 0.f;
    float       offset_rot_z = 0.f;
    float       offset_scale = 1.f;  // uniform
};

struct ActorDef {
    int         id = 0;
    std::string name;

    // Multiplies each mesh slot's model scale at render time.
    // 1.0 = natural size, 0.5 = filhote, 2.0 = pai grandão.
    float       scale      = 1.f;

    // Extra Y rotation (degrees) applied before world yaw at render time.
    // Use to correct models that were exported facing the wrong direction
    // (e.g. 180 for a model facing backwards in its FBX).
    float       yaw_offset = 0.f;
    float       y_offset   = 0.f;  // vertical offset (world units) to lift/sink the model

    // Gameplay defaults — used when the user places this actor in a zone
    // (or spawns an NPC from it). Empty strings / zero values mean "no
    // default, ask the user to fill it in".
    std::string default_name;
    std::string default_race;
    std::string default_class;
    int         default_level          = 1;
    int         default_hp             = 100;
    int         default_ep             = 100;
    int         default_aggressiveness = 0;     // 0=passive 1=defensive 2=aggressive 3=dialog
    float       default_aggro_range   = 8.f;
    float       default_attack_range  = 2.f;
    int         default_respawn_ms    = 30000;
    int         loot_table_id        = 0;
    bool        is_playable    = false;
    bool        is_mountable   = false;
    bool        is_interactive = false;
    int         initial_spawn_id = 0;  // FK → player_spawns.id (0 = use default)

    std::vector<ActorMeshSlot>  mesh_slots;
    std::vector<ActorAnimMap>   anim_map;
    std::vector<ActorDefSocket> socket_bindings; // B3a
};

// ---------------------------------------------------------------------------
// Media tab
// ---------------------------------------------------------------------------

// Lightweight entry from player_spawns — used by the Initial Spawn combo on playable actor defs.
struct PlayerSpawnOption {
    int         id = 0;
    std::string area_name;
    std::string name;
};

class MediaTab {
public:
    void Draw(sqlite3* db);

    // Borrow the shared deferred renderer from GUE main. Must be called before
    // the first Draw() that touches the 3D preview (Models / Actor Defs sub-tabs).
    void SetRenderer(::rco::renderer::Engine* engine,
                     ::rco::renderer::Pipeline* pipeline) {
        engine_   = engine;
        pipeline_ = pipeline;
    }

    // Public so other tabs (Actors) can build pickers without their own query.
    void EnsureTables(sqlite3* db);
    void FetchAll(sqlite3* db);
    void ReloadPlayerSpawns(sqlite3* db);
    void LoadDropListOptions(sqlite3* db);

    const std::vector<MediaModel>&    Models()     const { return models_;     }
    const std::vector<MediaMaterial>& Materials()  const { return materials_;  }
    const std::vector<MediaAnimClip>& Clips()      const { return clips_;      }
    const std::vector<ActorDef>&      ActorDefs()  const { return actor_defs_; }

    // Monotonic revision — bumped whenever the Media data that other tabs
    // consume (materials, model material_map) changes. Consumers poll this
    // and refresh their caches when it moves. Centralising the change
    // signal here means every tab picks up edits without per-tab wiring.
    int MediaRevision() const { return media_revision_; }

private:
    // Sub-tabs
    void DrawModels     (sqlite3* db);
    void DrawMaterials  (sqlite3* db);
    void DrawAnimClips  (sqlite3* db);
    void DrawActorDefs  (sqlite3* db);

    // Multi-file import (models only): opens the native picker, copies each
    // model file into assets/models/<stem>/ and registers a MediaModel row.
    // Reports counts in statusMsg_.
    void ImportFilesBatch(sqlite3* db);

    // Folder import: picks a folder via native dialog, copies the entire
    // subtree into assets/models/<folder_name>/ preserving internal structure,
    // and registers every mesh file (glb/fbx/b3d/…) as a MediaModel row.
    // Textures are NOT registered separately — they stay alongside their
    // meshes so Assimp resolves them by directory_ + "/" + texname.
    void ImportFolderTree(sqlite3* db);

    // Loose texture import: picks one or more image files and copies each into
    // assets/textures/<stem>/, creating a minimal MediaMaterial (albedo only).
    void ImportLooseTextures_(sqlite3* db);

    // CRUD helpers
    void SaveModel      (sqlite3* db, MediaModel& m);
    void DeleteModel    (sqlite3* db, int id);
    void SaveMaterial   (sqlite3* db, MediaMaterial& m);
    void DeleteMaterial (sqlite3* db, int id);
    void SaveAnimClip   (sqlite3* db, MediaAnimClip& c);
    void DeleteAnimClip (sqlite3* db, int id);
    void SaveActorDef      (sqlite3* db, ActorDef& d);
    void DeleteActorDef    (sqlite3* db, int id);
    // Clone an actor def plus its mesh slots and anim mappings. The new
    // def's name is the source name + " (copy)". Selects the new def in
    // the list on success.
    void DuplicateActorDef (sqlite3* db, int sourceId);
    void SaveMeshSlot   (sqlite3* db, ActorMeshSlot& s);
    void DeleteMeshSlot (sqlite3* db, int id);
    void SaveAnimMap    (sqlite3* db, ActorAnimMap& a);
    void DeleteAnimMap  (sqlite3* db, int id);
    void SaveActorDefSocket  (sqlite3* db, ActorDefSocket& s);
    void DeleteActorDefSocket(sqlite3* db, int id);
    void LoadShapesForModel  (sqlite3* db, int model_id);
    void SaveModelShape      (sqlite3* db, ModelShape& s);
    void DeleteModelShape    (sqlite3* db, int id);

    // Bone retargeting — per-model slots + global conventions (see
    // media.h's kCanonicalBoneSlots / ModelBoneSlot / ExternalBoneConvention
    // doc comments for the two-table design).
    void LoadBoneSlotsForModel (sqlite3* db, int model_id);
    void SaveBoneSlot          (sqlite3* db, int model_id, const std::string& slot,
                                const std::string& internal_bone_name);
    void LoadBoneConventions   (sqlite3* db);
    void SaveBoneConventionRow (sqlite3* db, const std::string& convention_name,
                                const std::string& slot, const std::string& external_name);
    void DeleteBoneConvention  (sqlite3* db, const std::string& convention_name);
    // Joins external_bone_conventions x model_bone_slots for one (model_id,
    // convention_name) pair — the GUE-local mirror of db.go's
    // BuildBoneAliasMap, used to inject a live preview via
    // preview_->GetModel().SetBoneAliases(...) so "Load Mixamo Names" is
    // testable immediately without restarting anything.
    std::unordered_map<std::string, std::string> BuildBoneAliasMap(
        sqlite3* db, int model_id, const std::string& convention_name);
    void DrawBoneConventions(sqlite3* db);

    // Retarget Pose offsets — calibratable per-bone rotation correction,
    // scoped by (model_id, slot, convention_name). See ModelRetargetOffset's
    // doc comment (this header) and TECH_DEBT.md for the design rationale
    // (inspired by Unreal's IK Retargeter "Retarget Pose" bone offsets).
    void LoadRetargetOffsets(sqlite3* db, int model_id, const std::string& convention_name);
    void SaveRetargetOffset (sqlite3* db, int model_id, const std::string& slot,
                              const std::string& convention_name, float x, float y, float z, float w);
    // Estimates a starting offset for every mapped slot of (model_id,
    // convention_name) by comparing each bone's PARENT FRAME's full composed
    // bind-pose orientation (root-to-parent, rco::renderer::Model::
    // GetBindParentWorldRotation / ParentBindWorldRotationsUnoptimized)
    // between the model's own hierarchy and a reference pose file for that
    // convention (any exported file sharing that convention's rest pose/rig
    // proportions — e.g. any Mixamo FBX). Uses the full parent-frame
    // rotation rather than a single bone-direction vector so there's no
    // twist ambiguity (a direction vector alone only pins 2 of 3 rotational
    // degrees of freedom). overwrite=false only fills slots that don't
    // already have a non-identity offset saved; overwrite=true replaces
    // everything. Returns the number of slots estimated.
    int EstimateRetargetOffsets(sqlite3* db, int model_id, const std::string& convention_name,
                                 const std::string& reference_file_path, bool overwrite);
    std::vector<ModelRetargetOffset> model_retarget_offsets_;
    int         retarget_offsets_model_id_ = -1;
    std::string retarget_offsets_convention_;
    // Session-only (not persisted) — the dev pastes/browses any file that
    // shares the convention's rest pose here, used only when "Estimate
    // Offsets" is clicked. Keyed by convention_name so switching conventions
    // remembers each one's last-used reference file within this session.
    std::unordered_map<std::string, std::string> retarget_reference_file_by_convention_;

    // Animation vocabulary (Phase A.1/A.2) — flat list of action names for
    // the actor anim editor's strict combo. Loaded independently from
    // SettingsTab (same anim_vocabulary table).
    void LoadAnimVocabNames(sqlite3* db);
    bool VocabContains(const std::string& name) const;

    // Socket vocabulary (B2) — flat list for the socket combo in the socket editor.
    void LoadSocketVocabNames(sqlite3* db);

    std::vector<MediaModel>    models_;
    std::vector<MediaMaterial> materials_;
    std::vector<MediaAnimClip> clips_;
    std::vector<ActorDef>      actor_defs_;
    std::vector<std::pair<int, std::string>> drop_list_options_;
    std::vector<PlayerSpawnOption> player_spawns_;
    std::vector<std::string>   anim_vocab_names_;
    std::vector<std::string>   socket_vocab_names_; // B3a

    bool needFetch_        = true;
    char statusMsg_[256]   = {};

    // Lazy-initialized preview (needs a valid GL context → built on first use).
    std::unique_ptr<PreviewViewport> preview_;
    bool preview_init_ok_ = false;

    // Borrowed from GUE main via SetRenderer().
    ::rco::renderer::Engine*   engine_   = nullptr;
    ::rco::renderer::Pipeline* pipeline_ = nullptr;

    // --- Models sub-tab state ---
    int        selModel_   = -1;
    MediaModel editModel_;
    bool       dirtyModel_ = false;
    bool       newModel_   = false;
    MediaModel pendingModel_;
    char       filterModel_[128] = {};

    // Multi-select bulk edit — Ctrl+click a row in the model list to toggle
    // it in/out of modelMultiSel_ (holds MediaModel::id, not vector indices,
    // so it survives a Refresh/refetch without desyncing). Shift+click
    // selects the contiguous range from modelMultiSelAnchor_ to the clicked
    // row (Shift+Ctrl+click adds that range to the existing selection
    // instead of replacing it). When 2+ are selected, DrawModels shows a
    // "Bulk Edit" panel to apply the same Scale and/or Black Cutout to every
    // selected model in one go.
    std::vector<int> modelMultiSel_;
    int   modelMultiSelAnchor_  = -1;  // index (in models_) of the last plain/Ctrl click
    bool  bulkApplyScale_       = false;
    float bulkScaleValue_       = 1.f;
    bool  bulkApplyBlackCutout_ = false;
    bool  bulkBlackCutoutValue_ = false;

    // Collision shapes for the selected model.
    std::vector<ModelShape> model_shapes_;
    bool       show_collision_preview_ = true;
    bool       show_socket_preview_    = true;  // Actor Defs socket gizmo overlay
    int        sel_shape_            = -1;
    int        shapes_model_id_      = -1; // model_id whose shapes are loaded
    ModelShape edit_shape_;
    bool       dirty_shape_          = false;
    ModelShape pending_shape_;
    bool       new_shape_            = false;

    // Bone Slots (per model) — always exactly kCanonicalBoneSlotCount rows
    // (loaded/defaulted from model_bone_slots for the selected model; unset
    // slots just have an empty internal_bone_name, no add/remove UI needed).
    std::vector<ModelBoneSlot> model_bone_slots_;
    int bone_slots_model_id_ = -1; // model_id whose slots are loaded into model_bone_slots_

    // Caches Model::AllNodeNamesUnoptimized's result for the Bone Slots combo
    // so the throwaway Assimp re-parse only happens when the selected model
    // changes, not every ImGui frame.
    std::vector<std::string> bone_slot_node_names_cache_;
    int bone_slot_node_names_model_id_ = -1;

    // "Preview with convention" combo selection (Bone Slots section, next to
    // "Apply for Preview"). Previously a function-local `static std::string`
    // inside DrawModels — that's process-lifetime storage in C++ so it isn't
    // destroyed by switching tabs, but it also wasn't scoped to any model, so
    // it could show a stale convention name left over from a PREVIOUSLY
    // selected model. Now a real member, reset only when the selected model
    // (editModel_.id) changes — not on every redraw or Bone Conventions reload.
    std::string preview_bone_convention_;
    int         preview_bone_convention_model_id_ = -1;

    // Bone Conventions (global, not per-model) — every convention's full set
    // of (slot, external_name) rows, loaded once and refreshed on save.
    std::vector<ExternalBoneConvention> external_bone_conventions_;
    std::string selected_bone_convention_; // convention_name currently shown, "" = none yet
    char new_bone_convention_name_[64] = {};

    // --- Materials sub-tab state ---
    int           selMat_  = -1;
    MediaMaterial editMat_;
    bool          dirtyMat_ = false;
    bool          newMat_   = false;
    MediaMaterial pendingMat_;
    char          filterMat_[128] = {};
    GLuint        preview_mat_tex_    = 0;   // 0 = none loaded
    int           preview_mat_tex_id_ = -1;  // material.id whose tex is in preview_mat_tex_

    // --- Bulk texture folder import (Materials tab) ---
    bool                        showImportDlg_       = false;
    bool                        showPbrScanFailDlg_  = false;
    std::vector<TextureGroup>   importGroups_;
    TextureImportOptions        importOpts_;
    std::string                 importSourceFolder_;
    char                        importSubdir_[128] = {};

    // --- Folder bulk-delete (Models + Materials) ---
    // One entry per asset found under the selected folder.
    struct FolderDeleteEntry {
        int         id;
        std::string name;
        std::string file_path;    // models: the file on disk (e.g. "assets/models/...")
        std::string albedo_path;  // materials: texture files
        std::string normal_path;
        std::string orm_path;
        // Non-empty = asset is referenced somewhere; list human-readable descriptions.
        // Empty = asset is safe to delete.
        std::vector<std::string> used_by;
    };
    bool                           showFolderDeleteDlg_ = false;
    std::string                    folderDeleteLabel_;    // folder path shown in modal title
    std::vector<FolderDeleteEntry> folderDeleteItems_;
    bool                           folderDeleteIsModel_  = true;

    void OpenFolderDeleteModal_(sqlite3* db,
                                const std::string& folder_path,
                                bool is_model);
    void DrawFolderDeleteModal_(sqlite3* db);

    // --- Anim Clips sub-tab state ---
    int           selClip_ = -1;
    MediaAnimClip editClip_;
    bool          dirtyClip_ = false;
    bool          newClip_   = false;
    MediaAnimClip pendingClip_;
    char          filterClip_[128] = {};

    // --- Actor Defs sub-tab state ---
    int      selActorDef_   = -1;
    char     filterActorDef_[128] = {};

    // Resizable layout — both are member state so they survive across frames
    // (a local would reset to the computed default every frame while dragging).
    // 0 means "not yet initialised"; set to the 40%-of-available fallback on
    // the first frame once we know the actual window width.
    float    ad_preview_w_  = 0.f;   // width of the right (preview) column
    float    ad_anim_h_     = 300.f; // height of the Animations child window

    // gue_prefs.txt — read once on first use, written when a splitter releases.
    bool     prefs_loaded_  = false;
    void     LoadPrefs_();
    void     SavePrefs_() const;
    // Track the last (model, material) pair applied to preview_ so we don't
    // re-upload textures from disk every frame.
    int      preview_last_model_id_    = -1;
    int      preview_last_material_id_ = -1;
    // Tracks the last submesh_materials map applied to the preview so that
    // editing any per-part choice in the UI triggers a re-apply.
    std::unordered_map<std::string, int> preview_last_submesh_materials_;
    // Set whenever materials_ changes — forces the Models preview to
    // re-resolve name-based material bindings next frame.
    bool     materialsDirtyForPreview_ = true;

    // Bumped by SaveBoneSlot (and available for a manual "Refresh
    // Retargeting" button) so the Actor Def preview can re-inject
    // bone_aliases_ even when the Body model itself hasn't changed. Without
    // this, editing Bone Slots while an Actor Def preview is already showing
    // that model would never re-fire SetBoneAliases: the injection below was
    // previously gated only on `model_changed` (preview_->CurrentPath() !=
    // mdl->file_path), which Bone Slot edits never touch. The stale alias map
    // then persists for the rest of the process — matching the reported bug
    // where changes only took effect after restarting the whole app.
    int      bone_slots_revision_               = 0;
    int      preview_last_bone_slots_revision_   = -1;

    // Revision counter. Bumped by SaveModel / SaveMaterial / DeleteMaterial
    // / the import flow. Read via MediaRevision() by other tabs.
    int      media_revision_ = 0;

    // Per-model user-supplied mapping from aiMaterial name → media_material
    // name. Lets the user bridge Maya/Substance naming mismatches (e.g.
    // "blinn1" → "ID01"). In-memory only — resets when the model reloads.
    std::unordered_map<std::string, std::string> previewAiToMedia_;
    std::string preview_material_names_model_;  // path this mapping belongs to
    ActorDef editActorDef_;
    bool     dirtyActorDef_ = false;
    bool     newActorDef_   = false;
    ActorDef pendingActorDef_;

    // New mesh slot / anim map input state (for the selected actor def)
    int  newSlotSlot_      = 0;
    int  newSlotModelIdx_  = -1;
    int  newSlotMatIdx_    = -1;
    char newAnimAction_[64] = {};
    int  newAnimClipIdx_    = -1;

    // --- Animation Events editor state (for the selected clip) ---
    std::vector<MediaAnimEvent> clip_events_;         // events for the currently selected clip
    int                         sel_event_            = -1;
    MediaAnimEvent              edit_event_;
    bool                        dirty_event_          = false;
    int                         events_loaded_for_clip_ = -1;  // clip id whose events are loaded

    // Event CRUD helpers
    void LoadEventsForClip(sqlite3* db, int clip_id);
    void SaveAnimEvent    (sqlite3* db, MediaAnimEvent& e);
    void DeleteAnimEvent  (sqlite3* db, int id);
};

const char* ActorSlotName(int slot);

} // namespace gue
