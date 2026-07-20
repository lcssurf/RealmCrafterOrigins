#include "rco/renderer/shader.h"
#include "rco/renderer/helpers.h"
#include <glad/glad.h>
#include <string>

namespace rco::renderer {

void CompileAllShaders() {
    Shader::shaders["gBuffer"].emplace(Shader({
        {"gBuffer.vs", GL_VERTEX_SHADER},
        {"gBuffer.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gBufferBindless"].emplace(Shader({
        {"gBufferBindless.vs", GL_VERTEX_SHADER},
        {"gBufferBindless.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gBufferSkinned"].emplace(Shader({
        {"gBufferBindless.vs", GL_VERTEX_SHADER,   "#define HAS_SKINNING\n"},
        {"gBufferBindless.fs", GL_FRAGMENT_SHADER, ""}
    }));
    Shader::shaders["gBufferSkinnedInstanced"].emplace(Shader({
        {"gBufferBindless.vs", GL_VERTEX_SHADER,   "#define HAS_SKINNING\n#define HAS_INSTANCED_SKINNING\n"},
        {"gBufferBindless.fs", GL_FRAGMENT_SHADER, ""}
    }));
    Shader::shaders["fstexture"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"texture.fs",        GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["fstexture_depth"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"texture_depth.fs",  GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gPhongGlobal"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"gPhongGlobal.fs",   GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gPhongManyLocal"].emplace(Shader({
        {"lightGeom.vs",       GL_VERTEX_SHADER},
        {"gPhongManyLocal.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["generate_histogram"].emplace(Shader({
        {"generate_histogram.cs", GL_COMPUTE_SHADER}
    }));
    Shader::shaders["calc_exposure"].emplace(Shader({
        {"calc_exposure.cs", GL_COMPUTE_SHADER}
    }));
    Shader::shaders["tonemap"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"tonemap.fs",        GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["fxaa"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"fxaa.fs",           GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["shadow"].emplace(Shader({
        {"shadow.vs", GL_VERTEX_SHADER}
    }));
    Shader::shaders["shadowBindless"].emplace(Shader({
        {"shadowBindless.vs", GL_VERTEX_SHADER}
    }));
    // Shadow variants for dynamic submissions (mirrors gBufferBindless variants).
    Shader::shaders["shadow_dynamic"].emplace(Shader({
        {"shadow_dynamic.vs", GL_VERTEX_SHADER}
    }));
    Shader::shaders["shadow_skinned_instanced"].emplace(Shader({
        {"shadow_skinned_instanced.vs", GL_VERTEX_SHADER}
    }));
    Shader::shaders["shadow_skinned"].emplace(Shader({
        {"shadow_skinned.vs", GL_VERTEX_SHADER}
    }));
    Shader::shaders["volumetric"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"volumetric.fs",     GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["vol_composite"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"vol_composite.fs",  GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["vsm_copy"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"vsm_copy.fs",       GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["esm_copy"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"esm_copy.fs",       GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["msm_copy"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"msm_copy.fs",       GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["atrous"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"atrous.fs",         GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["atrous_volumetric"].emplace(Shader({
        {"fullscreen_tri.vs",    GL_VERTEX_SHADER},
        {"atrous_volumetric.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["atrous_ssao"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"atrous_ssao.fs",    GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["ssr"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"ssr.fs",            GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["ssao"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"ssao.fs",           GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["hdri_skybox"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"hdri_skybox.fs",    GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["convolve_image"].emplace(Shader({
        {"irradiance_convolve.cs", GL_COMPUTE_SHADER}
    }));

    // ---- IBL cubemap suite ----
    Shader::shaders["equirect_to_cube"].emplace(Shader({
        {"cubemap_capture.vs", GL_VERTEX_SHADER},
        {"equirect_to_cube.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["diffuse_irradiance"].emplace(Shader({
        {"cubemap_capture.vs", GL_VERTEX_SHADER},
        {"diffuse_irradiance.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["prefilter_env"].emplace(Shader({
        {"cubemap_capture.vs", GL_VERTEX_SHADER},
        {"prefilter_env.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["brdf_lut"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"brdf_lut.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["skybox_cube"].emplace(Shader({
        {"skybox_cube.vs", GL_VERTEX_SHADER},
        {"skybox_cube.fs", GL_FRAGMENT_SHADER}
    }));

    // Gaussian blur variants — 24 shaders (4 formats × 6 radii)
    for (int r = 1; r <= 6; ++r) {
        std::string rs = std::to_string(r);

        Shader::shaders["gaussian_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n"}
        }));
        Shader::shaders["gaussian32f_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n#define FORMAT R32f\n"}
        }));
        Shader::shaders["gaussianRGBA32f_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n#define FORMAT RGBA32f\n"}
        }));
        Shader::shaders["gaussianRGBA16f_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n#define FORMAT RGBA16f\n"}
        }));
    }

    // Terrain G-buffer writer (triplanar + 4-material splat blend). Used by
    // BOTH the game client and the GUE editor's exact-4-material path.
    Shader::shaders["terrainGBuffer"].emplace(Shader({
        {"terrainGBuffer.vs", GL_VERTEX_SHADER},
        {"terrainGBuffer.fs", GL_FRAGMENT_SHADER}
    }));

    // Terrain G-buffer writer, generalized N-material path (GUE editor
    // authoring preview only — the game client always uses terrainGBuffer
    // above). Separate program, own texture-unit budget, so it can never
    // collide with or destabilize the legacy shader — see docs/TECH_DEBT.md
    // "Terrain multi-material authoring (Phase 1)". Reuses the same vertex
    // shader (identical vertex stage, only the fragment blend differs).
    Shader::shaders["terrainGBufferExt"].emplace(Shader({
        {"terrainGBuffer.vs",    GL_VERTEX_SHADER},
        {"terrainGBufferExt.fs", GL_FRAGMENT_SHADER}
    }));

    // Brush overlay ring — used by tools/terrain-editor as forward pass
    Shader::shaders["brush_overlay"].emplace(Shader({
        {"brush_overlay.vs", GL_VERTEX_SHADER},
        {"brush_overlay.fs", GL_FRAGMENT_SHADER}
    }));

    // Simple forward preview — static (non-skinned) mesh preview in the GUE.
    // No bindless textures, no SSBO: plain sampler2D + Phong lighting.
    Shader::shaders["preview_static"].emplace(Shader({
        {"preview_static.vs", GL_VERTEX_SHADER},
        {"preview_static.fs", GL_FRAGMENT_SHADER}
    }));

    // Particle system — forward pass with GL_POINTS + additive blend
    Shader::shaders["particle"].emplace(Shader({
        {"particle.vert", GL_VERTEX_SHADER},
        {"particle.frag", GL_FRAGMENT_SHADER}
    }));

    // Water Phase 0 — forward pass, textured quad, alpha blend. No waves/
    // depth/reflection yet (see doc/TECH_DEBT.md). Same file also compiled
    // standalone by the GUE's ZoneRenderer for the editor preview.
    Shader::shaders["water"].emplace(Shader({
        {"water.vs", GL_VERTEX_SHADER},
        {"water.fs", GL_FRAGMENT_SHADER}
    }));

    // Ripple sim Phase (a) — standalone Hugo Elias height-field update,
    // not yet sampled by water.vs/water.fs (see rco::renderer::RippleSim).
    Shader::shaders["ripple_update"].emplace(Shader({
        {"fullscreen_tri.vs",  GL_VERTEX_SHADER},
        {"ripple_update.fs",   GL_FRAGMENT_SHADER}
    }));

    // Ripple sim window recenter — shifts the height buffer's contents by
    // an integer texel offset (RippleSim::Update, only runs on a hysteresis
    // recenter) so existing ripples stay attached to their world position
    // instead of jumping/disappearing when the player-following window
    // moves. See RippleSim::Update's shift-pass comment.
    Shader::shaders["ripple_shift"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"ripple_shift.fs",   GL_FRAGMENT_SHADER}
    }));
}

} // namespace rco::renderer
