package net

import (
	"context"
	"log"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/world"
)

// BuildAppearance resolves an actor_def_id into a fully concrete
// world.Appearance by looking up each mesh slot's model + material and each
// anim mapping's clip. Returns nil if the def has no meshes or the ID is 0.
func BuildAppearance(ctx context.Context, database *db.DB, defID int) *world.Appearance {
	if defID <= 0 {
		return nil
	}
	def, err := database.LoadActorDef(ctx, defID)
	if err != nil || def == nil {
		return nil
	}
	return BuildAppearanceFromDef(ctx, database, def)
}

// BuildAppearanceFromDef resolves appearance data from an already-loaded actor def.
func BuildAppearanceFromDef(ctx context.Context, database *db.DB, def *db.ActorDef) *world.Appearance {
	if def == nil || def.ID <= 0 {
		return nil
	}
	out := &world.Appearance{}

	actorScale := def.Scale
	if actorScale <= 0 {
		actorScale = 1.0
	}

	matsByName, _ := database.ListMediaMaterials(ctx)

	for _, m := range def.Meshes {
		model, _ := database.GetMediaModel(ctx, m.ModelID)
		if model == nil || model.FilePath == "" {
			continue
		}
		slot := world.MeshSlot{
			Slot:      uint8(m.Slot),
			ModelPath: model.FilePath,
			Scale:     model.Scale * actorScale,
		}
		if mat, _ := database.GetMediaMaterial(ctx, m.MaterialID); mat != nil {
			slot.AlbedoPath = mat.AlbedoPath
			slot.NormalPath = mat.NormalPath
			slot.ORMPath    = mat.ORMPath
			slot.AlbedoR    = mat.AlbedoR
			slot.AlbedoG    = mat.AlbedoG
			slot.AlbedoB    = mat.AlbedoB
			slot.Roughness  = mat.Roughness
			slot.Metallic   = mat.Metallic
		}
		// Per-actor-def submesh materials take precedence over the model's global
		// material_map. Fallback to model.MaterialMap when no rows exist so that
		// existing actor defs (with no per-submesh assignments) are unaffected.
		perActor, _ := database.LoadActorDefSubmeshMaterials(ctx, m.ID)
		if len(perActor) > 0 {
			for aiName, matID := range perActor {
				mat, err := database.GetMediaMaterial(ctx, matID)
				if err != nil || mat == nil {
					continue
				}
				slot.MaterialMap = append(slot.MaterialMap, world.AiMaterial{
					AiName:     aiName,
					AlbedoPath: mat.AlbedoPath,
					NormalPath: mat.NormalPath,
					ORMPath:    mat.ORMPath,
					AlbedoR:    mat.AlbedoR,
					AlbedoG:    mat.AlbedoG,
					AlbedoB:    mat.AlbedoB,
					Roughness:  mat.Roughness,
					Metallic:   mat.Metallic,
				})
			}
		} else {
			// Fallback: model-level material_map (shared across all actor defs
			// using this model). Used by legacy actor defs with no per-part override.
			for aiName, matName := range model.MaterialMap {
				mm := matsByName[matName]
				if mm == nil {
					continue
				}
				slot.MaterialMap = append(slot.MaterialMap, world.AiMaterial{
					AiName:     aiName,
					AlbedoPath: mm.AlbedoPath,
					NormalPath: mm.NormalPath,
					ORMPath:    mm.ORMPath,
					AlbedoR:    mm.AlbedoR,
					AlbedoG:    mm.AlbedoG,
					AlbedoB:    mm.AlbedoB,
					Roughness:  mm.Roughness,
					Metallic:   mm.Metallic,
				})
			}
		}
		out.Meshes = append(out.Meshes, slot)
	}
	if len(out.Meshes) == 0 {
		return nil
	}
	out.YawOffset = float32(def.YawOffset)
	out.YOffset   = float32(def.YOffset)

	// Socket bindings (B3a)
	if sockRows, err := database.LoadActorDefSockets(ctx, def.ID); err == nil {
		for _, s := range sockRows {
			out.Sockets = append(out.Sockets, world.SocketBinding{
				SocketName:  s.SocketName,
				BoneName:    s.BoneName,
				OffsetPos:   [3]float32{float32(s.OffsetPosX), float32(s.OffsetPosY), float32(s.OffsetPosZ)},
				OffsetRot:   [3]float32{float32(s.OffsetRotX), float32(s.OffsetRotY), float32(s.OffsetRotZ)},
				OffsetScale: float32(s.OffsetScale),
			})
		}
	}

	log.Printf("[appearance-anims] def_id=%d total_anim_entries=%d", def.ID, len(def.Anims))
	for _, a := range def.Anims {
		if a.ClipID == 0 {
			log.Printf("[appearance-anims]   SKIP action=%q clip_id=0 (not linked to a clip — set clip_id in GUE)", a.Action)
			continue
		}
		clip, _ := database.GetMediaAnimClip(ctx, a.ClipID)
		if clip == nil {
			log.Printf("[appearance-anims]   SKIP action=%q clip_id=%d: GetMediaAnimClip returned nil (clip row missing?)", a.Action, a.ClipID)
			continue
		}
		log.Printf("[appearance-anims]   clip action=%q clip_id=%d source_path=%q clip_override=%q start=%d end=%d fps=%.1f",
			a.Action, a.ClipID, clip.SourcePath, clip.ClipOverride, clip.StartFrame, clip.EndFrame, clip.FPS)
		// Skip bindings that have neither a source file nor an embedded clip alias —
		// the client has nothing to load or alias, so the binding is truly empty.
		if clip.SourcePath == "" && clip.ClipOverride == "" {
			log.Printf("[appearance-anims]   SKIP action=%q clip_id=%d: source_path and clip_override both empty (fill 'Source file' in GUE anim table, or set Clip name for embedded clips)",
				a.Action, a.ClipID)
			continue
		}
		events, _ := database.LoadAnimEvents(ctx, a.ClipID)
		var animEvents []world.AnimEvent
		for _, e := range events {
			animEvents = append(animEvents, world.AnimEvent{
				Frame:     e.Frame,
				EventType: e.EventType,
				Payload:   e.Payload,
			})
		}
		out.Anims = append(out.Anims, world.AnimBinding{
			Action:       a.Action,
			SourcePath:   clip.SourcePath,
			ClipOverride: clip.ClipOverride,
			StartFrame:   clip.StartFrame,
			EndFrame:     clip.EndFrame,
			FPS:          clip.FPS,
			Loop:         a.Loop,
			Speed:        a.Speed,
			BlendIn:      a.BlendIn,
			ReturnTo:     a.ReturnTo,
			Priority:     a.Priority,
			Events:       animEvents,
		})
	}
	log.Printf("[appearance-anims]   RESULT: %d binding(s) built (out of %d configured)", len(out.Anims), len(def.Anims))
	return out
}
