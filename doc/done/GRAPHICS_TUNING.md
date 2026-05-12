# Graphics Tuning Reference

Every knob that controls the look of the renderer, with `file:line` so you can edit directly. All three viewports (Media preview, Zone editor, Client game) share the same `rco::renderer::Engine` + `Pipeline` — differences come from per-viewport settings at render time.

Rebuild after editing: client via `build-client.bat`, GUE via `build-gue.bat`.

---

## Viewport cheat sheet

| Setting | Client | Media preview | Zone editor |
|---|---|---|---|
| **FOV** | 60° | 55° | 60° |
| **Near / Far** | 0.5 / 2000 | 0.05 / 200 | 0.5 / 4000 |
| **Sun direction** | `-normalize(0.3, 1, 0.5)` | `(-0.4, -1, -0.3)` | `-normalize(0.5, 1, 0.3)` |
| **Sun color (RGB)** | `1.00, 0.95, 0.80` | `1.00, 0.95, 0.80` | `1.00, 0.96, 0.85` |
| **Volumetrics** | On | On | **Off** (cleaner editor) |
| **SSAO / FXAA** | On | On | On in Lit, Off in Simple |
| **Shadow res** | 1024² | 1024² | 1024² |
| **IBL HDRI** | `assets/ibl/default.hdr` | same | same |

If the same model looks different across viewports, it's almost always one of: **sun direction, FOV, or volumetrics off**.

---

## Per-viewport setup

- **Client** — `client/src/core/main.cpp:1159` (`SetSun`), camera defaults at `client/src/renderer/camera.h:31-33` (FOV, near, far).
- **Media preview** — `tools/gue/src/preview_viewport.cpp:121` (`SetSun`), `preview_viewport.cpp:118` (inline `glm::perspective` with FOV + near + far).
- **Zone editor** — `tools/gue/src/zone_renderer.cpp:458-459` (`SetSun`), camera in `tools/gue/src/zone_camera.h:19-21`.
- **FeatureConfig per viewport** — `tools/gue/src/zone_renderer.cpp:421-428` toggles volumetrics/SSAO/FXAA per mode.

---

## Shared pipeline knobs

All live under `shared/renderer/include/rco/renderer/pipeline.h`. Edit + rebuild the shared renderer lib (`build-client.bat` or `build-gue.bat` both rebuild it).

### FeatureConfig — on/off switches
`pipeline.h:14-20`
```cpp
bool ssao          = true;
bool volumetrics   = true;
bool ssr           = false;   // screen-space reflections — off by default (expensive)
bool fxaa          = true;
int  shadow_method = SHADOW_METHOD_ESM;  // ESM | VSM
```

### HDR / tonemap
`pipeline.h:194-201` — **auto-exposure via histogram**
```cpp
float targetLuminance = 0.22f;  // ↑ = darker scene (pushes exposure down)
float minExposure     = 0.1f;   // clamp bottom (avoid pitch-black on dim scenes)
float maxExposure     = 100.0f; // clamp top (avoid whiteout)
float exposureFactor  = 1.0f;   // manual multiplier on top of auto — bump for "more contrast"
float adjustmentSpeed = 2.0f;   // how fast eye adapts (per second)
```

### Shadows (ESM single-pass)
`pipeline.h:113-114` + `pipeline.cpp:65-68`
```cpp
sunConstantC_ = 80.0f;   // ESM softness — higher = softer, lower = sharper
vsmLightBleed = 0.9f;    // reduces VSM acne (0-1)
```
Ortho frustum: 120×120 world units, depth 1–350 (`pipeline.cpp:68`). Increase frustum size if shadows get clipped at zoom-out; depth range tighter = better precision.

### SSAO
`pipeline.h:164-176`
```cpp
int   samples_near    = 12;     // quality vs cost
float range           = 1.1f;   // occlusion radius (world units)
float s               = 1.8f;   // contrast
float k               = 1.0f;   // intensity — ↓ for subtle, ↑ for dirty corners
int   atrous_passes   = 3;      // bilateral denoise (1=fast noisy, 4=smooth)
```

### Volumetrics (god rays / fog)
`pipeline.h:136-162`
```cpp
int   steps         = 32;     // ray-march quality
float intensity     = 0.025f; // scattering strength — main "fog thickness" knob
float beerPower     = 1.0f;   // absorption falloff
float powderPower   = 1.0f;   // forward scattering (sun halo)
float heightOffset  = 0.0f;   // pushes fog up/down in world space
float hfIntensity   = 0.025f; // high-freq detail noise
```
Disabled in Zone editor and often in Media preview for clean inspection.

### FXAA
`pipeline.h:187-192`
```cpp
float contrastThreshold  = 0.0312f;  // edge detection — lower = more AA
float relativeThreshold  = 0.125f;
float pixelBlendStrength = 1.0f;
float edgeBlendStrength  = 1.0f;
```

### IBL
`pipeline.h:203`
```cpp
int numEnvSamples_ = 10;  // specular IBL samples — ↑ for crisp reflections, cost grows linearly
```
HDRI file loaded via `Engine::LoadIBL("assets/ibl/default.hdr")` — replace with any equirectangular `.hdr`.

---

## Common recipes

**Make scene brighter / more punchy**
- `HDRTuning.targetLuminance` ↓ (e.g., 0.18) — pushes exposure higher
- `HDRTuning.exposureFactor` ↑ (e.g., 1.3) — flat multiplier

**Softer shadows**
- `sunConstantC_` ↓ (e.g., 40) — ESM bleed increases softness

**Crisper shadows**
- `sunConstantC_` ↑ (e.g., 120) — sharper but more acne risk
- Shrink ortho frustum (`pipeline.cpp:68` first vec2) → more texels per world unit

**Less "foggy" client**
- `VolumetricTuning.intensity` ↓ (0.01 or less) — or just turn off `features_.volumetrics`

**Make sun direction uniform across viewports**
- Pick one vector; paste the same `SetSun(...)` call into all three files listed under "Per-viewport setup"

**Disable all post-processing to debug geometry/materials**
- In the relevant viewport, set every FeatureConfig flag to `false` and call `Pipeline::SetFeatures(cfg)` before `Begin()`

---

## After tuning — diff the viewports

Quick checklist to compare Media vs Zone vs Client on the same asset:

1. Load the actor def in **Media → Actor Defs** and note how it renders
2. Drop the same actor def as an NPC in **Zone editor**, switch to **Lit** mode
3. Spawn the same NPC in the **Client** (restart server to pick up new `npc_spawns` rows)
4. Compare. Likely culprits if they diverge:
   - **Colors/contrast differ** → sun color + direction differs across the three `SetSun` calls
   - **Outline/edges differ** → FXAA on/off, or `contrastThreshold` differs
   - **Material sheen differs** → volumetrics or SSAO on in one, off in another
   - **Shadow shape differs** → sun direction differs (shadows fall differently)
   - **Scale looks different** → FOV mismatch (Media 55° vs others 60°)

The cleanest path to "all three look identical" is: make all three viewports call the same `SetSun()`, identical FeatureConfig, and the same FOV. That's the principle; any intentional divergence (editor cleanliness) should be documented in the code comment next to the override.
