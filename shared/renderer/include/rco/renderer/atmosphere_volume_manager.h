#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace rco::renderer {

// One placed atmosphere volume ("Post Process Volume" style region) as
// received from the server via PAtmosphereVolumes — see
// server/internal/world/frame.go AtmosphereVolumesPayload and
// tools/gue/src/zone_scene.h ZAtmosphereVolume (the authoring side, Zone
// editor). Field set mirrors the same "authoritative render tuning" section
// AreaConfig sends via PAreaConfig (see AreaLightingProfile-adjacent fields
// in client/src/core/main.cpp), so a volume can override any subset of it.
//
// Fase 1 (this struct + AtmosphereVolumeManager): the client only STORES
// this list — nothing reads it yet. Fase 2 (not implemented here — see
// docs/TECH_DEBT.md "Atmosphere volumes"): every frame, test the player's
// position against each volume (AABB or sphere per `shape`), pick the
// highest-priority containing volume, and blend the area's own atmosphere
// toward/away from it over `transitionSeconds` — feeding the result into
// the same Pipeline::SetSceneLook/SetAtmosphereFog/SetSun calls the area's
// base config already drives every frame (main.cpp, right after
// pipeline->Begin()).
struct AtmosphereVolumeEntry {
    std::string name;
    int         shape = 0; // 0=AABB (pos±size/2), 1=sphere (radius=size.x/2)
    glm::vec3   pos{0.f};
    glm::vec3   size{10.f};
    int         priority = 0;
    float       transitionSeconds = 2.0f;

    glm::vec3 sunDir{0.18f, 0.96f, 0.20f};
    glm::vec3 sunColor{1.14f, 1.12f, 1.05f};
    float     sunIntensityMul = 1.00f;
    float     skyIntensityMul = 1.00f;
    float     fogDensityMul   = 0.92f;
    glm::vec3 fogColor{0.70f, 0.80f, 0.93f};
    bool      volumetrics = true;

    float charShadowLift   = 0.30f;
    float charRimStrength  = 0.18f;
    float charRimExponent  = 2.40f;
    float charMinNdotL     = 0.10f;
    float charAmbientBoost = 0.12f;

    float sceneIblIntensity       = 1.00f;
    float sceneSkyIntensity       = 1.16f;
    float sceneWorldShadowLift    = 0.10f;
    float sceneDirectScale        = 1.32f;
    float sceneAmbientScale       = 0.88f;
    float sceneFlatAmbient        = 0.03f;
    float sceneWorldMinNdotL      = 0.05f;
    float sceneAlbedoMinLuma      = 0.18f;
    float sceneAlbedoLiftStrength = 0.00f;
    float sceneSpecularScale      = 0.88f;
    float sceneExposureFactor     = 1.10f;
    float sceneSunIntensity       = 1.36f;

    float colorContrast         = 1.08f;
    float colorSaturation       = 1.08f;
    float colorVibrance         = 0.20f;
    float colorBlackPoint       = 0.010f;
    float colorVignetteStrength = 0.04f;
    float colorVignetteSoftness = 0.55f;
};

// Holds the current area's placed atmosphere volumes. Fase 1 — pure data
// storage, replaced wholesale on PAtmosphereVolumes (initial area entry +
// every portal/area change), same lifecycle as LightManager/WaterManager.
// No SubmitAll()/Render() yet — there's nothing to submit into the pipeline
// until Fase 2 implements the point-in-volume test and blend.
class AtmosphereVolumeManager {
public:
    void SetVolumes(std::vector<AtmosphereVolumeEntry> volumes) { volumes_ = std::move(volumes); }
    void Clear() { volumes_.clear(); }
    size_t Count() const { return volumes_.size(); }
    const std::vector<AtmosphereVolumeEntry>& Volumes() const { return volumes_; }

private:
    std::vector<AtmosphereVolumeEntry> volumes_;
};

} // namespace rco::renderer
