#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

// ---------------------------------------------------------------------------
// Raw asset entries
// ---------------------------------------------------------------------------

struct MediaModel {
    int         id = 0;
    std::string name;
    std::string file_path;  // relative to project root, e.g. "client/assets/models/human.glb"
    float       scale = 1.f;
};

struct MediaMaterial {
    int         id = 0;
    std::string name;
    std::string albedo_path;
    std::string normal_path;
    std::string orm_path;
    float       albedo_r  = 0.72f;
    float       albedo_g  = 0.68f;
    float       albedo_b  = 0.60f;
    float       roughness = 0.5f;
    float       metallic  = 0.f;
};

struct MediaAnimClip {
    int         id = 0;
    std::string name;            // friendly display name, e.g. "Walk"
    std::string source_path;     // "" = embedded in the actor's model; else separate FBX/GLB
    std::string clip_override;   // override clip name inside source file ("" = use file's own)
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
};

struct ActorDef {
    int         id = 0;
    std::string name;
    std::vector<ActorMeshSlot> mesh_slots;
    std::vector<ActorAnimMap>  anim_map;
};

// ---------------------------------------------------------------------------
// Media tab
// ---------------------------------------------------------------------------

class MediaTab {
public:
    void Draw(sqlite3* db);

    // Public so other tabs (Actors) can build pickers without their own query.
    void EnsureTables(sqlite3* db);
    void FetchAll(sqlite3* db);

    const std::vector<MediaModel>&    Models()     const { return models_;     }
    const std::vector<MediaMaterial>& Materials()  const { return materials_;  }
    const std::vector<MediaAnimClip>& Clips()      const { return clips_;      }
    const std::vector<ActorDef>&      ActorDefs()  const { return actor_defs_; }

private:
    // Sub-tabs
    void DrawModels     (sqlite3* db);
    void DrawMaterials  (sqlite3* db);
    void DrawAnimClips  (sqlite3* db);
    void DrawActorDefs  (sqlite3* db);

    // CRUD helpers
    void SaveModel      (sqlite3* db, MediaModel& m);
    void DeleteModel    (sqlite3* db, int id);
    void SaveMaterial   (sqlite3* db, MediaMaterial& m);
    void DeleteMaterial (sqlite3* db, int id);
    void SaveAnimClip   (sqlite3* db, MediaAnimClip& c);
    void DeleteAnimClip (sqlite3* db, int id);
    void SaveActorDef   (sqlite3* db, ActorDef& d);
    void DeleteActorDef (sqlite3* db, int id);
    void SaveMeshSlot   (sqlite3* db, ActorMeshSlot& s);
    void DeleteMeshSlot (sqlite3* db, int id);
    void SaveAnimMap    (sqlite3* db, ActorAnimMap& a);
    void DeleteAnimMap  (sqlite3* db, int id);

    std::vector<MediaModel>    models_;
    std::vector<MediaMaterial> materials_;
    std::vector<MediaAnimClip> clips_;
    std::vector<ActorDef>      actor_defs_;

    bool needFetch_        = true;
    char statusMsg_[256]   = {};

    // --- Models sub-tab state ---
    int        selModel_   = -1;
    MediaModel editModel_;
    bool       dirtyModel_ = false;
    bool       newModel_   = false;
    MediaModel pendingModel_;

    // --- Materials sub-tab state ---
    int           selMat_  = -1;
    MediaMaterial editMat_;
    bool          dirtyMat_ = false;
    bool          newMat_   = false;
    MediaMaterial pendingMat_;

    // --- Anim Clips sub-tab state ---
    int           selClip_ = -1;
    MediaAnimClip editClip_;
    bool          dirtyClip_ = false;
    bool          newClip_   = false;
    MediaAnimClip pendingClip_;

    // --- Actor Defs sub-tab state ---
    int      selActorDef_   = -1;
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
};

const char* ActorSlotName(int slot);

} // namespace gue
