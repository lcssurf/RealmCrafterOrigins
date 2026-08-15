#pragma once

#include "../preview_viewport.h"

#include <memory>
#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct ItemAttribute {
    std::string key;
    double      value = 0.0;
};

struct ItemSocketOverride {
    int   id = 0;
    int   item_template_id = 0;
    int   actor_def_id = 0;
    float offset_pos_x = 0.f;
    float offset_pos_y = 0.f;
    float offset_pos_z = 0.f;
    float offset_rot_x = 0.f;
    float offset_rot_y = 0.f;
    float offset_rot_z = 0.f;
    float offset_scale = 1.f;
};

struct ItemTemplate {
    int         id            = 0;
    std::string name;
    int         item_type     = 3; // 0=weapon 1=armor 2=consumable 3=misc
    int         slot_type     = 255;
    int         weapon_damage = 0;
    int         armor_level   = 0;
    int         weapon_dimension = 0; // 0=melee 1=ranged 2=magic
    int         weapon_hands     = 1; // 1=one-hand 2=two-hand (no mechanical effect yet)
    float       weapon_range     = 0.0f; // explicit attack range; 0 = use dimension default
    std::string weapon_kit;        // kit_key reference, "" = none
    // weapon_anim_style: PHYSICAL grip/pose archetype (weapon_anim_styles.style_key
    // reference — "sword_1h", "staff", "bow"...), "" = none assigned.
    // INDEPENDENT of weapon_kit (skills granted) and weapon_dimension/
    // weapon_hands (damage/range mechanics) — many items with different
    // kits/stats can share one style. Drives the "{BaseAction}_{StyleKey}"
    // composite animation lookup (Animation Vocabulary tab), NOT weapon_kit.
    std::string weapon_anim_style;
    int         max_stack     = 1;
    int         item_value    = 0;
    bool        stackable     = false;
    std::string model_path;
    float       model_scale   = 1.f;
    std::string socket_name;
    // UI icon shown in the inventory slot (migrateV53). "" = legacy
    // behaviour, client draws item name as text instead. Independent of
    // model_path (the 3D model rendered in-hand/on-ground).
    std::string icon_path;
    // script_on_target (generic scripting Fase 2) — the specific Lua event
    // name fired when this item is used WITH a target instead of the
    // generic "item_use_on_target" fallback (tool-used-on-a-resource-node,
    // e.g. harvesting). Only meaningful for item_type==4 (Script Item).
    // See server/internal/scripting/registry.go ItemUseScriptOnTarget.
    std::string script_on_target;
    std::vector<ItemSocketOverride> overrides;
    std::vector<ItemAttribute> attributes;
};

class ItemsTab {
public:
    void Draw(sqlite3* db);

    // Borrow the shared deferred renderer from GUE main. Must be called
    // before the first Draw() that touches the item-on-socket preview.
    void SetRenderer(::rco::renderer::Engine* engine,
                     ::rco::renderer::Pipeline* pipeline) {
        engine_   = engine;
        pipeline_ = pipeline;
    }

private:
    struct WeaponKitOption {
        std::string kit_key;
        std::string display_name;
    };

    // Weapon Anim Style option (weapon_anim_styles table) — PHYSICAL
    // grip/pose archetype, separate catalog from WeaponKitOption above.
    struct WeaponAnimStyleOption {
        std::string style_key;
        std::string display_name;
    };

    // Model picker entry (from media_models) — lets the item's "Model Path"
    // field use the same searchable-by-name combo as the rest of the tool
    // instead of a raw path InputText.
    struct ModelOption {
        int         id = 0;
        std::string name;
        std::string file_path;
    };

    void Fetch(sqlite3* db);
    void FetchWeaponKitOptions(sqlite3* db);
    void FetchWeaponAnimStyleOptions(sqlite3* db);
    bool DrawFields(ItemTemplate& t);
    bool Save(sqlite3* db, ItemTemplate& t);
    bool Delete(sqlite3* db, int id);
    bool LoadItemAttributes(sqlite3* db, ItemTemplate& t);
    bool SaveItemAttributes(sqlite3* db, const ItemTemplate& t);
    bool LoadItemOverrides(sqlite3* db, ItemTemplate& t);
    bool SaveItemOverrides(sqlite3* db, const ItemTemplate& t);
    bool LoadSocketVocabulary(sqlite3* db);
    bool LoadActorDefs(sqlite3* db);
    bool LoadModelOptions(sqlite3* db);

    // Looks up media_models.scale (the model's own import-scale, e.g. 0.02
    // for an asset authored in different units) by file_path — the SAME
    // factor ResolveActorDefSocket already fetches for the BODY model, but
    // items.model_scale (the item template's own field, entered by hand in
    // DrawFields) was never wired to it, so editing a weapon's import-scale
    // in Assets/Model never reached the item socket preview. Returns 1.f if
    // model_path is empty or not found (matches every other <=0 guard in
    // this file — never lets a missing/invalid factor collapse a
    // composition to zero).
    float ResolveModelImportScale(sqlite3* db, const std::string& model_path);

    // Resolves the actor def's Body model path and the bone bound to
    // `socket_name` (plus that binding's own offset), used to compose the
    // item-on-socket preview. Returns false if the actor def, its Body
    // slot, or the socket binding can't be resolved.
    struct SocketResolution {
        std::string body_model_path;
        // Body model's own import-scale (media_models.scale) and the actor
        // def's own size-override multiplier (media_actor_defs.scale, same
        // field as the Actor Def editor's "Scale" slider — 0.5 filhote, 2.0
        // pai grandão). Both must be composed into the character's render
        // scale in DrawItemPreview the same way Actor Def preview already
        // does (mdl->scale * editActorDef_.scale) — this preview draws a
        // SEPARATE character instance, so it needs its own copy of the same
        // composition, not a value inherited from the Actor Def tab's state.
        float body_model_scale = 1.f;
        float actor_def_scale  = 1.f;
        std::string bone_name;
        float offset_pos_x = 0.f, offset_pos_y = 0.f, offset_pos_z = 0.f;
        float offset_rot_x = 0.f, offset_rot_y = 0.f, offset_rot_z = 0.f;
        float offset_scale = 1.f;
    };
    bool ResolveActorDefSocket(sqlite3* db, int actor_def_id,
                               const std::string& socket_name,
                               SocketResolution& out);

    void DrawItemPreview(sqlite3* db, ItemTemplate& t);

    std::vector<ItemTemplate> items_;
    std::vector<WeaponKitOption> weapon_kit_options_;
    std::vector<WeaponAnimStyleOption> weapon_anim_style_options_;
    int          selected_  = -1;
    bool         dirty_     = false;
    bool         needFetch_ = true;
    char         statusMsg_[256] = {};
    ItemTemplate editing_;
    ItemTemplate newItem_;
    bool         showNew_   = false;
    std::vector<std::string> socketVocab_;
    std::vector<std::pair<int, std::string>> actorDefOptions_;
    std::vector<ModelOption> modelOptions_;
    // Icon picker options (migrateV53) — every image found anywhere under
    // dist/client/assets/, as "assets/..." relative paths (gue::ListTextureAssets).
    // Refreshed on Fetch() and after importing a new icon.
    std::vector<std::string> iconOptions_;

    // Icon thumbnail preview — same safe lifecycle as MediaTab's
    // preview_mat_tex_/preview_mat_tex_id_ (media.h/media.cpp): at most one
    // texture alive at a time, freed before loading the next. Keyed by PATH
    // (not id) since icons are chosen by file path, not a media_materials-
    // style id.
    GLuint      preview_icon_tex_  = 0;   // 0 = none loaded
    std::string preview_icon_path_;       // path whose texture is in preview_icon_tex_

    // Which override row (index into t.overrides) is currently shown in the
    // preview panel. -1 = no preview.
    int previewOverrideIdx_ = -1;

    ::rco::renderer::Engine*         engine_         = nullptr;
    ::rco::renderer::Pipeline*       pipeline_       = nullptr;
    std::unique_ptr<PreviewViewport> preview_;
    bool                             preview_init_ok_ = false;
    // Item-socket attachment gizmo World/Local toggle state — persists
    // across frames/redraws (see DrawItemPreview's "Gizmo: World/Local" button).
    bool                             gizmoWorldSpace_ = false;
};

} // namespace gue
