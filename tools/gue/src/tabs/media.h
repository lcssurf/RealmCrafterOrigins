#pragma once

#include "../preview_viewport.h"
#include "../texture_importer.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <sqlite3.h>

namespace gue {

// ---------------------------------------------------------------------------
// Raw asset entries
// ---------------------------------------------------------------------------

// Collision primitive attached to a model. Type 0 = box (size_x/y/z = W/H/D),
// type 1 = sphere (size_x = radius, size_y/z ignored).
struct ModelShape {
    int   id       = 0;
    int   model_id = 0;
    int   type     = 0;
    float offset_x = 0.f, offset_y = 0.f, offset_z = 0.f;
    float size_x   = 1.f, size_y   = 1.f, size_z   = 1.f;
};

struct MediaModel {
    int         id = 0;
    std::string name;
    std::string file_path;  // relative to project root, e.g. "client/assets/models/human.glb"
    float       scale = 1.f;
    // aiMaterial name (from the mesh file) → media_materials.name.
    // Persisted as "k1=v1;k2=v2" in the DB column `material_map`.
    std::unordered_map<std::string, std::string> material_map;
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
    bool        is_playable    = false;
    bool        is_mountable   = false;
    bool        is_interactive = false;

    std::vector<ActorMeshSlot> mesh_slots;
    std::vector<ActorAnimMap>  anim_map;
};

// ---------------------------------------------------------------------------
// Media tab
// ---------------------------------------------------------------------------

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

    // Multi-file import: opens the native picker, classifies each file
    // by extension (model vs texture), copies into the right assets/
    // subfolder, and inserts a row in the matching DB table. Counts are
    // reported in statusMsg_.
    void ImportFilesBatch(sqlite3* db);

    // Folder import: picks a folder via native dialog, copies the entire
    // subtree into assets/models/<folder_name>/ preserving internal structure,
    // and registers every mesh file (glb/fbx/b3d/…) as a MediaModel row.
    // Textures are NOT registered separately — they stay alongside their
    // meshes so Assimp resolves them by directory_ + "/" + texname.
    void ImportFolderTree(sqlite3* db);

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
    void LoadShapesForModel  (sqlite3* db, int model_id);
    void SaveModelShape      (sqlite3* db, ModelShape& s);
    void DeleteModelShape    (sqlite3* db, int id);

    std::vector<MediaModel>    models_;
    std::vector<MediaMaterial> materials_;
    std::vector<MediaAnimClip> clips_;
    std::vector<ActorDef>      actor_defs_;

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

    // Collision shapes for the selected model.
    std::vector<ModelShape> model_shapes_;
    int        sel_shape_            = -1;
    int        shapes_model_id_      = -1; // model_id whose shapes are loaded
    ModelShape edit_shape_;
    bool       dirty_shape_          = false;
    ModelShape pending_shape_;
    bool       new_shape_            = false;

    // --- Materials sub-tab state ---
    int           selMat_  = -1;
    MediaMaterial editMat_;
    bool          dirtyMat_ = false;
    bool          newMat_   = false;
    MediaMaterial pendingMat_;
    char          filterMat_[128] = {};

    // --- Bulk texture folder import (Materials tab) ---
    bool                        showImportDlg_ = false;
    std::vector<TextureGroup>   importGroups_;
    TextureImportOptions        importOpts_;
    std::string                 importSourceFolder_;
    char                        importSubdir_[128] = {};

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
    // Track the last (model, material) pair applied to preview_ so we don't
    // re-upload textures from disk every frame.
    int      preview_last_model_id_    = -1;
    int      preview_last_material_id_ = -1;
    // Set whenever materials_ changes — forces the Models preview to
    // re-resolve name-based material bindings next frame.
    bool     materialsDirtyForPreview_ = true;

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
