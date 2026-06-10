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
		out.Meshes = append(out.Meshes, slot)
	}
	if len(out.Meshes) == 0 {
		return nil
	}
	out.YawOffset = float32(def.YawOffset)
	out.YOffset   = float32(def.YOffset)

	for _, a := range def.Anims {
		clip, _ := database.GetMediaAnimClip(ctx, a.ClipID)
		if clip == nil {
			continue
		}
		// Skip bindings that have neither a source file nor an embedded clip alias —
		// the client has nothing to load or alias, so the binding is truly empty.
		if clip.SourcePath == "" && clip.ClipOverride == "" {
			log.Printf("[appearance] skipping binding action=%q clip_id=%d: no source_path and no clip_override",
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
	return out
}
