#include "rco/renderer/pipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>

#include "rco/renderer/buffers.h"
#include "rco/renderer/helpers.h"
#include "rco/renderer/indirect.h"
#include "rco/renderer/mesh.h"
#include "rco/renderer/object.h"
#include "rco/renderer/shader.h"

namespace rco::renderer {

Pipeline::Pipeline(Engine& e) : engine_(&e) {
    sun_.direction = glm::normalize(glm::vec3(1.0f, -0.5f, 0.0f));
    sun_.diffuse   = glm::vec3(1.0f);
}

Pipeline::~Pipeline() = default;

void Pipeline::SetFeatures(const FeatureConfig& cfg) { features_ = cfg; }

void Pipeline::SetSun(const glm::vec3& dir, const glm::vec3& col) {
    sun_.direction = glm::normalize(dir);
    sun_.diffuse   = col;
}

void Pipeline::Begin(const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& cam_pos, float dt) {
    view_     = view;
    proj_     = proj;
    viewProj_ = proj * view;
    camPos_   = cam_pos;
    dt_       = dt;
    localLights_.clear();
    dynamicDraws_.clear();
    skinnedDraws_.clear();
    terrainChunks_.clear();
}

void Pipeline::AddPointLight(const glm::vec3& pos, const glm::vec3& color, float radius) {
    PointLight p{};
    p.diffuse       = glm::vec4(color, 0.0f);
    p.position      = glm::vec4(pos,   0.0f);
    p.linear        = 0.0f;
    p.quadratic     = 1.0f / glm::max(0.001f, radius * radius);
    p.radiusSquared = radius * radius;
    localLights_.push_back(p);
}

void Pipeline::SubmitDynamic(const DynamicDrawRequest& r) { dynamicDraws_.push_back(r); }
void Pipeline::SubmitSkinned(const DynamicDrawRequest& r) { skinnedDraws_.push_back(r); }
void Pipeline::SubmitTerrainChunk(const TerrainChunkSubmission& c) { terrainChunks_.push_back(c); }

void Pipeline::SubmitStaticScene() {
    // No-op: actual draw happens in gBufferPass_ via engine_->drawIndirectBuffer_
}

// ---------------------------------------------------------------------------
// computeLightMatrix_
// ---------------------------------------------------------------------------

void Pipeline::computeLightMatrix_() {
    glm::vec3 center = glm::vec3(camPos_.x, 0.0f, camPos_.z);
    const glm::vec3 sunPos = -glm::normalize(sun_.direction) * 200.0f + center + glm::vec3(0, 30, 0);
    lightMat_ = MakeLightMatrix(sun_, sunPos, glm::vec2(120.0f), glm::vec2(1.0f, 350.0f));
}

// ---------------------------------------------------------------------------
// shadowPass_
// ---------------------------------------------------------------------------

void Pipeline::shadowPass_() {
    if (!engine_->drawIndirectBuffer_) return;

    glViewport(0, 0, engine_->shadowWidth_, engine_->shadowHeight_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->shadowFbo_);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    std::vector<glm::mat4> uniforms;
    uniforms.reserve(engine_->staticMeshes_.size());
    for (std::size_t i = 0; i < engine_->staticMeshes_.size(); ++i) {
        uniforms.emplace_back(lightMat_);
    }
    StaticBuffer uniformBuffer(uniforms.data(), uniforms.size() * sizeof(glm::mat4), 0);

    auto& shadowShader = Shader::shaders["shadowBindless"];
    shadowShader->Bind();
    uniformBuffer.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
    engine_->drawIndirectBuffer_->Bind(GL_DRAW_INDIRECT_BUFFER);
    glVertexArrayVertexBuffer(engine_->vao_, 0,
        engine_->vertexBuffer_->GetBufferHandle(), 0, sizeof(Vertex));
    glVertexArrayElementBuffer(engine_->vao_,
        engine_->indexBuffer_->GetBufferHandle());
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, 0,
        static_cast<GLsizei>(uniforms.size()),
        sizeof(DrawElementsIndirectCommand));

    // Shadow copy/blur for VSM/ESM/MSM
    filteredShadowTex_ = 0;
    switch (features_.shadow_method) {
        case SHADOW_METHOD_VSM: filteredShadowTex_ = engine_->vshadowDepthGoodFormat_; break;
        case SHADOW_METHOD_ESM: filteredShadowTex_ = engine_->eExpShadowDepth_;        break;
        case SHADOW_METHOD_MSM: filteredShadowTex_ = engine_->msmShadowMoments_;       break;
        default: break;
    }
    if (features_.shadow_method == SHADOW_METHOD_VSM
     || features_.shadow_method == SHADOW_METHOD_ESM
     || features_.shadow_method == SHADOW_METHOD_MSM) {
        glBindTextureUnit(0, engine_->shadowDepth_);

        const char* shaderName = "none";
        GLuint fbo = 0;
        switch (features_.shadow_method) {
            case SHADOW_METHOD_VSM: shaderName = "vsm_copy"; fbo = engine_->vshadowGoodFormatFbo_; break;
            case SHADOW_METHOD_ESM: shaderName = "esm_copy"; fbo = engine_->eShadowFbo_;           break;
            case SHADOW_METHOD_MSM: shaderName = "msm_copy"; fbo = engine_->msmShadowFbo_;         break;
        }
        auto& copyShader = Shader::shaders[shaderName];
        copyShader->Bind();
        if (features_.shadow_method == SHADOW_METHOD_ESM) {
            copyShader->SetFloat("u_C", sunConstantC_);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        const GLuint W = engine_->shadowWidth_;
        const GLuint H = engine_->shadowHeight_;
        if (features_.shadow_method == SHADOW_METHOD_VSM) {
            blurTextureRG32f(engine_->vshadowDepthGoodFormat_, engine_->vshadowMomentBlur_,
                             W, H, blurPasses_, blurStrength_);
        } else if (features_.shadow_method == SHADOW_METHOD_ESM) {
            blurTextureR32f(engine_->eExpShadowDepth_, engine_->eShadowDepthBlur_,
                            W, H, blurPasses_, blurStrength_);
        } else if (features_.shadow_method == SHADOW_METHOD_MSM) {
            blurTextureRGBA32f(engine_->msmShadowMoments_, engine_->msmShadowMomentsBlur_,
                               W, H, blurPasses_, blurStrength_);
        }
    }
}

// ---------------------------------------------------------------------------
// gBufferPass_
// ---------------------------------------------------------------------------

void Pipeline::gBufferPass_() {
    glViewport(0, 0, engine_->width_, engine_->height_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->gfbo_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Static scene (multi-draw indirect)
    if (engine_->drawIndirectBuffer_ && !engine_->staticMeshes_.empty()) {
        std::vector<ObjectUniforms> uniforms;
        uniforms.reserve(engine_->staticMeshes_.size());
        for (const auto& rec : engine_->staticMeshes_) {
            uniforms.push_back(ObjectUniforms{
                .modelMatrix   = glm::mat4(1.0f),
                .materialIndex = static_cast<uint32_t>(rec.material_index),
            });
        }
        StaticBuffer uniformBuffer(uniforms.data(), sizeof(ObjectUniforms) * uniforms.size(), 0);

        auto& sh = Shader::shaders["gBufferBindless"];
        sh->Bind();
        sh->SetMat4 ("u_viewProj",                viewProj_);
        sh->SetBool ("u_materialOverride",         false);
        sh->SetVec3 ("u_albedoOverride",           glm::vec3(1.0f));
        sh->SetFloat("u_roughnessOverride",        0.5f);
        sh->SetFloat("u_metalnessOverride",        0.0f);
        sh->SetFloat("u_AOoverride",               0.0f);
        sh->SetFloat("u_ambientOcclusionOverride", 1.0f);
        uniformBuffer.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
        engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        engine_->drawIndirectBuffer_->Bind(GL_DRAW_INDIRECT_BUFFER);
        glVertexArrayVertexBuffer(engine_->vao_, 0,
            engine_->vertexBuffer_->GetBufferHandle(), 0, sizeof(Vertex));
        glVertexArrayElementBuffer(engine_->vao_,
            engine_->indexBuffer_->GetBufferHandle());
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, 0,
            static_cast<GLsizei>(uniforms.size()),
            sizeof(DrawElementsIndirectCommand));
    }

    // Dynamic (non-skinned) draws — VAO is assumed to be pre-configured by the caller
    // (has its own vertex/index buffers bound). Only fall back to the engine's global
    // VAO + explicit VBO/EBO when the caller didn't provide a VAO.
    if (!dynamicDraws_.empty() && engine_->materialsBuffer_) {
        auto& sh = Shader::shaders["gBufferBindless"];
        sh->Bind();
        sh->SetMat4("u_viewProj", viewProj_);
        engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        for (const auto& r : dynamicDraws_) {
            ObjectUniforms u{ r.model, static_cast<uint32_t>(r.material_idx) };
            StaticBuffer ub(&u, sizeof(u), 0);
            ub.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
            if (r.vao) {
                glBindVertexArray(r.vao);
            } else {
                glBindVertexArray(engine_->vao_);
                if (r.vbo) glVertexArrayVertexBuffer(engine_->vao_, 0, r.vbo, 0, sizeof(Vertex));
                if (r.ebo) glVertexArrayElementBuffer(engine_->vao_, r.ebo);
            }
            glDrawElements(GL_TRIANGLES, r.index_count, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(engine_->vao_);
    }

    // Skinned draws — use gBufferSkinned shader variant with bone SSBO at binding 2.
    if (!skinnedDraws_.empty() && engine_->materialsBuffer_) {
        auto& sh = Shader::shaders["gBufferSkinned"];
        sh->Bind();
        sh->SetMat4("u_viewProj", viewProj_);
        engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        for (const auto& r : skinnedDraws_) {
            ObjectUniforms u{ r.model, static_cast<uint32_t>(r.material_idx) };
            StaticBuffer ub(&u, sizeof(u), 0);
            ub.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
            if (r.bone_ssbo) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, r.bone_ssbo);
            }
            // Skinned meshes MUST use their own VAO (attributes 4/5 for bone ids/weights).
            GLuint vao = r.vao ? r.vao : engine_->vao_;
            glBindVertexArray(vao);
            glDrawElements(GL_TRIANGLES, r.index_count, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(engine_->vao_);
    }
}

// ---------------------------------------------------------------------------
// terrainPass_ — writes terrain chunks into the G-buffer (same attachments
// as gBufferPass_). Runs after gBufferPass_ and before lighting so all
// deferred effects (CSM shadows, SSAO, IBL) apply transparently.
// ---------------------------------------------------------------------------

void Pipeline::terrainPass_() {
    if (terrainChunks_.empty()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, engine_->gfbo_);
    glViewport(0, 0, engine_->width_, engine_->height_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    auto& sh = Shader::shaders["terrainGBuffer"];
    sh->Bind();
    sh->SetMat4("u_viewProj", viewProj_);

    for (const auto& c : terrainChunks_) {
        sh->SetMat4 ("u_model",         c.model);
        sh->SetFloat("u_tiling",        c.tiling);
        sh->SetVec2 ("u_terrainOrigin", c.terrain_origin.x, c.terrain_origin.y);
        sh->SetVec2 ("u_terrainSize",   c.terrain_size.x,   c.terrain_size.y);

        glBindTextureUnit(0, c.splatmap);
        sh->SetInt("u_splatmap", 0);
        for (int i = 0; i < 4; ++i) {
            glBindTextureUnit(1 + i*3 + 0, c.mat_albedo[i]);
            glBindTextureUnit(1 + i*3 + 1, c.mat_normal[i]);
            glBindTextureUnit(1 + i*3 + 2, c.mat_roughness[i]);
        }
        sh->SetInt("u_mat0_albedo",  1); sh->SetInt("u_mat0_normal",  2); sh->SetInt("u_mat0_roughness",  3);
        sh->SetInt("u_mat1_albedo",  4); sh->SetInt("u_mat1_normal",  5); sh->SetInt("u_mat1_roughness",  6);
        sh->SetInt("u_mat2_albedo",  7); sh->SetInt("u_mat2_normal",  8); sh->SetInt("u_mat2_roughness",  9);
        sh->SetInt("u_mat3_albedo", 10); sh->SetInt("u_mat3_normal", 11); sh->SetInt("u_mat3_roughness", 12);

        glBindVertexArray(c.vao);
        glDrawElements(GL_TRIANGLES, c.index_count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(engine_->vao_);
}

// ---------------------------------------------------------------------------
// ssaoPass_
// ---------------------------------------------------------------------------

void Pipeline::ssaoPass_() {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->ssaoFbo_);
    float clearWhite[] = { 1, 1, 1, 1 };
    glClearNamedFramebufferfv(engine_->ssaoFbo_, GL_COLOR, 0, clearWhite);
    glBindTextureUnit(0, engine_->gDepth_);
    glBindTextureUnit(1, engine_->gNormal_);

    if (!features_.ssao) return;

    auto& ssaoShader = Shader::shaders["ssao"];
    ssaoShader->Bind();
    ssaoShader->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
    ssaoShader->SetMat4 ("u_view",        view_);
    ssaoShader->SetUInt ("u_numSamples",  (unsigned)ssao_.samples_near);
    ssaoShader->SetFloat("u_delta",       ssao_.delta);
    ssaoShader->SetFloat("u_R",           ssao_.range);
    ssaoShader->SetFloat("u_s",           ssao_.s);
    ssaoShader->SetFloat("u_k",           ssao_.k);
    glNamedFramebufferTexture(engine_->ssaoFbo_, GL_COLOR_ATTACHMENT0, engine_->ssaoTex_, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (ssao_.atrous_passes > 0) {
        glBindTextureUnit(1, engine_->gDepth_);
        glBindTextureUnit(2, engine_->gNormal_);
        auto& blur = Shader::shaders["atrous_ssao"];
        blur->Bind();
        blur->SetFloat("n_phi",    ssao_.atrous_n_phi);
        blur->SetFloat("p_phi",    ssao_.atrous_p_phi);
        blur->SetFloat("stepwidth", ssao_.atrous_step_width);
        blur->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
        blur->SetIVec2("u_resolution",  engine_->width_, engine_->height_);
        blur->Set1FloatArray("kernel[0]",  ssao_.atrous_kernel);
        blur->Set1FloatArray("offsets[0]", ssao_.atrous_offsets);
        for (int i = 0; i < ssao_.atrous_passes; ++i) {
            float offsets2[5];
            for (int j = 0; j < 5; ++j)
                offsets2[j] = ssao_.atrous_offsets[j] * glm::pow(2.0f, (float)i);
            blur->Set1FloatArray("offsets[0]", offsets2);
            blur->SetBool("u_horizontal", false);
            glBindTextureUnit(0, engine_->ssaoTex_);
            glNamedFramebufferTexture(engine_->ssaoFbo_, GL_COLOR_ATTACHMENT0, engine_->ssaoBlurred_, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            blur->SetBool("u_horizontal", true);
            glBindTextureUnit(0, engine_->ssaoBlurred_);
            glNamedFramebufferTexture(engine_->ssaoFbo_, GL_COLOR_ATTACHMENT0, engine_->ssaoTex_, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
}

// ---------------------------------------------------------------------------
// globalLightPass_
// ---------------------------------------------------------------------------

void Pipeline::globalLightPass_() {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->hdrFbo_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindTextureUnit(0, engine_->gNormal_);
    glBindTextureUnit(1, engine_->gAlbedo_);
    glBindTextureUnit(2, engine_->gRMA_);
    glBindTextureUnit(3, engine_->gDepth_);
    glBindTextureUnit(4, engine_->ssaoTex_);
    glBindTextureUnit(5, filteredShadowTex_);
    // IBL split-sum suite: irradiance cubemap + prefiltered specular cubemap + BRDF LUT.
    if (engine_->irradianceCube_) glBindTextureUnit(6, engine_->irradianceCube_);
    if (engine_->prefilterCube_)  glBindTextureUnit(7, engine_->prefilterCube_);
    if (engine_->brdfLUT_)        glBindTextureUnit(8, engine_->brdfLUT_);

    glDisable(GL_DEPTH_TEST);
    auto& sh = Shader::shaders["gPhongGlobal"];
    sh->Bind();
    sh->SetInt  ("u_shadowMethod",          features_.shadow_method);
    sh->SetFloat("u_C",                     sunConstantC_);
    sh->SetVec3 ("u_viewPos",               camPos_);
    sh->SetIVec2("u_screenSize",            engine_->width_, engine_->height_);
    sh->SetMat4 ("u_invViewProj",           glm::inverse(viewProj_));
    sh->SetVec3 ("u_globalLight_diffuse",   sun_.diffuse);
    sh->SetVec3 ("u_globalLight_direction", sun_.direction);
    sh->SetMat4 ("u_lightMatrix",           lightMat_);
    sh->SetFloat("u_lightBleedFix",         vlightBleedFix_);
    sh->SetInt  ("u_debugMode",             debugMode_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---------------------------------------------------------------------------
// localLightsPass_
// ---------------------------------------------------------------------------

void Pipeline::localLightsPass_() {
    if (localLights_.empty()) return;

    lightSSBO_ = std::make_unique<StaticBuffer>(
        localLights_.data(), localLights_.size() * sizeof(PointLight), 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    auto& sh = Shader::shaders["gPhongManyLocal"];
    sh->Bind();
    sh->SetMat4("u_viewProj",    viewProj_);
    sh->SetMat4("u_invViewProj", glm::inverse(viewProj_));
    sh->SetVec3("u_viewPos",     camPos_);
    sh->SetInt ("gNormal", 0);
    sh->SetInt ("gAlbedo", 1);
    sh->SetInt ("gRMA",    2);
    sh->SetInt ("gDepth",  3);
    lightSSBO_->BindBase(GL_SHADER_STORAGE_BUFFER, 0);

    Mesh& sph = GetUnitLightSphere();
    glVertexArrayVertexBuffer(engine_->vao_, 0, sph.GetVBOID(), 0, sizeof(Vertex));
    glVertexArrayElementBuffer(engine_->vao_, sph.GetEBOID());
    glDrawElementsInstanced(GL_TRIANGLES,
        static_cast<GLsizei>(sph.GetVertexCount()),
        GL_UNSIGNED_INT, nullptr,
        static_cast<GLsizei>(localLights_.size()));

    glCullFace(GL_BACK);
}

// ---------------------------------------------------------------------------
// skyboxPass_
// ---------------------------------------------------------------------------

void Pipeline::skyboxPass_() {
    glBlitNamedFramebuffer(engine_->gfbo_, engine_->hdrFbo_,
        0, 0, engine_->width_, engine_->height_,
        0, 0, engine_->width_, engine_->height_,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_ONE, GL_ZERO);

    if (!engine_->envCubemap_) return;

    auto& sh = Shader::shaders["skybox_cube"];
    sh->Bind();
    sh->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
    sh->SetVec3 ("u_camPos",      camPos_);
    sh->SetInt  ("u_envCube",     0);
    glBindTextureUnit(0, engine_->envCubemap_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---------------------------------------------------------------------------
// volumetricPass_
// ---------------------------------------------------------------------------

void Pipeline::volumetricPass_() {
    if (!features_.volumetrics) return;

    glViewport(0, 0, engine_->width_, engine_->height_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->volumetricsFbo_);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_FALSE);

    glBindTextureUnit(1, engine_->gDepth_);
    glBindTextureUnit(2, engine_->shadowDepth_);
    if (engine_->bluenoiseTex_)
        glBindTextureUnit(3, engine_->bluenoiseTex_->GetID());

    auto& vol = Shader::shaders["volumetric"];
    vol->Bind();
    vol->SetMat4 ("u_invViewProj",    glm::inverse(viewProj_));
    vol->SetMat4 ("u_lightMatrix",    lightMat_);
    vol->SetIVec2("u_screenSize",     engine_->width_, engine_->height_);
    vol->SetInt  ("NUM_STEPS",        volumetric_.steps);
    vol->SetFloat("intensity",        volumetric_.intensity);
    vol->SetFloat("noiseOffset",      volumetric_.noiseOffset);
    vol->SetFloat("u_beerPower",      volumetric_.beerPower);
    vol->SetFloat("u_powderPower",    volumetric_.powderPower);
    vol->SetFloat("u_distanceScale",  volumetric_.distanceScale);
    vol->SetFloat("u_heightOffset",   volumetric_.heightOffset);
    vol->SetFloat("u_hfIntensity",    volumetric_.hfIntensity);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (volumetric_.atrous_passes > 0) {
        glViewport(0, 0, engine_->width_, engine_->height_);
        glBindFramebuffer(GL_FRAMEBUFFER, engine_->volumetricsAtrousFbo_);

        auto& atrous = Shader::shaders["atrous_volumetric"];
        atrous->Bind();
        atrous->SetInt  ("gColor",       0);
        atrous->SetFloat("c_phi",        volumetric_.c_phi);
        atrous->SetFloat("stepwidth",    volumetric_.stepWidth);
        atrous->SetIVec2("u_resolution", engine_->width_, engine_->height_);
        atrous->Set1FloatArray("kernel[0]",  volumetric_.atrouskernel);
        atrous->Set2FloatArray("offsets[0]", volumetric_.atrouskerneloffsets);

        glNamedFramebufferDrawBuffer(engine_->volumetricsAtrousFbo_, GL_COLOR_ATTACHMENT0);
        glBindTextureUnit(0, engine_->volumetricsTex_);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (volumetric_.atrous_passes == 2) {
            glm::vec2 offsets2[25];
            for (int i = 0; i < 25; i++)
                offsets2[i] = volumetric_.atrouskerneloffsets[i] * 2.0f;
            atrous->Set2FloatArray("offsets[0]", offsets2);
            glNamedFramebufferDrawBuffer(engine_->volumetricsAtrousFbo_, GL_COLOR_ATTACHMENT1);
            glBindTextureUnit(0, engine_->volumetricsAtrousTex_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
}

// ---------------------------------------------------------------------------
// ssrPass_
// ---------------------------------------------------------------------------

void Pipeline::ssrPass_() {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glDepthMask(GL_FALSE);

    if (!features_.ssr) return;

    glViewport(0, 0, engine_->ssrWidth_, engine_->ssrHeight_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->ssrFbo_);
    glClear(GL_COLOR_BUFFER_BIT);

    auto& sh = Shader::shaders["ssr"];
    sh->Bind();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTextureUnit(0, engine_->hdrColorTex_);
    glBindTextureUnit(1, engine_->gDepth_);
    glBindTextureUnit(2, engine_->gRMA_);
    glBindTextureUnit(3, engine_->gNormal_);
    sh->SetMat4 ("u_proj",         proj_);
    sh->SetMat4 ("u_view",         view_);
    sh->SetMat4 ("u_invViewProj",  glm::inverse(viewProj_));
    sh->SetFloat("rayStep",         ssr_.rayStep);
    sh->SetFloat("minRayStep",      ssr_.minRayStep);
    sh->SetFloat("thickness",       ssr_.thickness);
    sh->SetFloat("searchDist",      ssr_.searchDist);
    sh->SetInt  ("maxSteps",        ssr_.maxRaySteps);
    sh->SetInt  ("binarySearchSteps", ssr_.binarySearchSteps);
    sh->SetIVec2("u_viewportSize",  (int)engine_->ssrWidth_, (int)engine_->ssrHeight_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---------------------------------------------------------------------------
// compositePass_
// ---------------------------------------------------------------------------

void Pipeline::compositePass_() {
    glViewport(0, 0, engine_->width_, engine_->height_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
    glBlendFunc(GL_ONE, GL_ONE);
    glClear(GL_COLOR_BUFFER_BIT);

    auto& fs = Shader::shaders["fstexture"];
    fs->Bind();
    fs->SetInt("u_texture", 0);
    glBindTextureUnit(0, engine_->hdrColorTex_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (features_.volumetrics) {
        GLuint volTex = (volumetric_.atrous_passes % 2 == 0)
            ? engine_->volumetricsTex_
            : engine_->volumetricsAtrousTex_;
        auto& vc = Shader::shaders["vol_composite"];
        vc->Bind();
        vc->SetInt("u_texture", 0);
        glBindTextureUnit(0, volTex);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        fs->Bind();
    }

    if (features_.ssr) {
        glBindTextureUnit(0, engine_->ssrTex_);
        glBlendFunc(GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
}

// ---------------------------------------------------------------------------
// tonemappingPass_
// ---------------------------------------------------------------------------

void Pipeline::tonemappingPass_(float dt) {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
    glDisable(GL_BLEND);

    glBindTextureUnit(1, engine_->postprocessColor_);

    const float logLowLum     = glm::log(hdr_.targetLuminance / hdr_.maxExposure);
    const float logMaxLum     = glm::log(hdr_.targetLuminance / hdr_.minExposure);
    const int   computePixelsX = engine_->width_  / 2;
    const int   computePixelsY = engine_->height_ / 2;

    {
        auto& hshdr = Shader::shaders["generate_histogram"];
        hshdr->Bind();
        hshdr->SetInt  ("u_hdrBuffer",  1);
        hshdr->SetFloat("u_logLowLum", logLowLum);
        hshdr->SetFloat("u_logMaxLum", logMaxLum);
        constexpr int X_SIZE = 8, Y_SIZE = 8;
        engine_->histogramBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 0);
        glDispatchCompute((computePixelsX + X_SIZE - 1) / X_SIZE,
                          (computePixelsY + Y_SIZE - 1) / Y_SIZE, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    {
        engine_->exposureBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 0);
        engine_->histogramBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        auto& cshdr = Shader::shaders["calc_exposure"];
        cshdr->Bind();
        cshdr->SetFloat("u_dt",              dt);
        cshdr->SetFloat("u_adjustmentSpeed", hdr_.adjustmentSpeed);
        cshdr->SetFloat("u_logLowLum",       logLowLum);
        cshdr->SetFloat("u_logMaxLum",       logMaxLum);
        cshdr->SetFloat("u_targetLuminance", hdr_.targetLuminance);
        cshdr->SetInt  ("u_numPixels",       computePixelsX * computePixelsY);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glViewport(0, 0, engine_->width_, engine_->height_);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    auto& shdr = Shader::shaders["tonemap"];
    shdr->Bind();
    shdr->SetFloat("u_exposureFactor", hdr_.exposureFactor);
    shdr->SetInt  ("u_hdrBuffer",      1);
    shdr->SetInt  ("u_debugBypass",    debugMode_ != 0 ? 1 : 0);
    glNamedFramebufferTexture(engine_->postprocessFbo_, GL_COLOR_ATTACHMENT0,
                              engine_->postprocessPostSRGB_, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glNamedFramebufferTexture(engine_->postprocessFbo_, GL_COLOR_ATTACHMENT0,
                              engine_->postprocessColor_, 0);
}

// ---------------------------------------------------------------------------
// fxaaPass_ + finalBlit_
// ---------------------------------------------------------------------------

void Pipeline::fxaaPass_() {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
    glBlendFunc(GL_ONE, GL_ONE);
    glNamedFramebufferTexture(engine_->postprocessFbo_, GL_COLOR_ATTACHMENT0,
                              engine_->legitFinalImage_, 0);
    if (features_.fxaa) {
        glBindTextureUnit(0, engine_->postprocessPostSRGB_);
        auto& sh = Shader::shaders["fxaa"];
        sh->Bind();
        sh->SetVec2 ("u_invScreenSize",
                     1.0f / (float)engine_->width_, 1.0f / (float)engine_->height_);
        sh->SetFloat("u_contrastThreshold",  fxaa_.contrastThreshold);
        sh->SetFloat("u_relativeThreshold",  fxaa_.relativeThreshold);
        sh->SetFloat("u_pixelBlendStrength", fxaa_.pixelBlendStrength);
        sh->SetFloat("u_edgeBlendStrength",  fxaa_.edgeBlendStrength);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        drawFSTexture(engine_->postprocessPostSRGB_);
    }
    glNamedFramebufferTexture(engine_->postprocessFbo_, GL_COLOR_ATTACHMENT0,
                              engine_->postprocessColor_, 0);
}

void Pipeline::finalBlit_() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    drawFSTexture(engine_->legitFinalImage_);
}

// ---------------------------------------------------------------------------
// End — orchestrate all passes
// ---------------------------------------------------------------------------

void Pipeline::End() { End(EndConfig{}); }

void Pipeline::End(const std::function<void()>& forward_pass) {
    EndConfig cfg{};
    cfg.forward_pass = forward_pass;
    End(cfg);
}

void Pipeline::End(const EndConfig& cfg) {
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL);
    glBindVertexArray(engine_->vao_);

    computeLightMatrix_();
    shadowPass_();
    gBufferPass_();
    terrainPass_();
    ssaoPass_();
    globalLightPass_();
    localLightsPass_();
    skyboxPass_();
    volumetricPass_();
    ssrPass_();
    compositePass_();

    // Forward pass for transparent/additive draws (particles, brush overlays).
    // postprocessFbo_ has gDepth_ as its depth attachment → coherent depth test.
    if (cfg.forward_pass) {
        glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
        glViewport(0, 0, engine_->width_, engine_->height_);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        cfg.forward_pass();
        glDepthMask(GL_TRUE);
    }

    tonemappingPass_(dt_);
    fxaaPass_();
    if (cfg.blit_to_default) {
        finalBlit_();
    }
    // When blit_to_default == false, the caller samples engine.finalImage() as
    // ImTextureID and handles presentation (e.g. ImGui::Image inside a panel).
}

} // namespace rco::renderer
