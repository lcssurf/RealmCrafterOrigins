#include "rco/renderer/engine.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "rco/renderer/helpers.h"
#include "rco/renderer/object.h"
#include "rco/renderer/shader.h"

#include <glm/gtc/matrix_transform.hpp>

namespace rco::renderer {

namespace {

// Unit cube vertices (CCW from outside) — used as the input geometry for the
// IBL capture passes. Each face is rendered with a 90° perspective so the
// fragment shader can read gl_LocalPos as a direction vector.
constexpr float kCubeVerts[] = {
    // back
    -1, -1, -1,   1, -1, -1,   1,  1, -1,
     1,  1, -1,  -1,  1, -1,  -1, -1, -1,
    // front
    -1, -1,  1,   1,  1,  1,   1, -1,  1,
     1,  1,  1,  -1, -1,  1,  -1,  1,  1,
    // left
    -1,  1,  1,  -1, -1, -1,  -1,  1, -1,
    -1, -1, -1,  -1,  1,  1,  -1, -1,  1,
    // right
     1,  1,  1,   1,  1, -1,   1, -1, -1,
     1, -1, -1,   1, -1,  1,   1,  1,  1,
    // bottom
    -1, -1, -1,   1, -1,  1,   1, -1, -1,
     1, -1,  1,  -1, -1, -1,  -1, -1,  1,
    // top
    -1,  1, -1,   1,  1, -1,   1,  1,  1,
     1,  1,  1,  -1,  1,  1,  -1,  1, -1,
};

void RenderUnitCube(GLuint vao) {
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

} // namespace

namespace {
void GLAPIENTRY debugCB(GLenum /*source*/, GLenum /*type*/, GLuint id,
                        GLenum severity, GLsizei /*length*/,
                        const GLchar* message, const void* /*userParam*/) {
    // Drop notifications and low-severity driver chatter. NVIDIA in particular
    // spams "generic vertex attribute array N uses a pointer with a small
    // value (0x0)" every draw call when using DSA with offset 0 at an
    // attribute binding, which is the correct idiom.
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (severity == GL_DEBUG_SEVERITY_LOW)          return;

    // Known-noisy NVIDIA perf warning IDs we never want.
    switch (id) {
    case 131186:   // "Buffer object uses VIDEO memory ..."
    case 131204:   // texture state recompile
    case 131218:   // shader state recompile
        return;
    default: break;
    }

    std::cerr << "[GL] " << message << "\n";
}
} // namespace

// ---------------------------------------------------------------------------
// Init / Shutdown / Resize
// ---------------------------------------------------------------------------

void Engine::installDebugCallback_() {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    // Keep the callback filter explicit (in addition to callback-side drops)
    // so drivers stop bothering to generate those messages at all.
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION,
                          0, nullptr, GL_FALSE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW,
                          0, nullptr, GL_FALSE);
    glDebugMessageCallback(debugCB, nullptr);
}

void Engine::Init(const EngineConfig& cfg) {
    width_             = cfg.width;
    height_            = cfg.height;
    shadowWidth_       = cfg.shadow_width;
    shadowHeight_      = cfg.shadow_height;
    shadowLevels_      = (GLuint)glm::ceil(glm::log2(
        (float)glm::max(shadowWidth_, shadowHeight_)));
    maxStaticVertices_ = cfg.max_static_vertices;

    if (!glGetString) {
        throw std::runtime_error(
            "Engine::Init called before gladLoadGL — load the GL function pointers first");
    }

    if (cfg.enable_debug_output) {
        installDebugCallback_();
    }

    // -------- OpenGL feature requirements --------
    {
        GLint numExt = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
        bool hasBindless = false;
        for (GLint i = 0; i < numExt; ++i) {
            const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (e && std::strcmp(e, "GL_ARB_bindless_texture") == 0) {
                hasBindless = true;
                break;
            }
        }
        if (!hasBindless) {
            const char* vendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            const char* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            std::fprintf(stderr,
                "[rco_renderer] FATAL: GL_ARB_bindless_texture not supported.\n"
                "  Vendor:   %s\n"
                "  Renderer: %s\n"
                "  Version:  %s\n"
                "Upgrade your GPU driver, or run on a GPU that supports bindless\n"
                "textures (GTX 700+ on NVIDIA, GCN 1.2+ on AMD, Arc on Intel).\n",
                vendor   ? vendor   : "?",
                renderer ? renderer : "?",
                version  ? version  : "?");
            throw std::runtime_error("GL_ARB_bindless_texture unavailable");
        }
    }

    {
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major < 4 || (major == 4 && minor < 6)) {
            std::fprintf(stderr,
                "[rco_renderer] FATAL: OpenGL 4.6 required, got %d.%d\n", major, minor);
            throw std::runtime_error("OpenGL 4.6 unavailable");
        }
    }

    glEnable(GL_MULTISAMPLE);
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &deviceAnisotropy_);

    TextureCreateInfo bnInfo{
        .path        = "assets/textures/bluenoise_64.png",
        .sRGB        = false,
        .generateMips = false,
        .HDR         = false,
        .minFilter   = GL_LINEAR,
        .magFilter   = GL_LINEAR,
    };
    bluenoiseTex_ = std::make_unique<Texture2D>(bnInfo);

    vertexBuffer_ = std::make_unique<DynamicBuffer>(
        sizeof(Vertex) * maxStaticVertices_, sizeof(Vertex));
    indexBuffer_ = std::make_unique<DynamicBuffer>(
        sizeof(uint32_t) * maxStaticVertices_, sizeof(uint32_t));

    createFramebuffers_();
    createVAO_();

    // Default materialsBuffer — one zero-handle entry. The gBuffer fragment
    // shader checks handle != 0 before sampling, so index 0 renders flat gray
    // until BeginStaticScene/EndStaticScene populates real materials, or the
    // actor material pipeline lands. This lets dynamic/skinned draws run
    // without a static scene.
    {
        BindlessMaterial zero{};
        materialsBuffer_ = std::make_unique<StaticBuffer>(
            &zero, sizeof(BindlessMaterial), 0);
    }

    Shader::SetShaderDir(cfg.shader_dir);
    CompileAllShaders();
}

void Engine::Shutdown() {
    destroyFramebuffers_();
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    vertexBuffer_.reset();
    indexBuffer_.reset();
    materialsBuffer_.reset();
    drawIndirectBuffer_.reset();
    histogramBuffer_.reset();
    exposureBuffer_.reset();
    bluenoiseTex_.reset();
    envMap_hdri_.reset();
    if (irradianceMap_)   { glDeleteTextures(1, &irradianceMap_);   irradianceMap_   = 0; }
    if (envCubemap_)      { glDeleteTextures(1, &envCubemap_);      envCubemap_      = 0; }
    if (irradianceCube_)  { glDeleteTextures(1, &irradianceCube_);  irradianceCube_  = 0; }
    if (prefilterCube_)   { glDeleteTextures(1, &prefilterCube_);   prefilterCube_   = 0; }
    if (brdfLUT_)         { glDeleteTextures(1, &brdfLUT_);         brdfLUT_         = 0; }
    if (iblCubeVAO_)      { glDeleteVertexArrays(1, &iblCubeVAO_);  iblCubeVAO_      = 0; }
    if (iblCubeVBO_)      { glDeleteBuffers(1, &iblCubeVBO_);       iblCubeVBO_      = 0; }
}

Engine::~Engine() {
    Shutdown();
}

void Engine::Resize(int w, int h) {
    if (w == width_ && h == height_) return;
    width_  = w;
    height_ = h;
    destroyFramebuffers_();
    createFramebuffers_();
}

// ---------------------------------------------------------------------------
// createFramebuffers_
// ---------------------------------------------------------------------------

void Engine::createFramebuffers_() {
    GLint swizzleMask[] = { GL_RED, GL_RED, GL_RED, GL_ONE };

    // SSAO
    glCreateTextures(GL_TEXTURE_2D, 1, &ssaoTex_);
    glTextureStorage2D(ssaoTex_, 1, GL_R8, width_, height_);
    glTextureParameteriv(ssaoTex_, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);
    glCreateTextures(GL_TEXTURE_2D, 1, &ssaoBlurred_);
    glTextureStorage2D(ssaoBlurred_, 1, GL_R8, width_, height_);
    glCreateFramebuffers(1, &ssaoFbo_);
    glNamedFramebufferTexture(ssaoFbo_, GL_COLOR_ATTACHMENT0, ssaoTex_, 0);
    glNamedFramebufferDrawBuffer(ssaoFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(ssaoFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create SSAO framebuffer");

    // HDR
    glCreateTextures(GL_TEXTURE_2D, 1, &hdrColorTex_);
    glTextureStorage2D(hdrColorTex_, 1, GL_RGBA16F, width_, height_);
    glCreateTextures(GL_TEXTURE_2D, 1, &hdrDepthTex_);
    glTextureStorage2D(hdrDepthTex_, 1, GL_DEPTH_COMPONENT32F, width_, height_);
    glCreateFramebuffers(1, &hdrFbo_);
    glNamedFramebufferTexture(hdrFbo_, GL_COLOR_ATTACHMENT0, hdrColorTex_, 0);
    glNamedFramebufferTexture(hdrFbo_, GL_DEPTH_ATTACHMENT,  hdrDepthTex_, 0);
    glNamedFramebufferDrawBuffer(hdrFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(hdrFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create HDR framebuffer");

    // SSR (half resolution)
    ssrWidth_  = (GLuint)(width_  / 2);
    ssrHeight_ = (GLuint)(height_ / 2);
    glCreateTextures(GL_TEXTURE_2D, 1, &ssrTex_);
    glTextureStorage2D(ssrTex_, 1, GL_RGBA16F, ssrWidth_, ssrHeight_);
    glTextureParameteri(ssrTex_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(ssrTex_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateTextures(GL_TEXTURE_2D, 1, &ssrTexBlur_);
    glTextureStorage2D(ssrTexBlur_, 1, GL_RGBA16F, ssrWidth_, ssrHeight_);
    glCreateFramebuffers(1, &ssrFbo_);
    glNamedFramebufferTexture(ssrFbo_, GL_COLOR_ATTACHMENT0, ssrTex_, 0);
    glNamedFramebufferDrawBuffer(ssrFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(ssrFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create SSR framebuffer");

    // Volumetrics
    glCreateTextures(GL_TEXTURE_2D, 1, &volumetricsTex_);
    glTextureStorage2D(volumetricsTex_, 1, GL_R16F, width_, height_);
    glTextureParameteri(volumetricsTex_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(volumetricsTex_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(volumetricsTex_, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTextureParameteri(volumetricsTex_, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glCreateFramebuffers(1, &volumetricsFbo_);
    glNamedFramebufferTexture(volumetricsFbo_, GL_COLOR_ATTACHMENT0, volumetricsTex_, 0);
    glNamedFramebufferDrawBuffer(volumetricsFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(volumetricsFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create volumetrics framebuffer");

    glCreateTextures(GL_TEXTURE_2D, 1, &volumetricsTexBlur_);
    glTextureStorage2D(volumetricsTexBlur_, 1, GL_R16F, width_, height_);
    glTextureParameteriv(volumetricsTexBlur_, GL_TEXTURE_SWIZZLE_RGBA, swizzleMask);

    // Volumetrics a-trous
    glCreateTextures(GL_TEXTURE_2D, 1, &volumetricsAtrousTex_);
    glTextureStorage2D(volumetricsAtrousTex_, 1, GL_R16F, width_, height_);
    GLint swizzleMaskVolumetric[] = { GL_RED, GL_RED, GL_RED, GL_RED };
    glTextureParameteriv(volumetricsAtrousTex_, GL_TEXTURE_SWIZZLE_RGBA, swizzleMaskVolumetric);
    glTextureParameteri(volumetricsAtrousTex_, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
    glTextureParameteri(volumetricsAtrousTex_, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
    glCreateFramebuffers(1, &volumetricsAtrousFbo_);
    glNamedFramebufferTexture(volumetricsAtrousFbo_, GL_COLOR_ATTACHMENT0, volumetricsAtrousTex_, 0);
    glNamedFramebufferTexture(volumetricsAtrousFbo_, GL_COLOR_ATTACHMENT1, volumetricsTex_,       0);
    glNamedFramebufferDrawBuffer(volumetricsAtrousFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(volumetricsAtrousFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create a-trous framebuffer");
    glTextureParameteriv(volumetricsTex_, GL_TEXTURE_SWIZZLE_RGBA, swizzleMaskVolumetric);

    // Tonemapping SSBOs
    {
        std::vector<int> zeros(128, 0);
        float expo[] = { 1.0f, 0.0f };
        exposureBuffer_  = std::make_unique<StaticBuffer>(expo, 2 * sizeof(float), 0);
        histogramBuffer_ = std::make_unique<StaticBuffer>(zeros.data(), zeros.size() * sizeof(int), 0);
    }

    // Shadow depth
    const GLfloat txzeros[] = { 0, 0, 0, 0 };
    glCreateTextures(GL_TEXTURE_2D, 1, &shadowDepth_);
    glTextureStorage2D(shadowDepth_, 1, GL_DEPTH_COMPONENT32, shadowWidth_, shadowHeight_);
    glTextureParameterfv(shadowDepth_, GL_TEXTURE_BORDER_COLOR, txzeros);
    glTextureParameteri(shadowDepth_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(shadowDepth_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glCreateFramebuffers(1, &shadowFbo_);
    glNamedFramebufferTexture(shadowFbo_, GL_DEPTH_ATTACHMENT, shadowDepth_, 0);
    glNamedFramebufferDrawBuffer(shadowFbo_, GL_NONE);
    if (glCheckNamedFramebufferStatus(shadowFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create shadow framebuffer");

    // VSM
    glCreateTextures(GL_TEXTURE_2D, 1, &vshadowDepthGoodFormat_);
    glCreateTextures(GL_TEXTURE_2D, 1, &vshadowMomentBlur_);
    glTextureStorage2D(vshadowDepthGoodFormat_, shadowLevels_, GL_RG32F, shadowWidth_, shadowHeight_);
    glTextureStorage2D(vshadowMomentBlur_,      1,             GL_RG32F, shadowWidth_, shadowHeight_);
    glTextureParameterfv(vshadowDepthGoodFormat_, GL_TEXTURE_BORDER_COLOR, txzeros);
    glTextureParameteri(vshadowDepthGoodFormat_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(vshadowDepthGoodFormat_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTextureParameteri(vshadowDepthGoodFormat_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(vshadowDepthGoodFormat_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameterf(vshadowDepthGoodFormat_, GL_TEXTURE_MAX_ANISOTROPY, deviceAnisotropy_);
    glCreateFramebuffers(1, &vshadowGoodFormatFbo_);
    glNamedFramebufferTexture(vshadowGoodFormatFbo_, GL_COLOR_ATTACHMENT0, vshadowDepthGoodFormat_, 0);
    glNamedFramebufferDrawBuffer(vshadowGoodFormatFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(vshadowGoodFormatFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create VSM framebuffer");

    // ESM
    glCreateTextures(GL_TEXTURE_2D, 1, &eExpShadowDepth_);
    glCreateTextures(GL_TEXTURE_2D, 1, &eShadowDepthBlur_);
    glTextureStorage2D(eExpShadowDepth_, shadowLevels_, GL_R32F, shadowWidth_, shadowHeight_);
    glTextureStorage2D(eShadowDepthBlur_, 1,            GL_R32F, shadowWidth_, shadowHeight_);
    glTextureParameterfv(eExpShadowDepth_, GL_TEXTURE_BORDER_COLOR, txzeros);
    glTextureParameteri(eExpShadowDepth_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(eExpShadowDepth_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTextureParameteri(eExpShadowDepth_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(eExpShadowDepth_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameterf(eExpShadowDepth_, GL_TEXTURE_MAX_ANISOTROPY, deviceAnisotropy_);
    glCreateFramebuffers(1, &eShadowFbo_);
    glNamedFramebufferTexture(eShadowFbo_, GL_COLOR_ATTACHMENT0, eExpShadowDepth_, 0);
    glNamedFramebufferDrawBuffer(eShadowFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(eShadowFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create ESM framebuffer");

    // MSM
    glCreateTextures(GL_TEXTURE_2D, 1, &msmShadowMoments_);
    glCreateTextures(GL_TEXTURE_2D, 1, &msmShadowMomentsBlur_);
    glTextureStorage2D(msmShadowMoments_,     shadowLevels_, GL_RGBA32F, shadowWidth_, shadowHeight_);
    glTextureStorage2D(msmShadowMomentsBlur_, 1,             GL_RGBA32F, shadowWidth_, shadowHeight_);
    glTextureParameterfv(msmShadowMoments_, GL_TEXTURE_BORDER_COLOR, txzeros);
    glTextureParameteri(msmShadowMoments_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(msmShadowMoments_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTextureParameteri(msmShadowMoments_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(msmShadowMoments_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameterf(msmShadowMoments_, GL_TEXTURE_MAX_ANISOTROPY, deviceAnisotropy_);
    glCreateFramebuffers(1, &msmShadowFbo_);
    glNamedFramebufferTexture(msmShadowFbo_, GL_COLOR_ATTACHMENT0, msmShadowMoments_, 0);
    glNamedFramebufferDrawBuffer(msmShadowFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(msmShadowFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create MSM framebuffer");

    // G-buffer
    glCreateTextures(GL_TEXTURE_2D, 1, &gAlbedo_);
    glTextureStorage2D(gAlbedo_, 1, GL_RGBA8,          width_, height_);
    glCreateTextures(GL_TEXTURE_2D, 1, &gNormal_);
    glTextureStorage2D(gNormal_, 1, GL_RG8_SNORM,      width_, height_);
    glCreateTextures(GL_TEXTURE_2D, 1, &gRMA_);
    glTextureStorage2D(gRMA_,    1, GL_RGB10_A2,       width_, height_);
    glCreateTextures(GL_TEXTURE_2D, 1, &gDepth_);
    glTextureStorage2D(gDepth_,  1, GL_DEPTH_COMPONENT32F, width_, height_);

    glCreateFramebuffers(1, &gfbo_);
    glNamedFramebufferTexture(gfbo_, GL_COLOR_ATTACHMENT0, gAlbedo_, 0);
    glNamedFramebufferTexture(gfbo_, GL_COLOR_ATTACHMENT1, gNormal_, 0);
    glNamedFramebufferTexture(gfbo_, GL_COLOR_ATTACHMENT2, gRMA_,    0);
    glNamedFramebufferTexture(gfbo_, GL_DEPTH_ATTACHMENT,  gDepth_,  0);
    GLenum buffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glNamedFramebufferDrawBuffers(gfbo_, std::size(buffers), buffers);
    if (glCheckNamedFramebufferStatus(gfbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create G-buffer framebuffer");

    // Postprocess
    glCreateTextures(GL_TEXTURE_2D, 1, &postprocessColor_);
    glTextureStorage2D(postprocessColor_, 1, GL_RGBA16F, width_, height_);
    glCreateTextures(GL_TEXTURE_2D, 1, &postprocessPostSRGB_);
    glTextureStorage2D(postprocessPostSRGB_, 1, GL_RGBA8, width_, height_);
    glTextureParameteri(postprocessPostSRGB_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(postprocessPostSRGB_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateTextures(GL_TEXTURE_2D, 1, &legitFinalImage_);
    glTextureStorage2D(legitFinalImage_, 1, GL_RGBA8, width_, height_);
    glTextureParameteri(legitFinalImage_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(legitFinalImage_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCreateFramebuffers(1, &postprocessFbo_);
    glNamedFramebufferTexture(postprocessFbo_, GL_COLOR_ATTACHMENT0, postprocessColor_, 0);
    glNamedFramebufferTexture(postprocessFbo_, GL_DEPTH_ATTACHMENT,  gDepth_,           0);
    glNamedFramebufferDrawBuffer(postprocessFbo_, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(postprocessFbo_, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Failed to create postprocess framebuffer");
}

// ---------------------------------------------------------------------------
// destroyFramebuffers_
// ---------------------------------------------------------------------------

void Engine::destroyFramebuffers_() {
    if (volumetricsTex_)       { glDeleteTextures(1, &volumetricsTex_);       volumetricsTex_ = 0; }
    if (volumetricsTexBlur_)   { glDeleteTextures(1, &volumetricsTexBlur_);   volumetricsTexBlur_ = 0; }
    if (volumetricsFbo_)       { glDeleteFramebuffers(1, &volumetricsFbo_);   volumetricsFbo_ = 0; }
    if (volumetricsAtrousTex_) { glDeleteTextures(1, &volumetricsAtrousTex_); volumetricsAtrousTex_ = 0; }
    if (volumetricsAtrousFbo_) { glDeleteFramebuffers(1, &volumetricsAtrousFbo_); volumetricsAtrousFbo_ = 0; }

    if (gAlbedo_) { glDeleteTextures(1, &gAlbedo_); gAlbedo_ = 0; }
    if (gNormal_) { glDeleteTextures(1, &gNormal_); gNormal_ = 0; }
    if (gDepth_)  { glDeleteTextures(1, &gDepth_);  gDepth_  = 0; }
    if (gRMA_)    { glDeleteTextures(1, &gRMA_);    gRMA_    = 0; }
    if (gfbo_)    { glDeleteFramebuffers(1, &gfbo_); gfbo_   = 0; }

    if (postprocessColor_)    { glDeleteTextures(1, &postprocessColor_);    postprocessColor_ = 0; }
    if (postprocessPostSRGB_) { glDeleteTextures(1, &postprocessPostSRGB_); postprocessPostSRGB_ = 0; }
    if (legitFinalImage_)     { glDeleteTextures(1, &legitFinalImage_);     legitFinalImage_ = 0; }
    if (postprocessFbo_)      { glDeleteFramebuffers(1, &postprocessFbo_);  postprocessFbo_ = 0; }

    if (ssrTex_)     { glDeleteTextures(1, &ssrTex_);     ssrTex_ = 0; }
    if (ssrTexBlur_) { glDeleteTextures(1, &ssrTexBlur_); ssrTexBlur_ = 0; }
    if (ssrFbo_)     { glDeleteFramebuffers(1, &ssrFbo_); ssrFbo_ = 0; }
    ssrWidth_ = ssrHeight_ = 0;

    if (shadowDepth_) { glDeleteTextures(1, &shadowDepth_); shadowDepth_ = 0; }
    if (shadowFbo_)   { glDeleteFramebuffers(1, &shadowFbo_); shadowFbo_ = 0; }

    if (vshadowDepthGoodFormat_) { glDeleteTextures(1, &vshadowDepthGoodFormat_);   vshadowDepthGoodFormat_ = 0; }
    if (vshadowMomentBlur_)      { glDeleteTextures(1, &vshadowMomentBlur_);        vshadowMomentBlur_ = 0; }
    if (vshadowGoodFormatFbo_)   { glDeleteFramebuffers(1, &vshadowGoodFormatFbo_); vshadowGoodFormatFbo_ = 0; }

    if (eExpShadowDepth_)  { glDeleteTextures(1, &eExpShadowDepth_);  eExpShadowDepth_  = 0; }
    if (eShadowDepthBlur_) { glDeleteTextures(1, &eShadowDepthBlur_); eShadowDepthBlur_ = 0; }
    if (eShadowFbo_)       { glDeleteFramebuffers(1, &eShadowFbo_);   eShadowFbo_ = 0; }

    if (msmShadowMoments_)     { glDeleteTextures(1, &msmShadowMoments_);         msmShadowMoments_ = 0; }
    if (msmShadowMomentsBlur_) { glDeleteTextures(1, &msmShadowMomentsBlur_);     msmShadowMomentsBlur_ = 0; }
    if (msmShadowFbo_)         { glDeleteFramebuffers(1, &msmShadowFbo_);         msmShadowFbo_ = 0; }

    if (hdrColorTex_) { glDeleteTextures(1, &hdrColorTex_); hdrColorTex_ = 0; }
    if (hdrDepthTex_) { glDeleteTextures(1, &hdrDepthTex_); hdrDepthTex_ = 0; }
    if (hdrFbo_)      { glDeleteFramebuffers(1, &hdrFbo_);  hdrFbo_ = 0; }

    if (ssaoFbo_)     { glDeleteFramebuffers(1, &ssaoFbo_); ssaoFbo_ = 0; }
    if (ssaoTex_)     { glDeleteTextures(1, &ssaoTex_);     ssaoTex_ = 0; }
    if (ssaoBlurred_) { glDeleteTextures(1, &ssaoBlurred_); ssaoBlurred_ = 0; }

    if (exposureBuffer_)  { exposureBuffer_.reset(); }
    if (histogramBuffer_) { histogramBuffer_.reset(); }
}

// ---------------------------------------------------------------------------
// createVAO_
// ---------------------------------------------------------------------------

void Engine::createVAO_() {
    glCreateVertexArrays(1, &vao_);
    glEnableVertexArrayAttrib(vao_, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glEnableVertexArrayAttrib(vao_, 2);
    glEnableVertexArrayAttrib(vao_, 3);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribFormat(vao_, 3, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, tangent));
    glVertexArrayAttribBinding(vao_, 0, 0);
    glVertexArrayAttribBinding(vao_, 1, 0);
    glVertexArrayAttribBinding(vao_, 2, 0);
    glVertexArrayAttribBinding(vao_, 3, 0);
}

// ---------------------------------------------------------------------------
// LoadEnvironment
// ---------------------------------------------------------------------------

// LoadEnvironment — IBL cubemap suite based on tristancalderbank/OpenGL-PBR-Renderer.
// Generates: envCubemap_ (radiance), irradianceCube_ (diffuse), prefilterCube_
// (specular w/ roughness mips), and brdfLUT_ (split-sum BRDF integration).
void Engine::LoadEnvironment(const std::string& hdr_path) {
    std::fprintf(stderr, "[ibl] LoadEnvironment '%s' start\n", hdr_path.c_str());
    // 1) Load equirectangular HDRI as a 2D texture (still useful for raw skybox fallbacks).
    TextureCreateInfo info{
        .path         = hdr_path,
        .sRGB         = false,
        .generateMips = false,
        .HDR          = true,
        .minFilter    = GL_LINEAR,
        .magFilter    = GL_LINEAR,
    };
    envMap_hdri_ = std::make_unique<Texture2D>(info);

    // 2) Set up the unit cube VAO once.
    if (!iblCubeVAO_) {
        glCreateVertexArrays(1, &iblCubeVAO_);
        glCreateBuffers(1, &iblCubeVBO_);
        glNamedBufferStorage(iblCubeVBO_, sizeof(kCubeVerts), kCubeVerts, 0);
        glVertexArrayVertexBuffer(iblCubeVAO_, 0, iblCubeVBO_, 0, 3 * sizeof(float));
        glEnableVertexArrayAttrib(iblCubeVAO_, 0);
        glVertexArrayAttribFormat(iblCubeVAO_, 0, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexArrayAttribBinding(iblCubeVAO_, 0, 0);
    }

    // 3) Allocate destination cubemaps.
    if (envCubemap_)     { glDeleteTextures(1, &envCubemap_);     envCubemap_     = 0; }
    if (irradianceCube_) { glDeleteTextures(1, &irradianceCube_); irradianceCube_ = 0; }
    if (prefilterCube_)  { glDeleteTextures(1, &prefilterCube_);  prefilterCube_  = 0; }
    if (brdfLUT_)        { glDeleteTextures(1, &brdfLUT_);        brdfLUT_        = 0; }

    auto allocCubemap = [](GLuint& tex, int size, int mips) {
        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &tex);
        glTextureStorage2D(tex, mips, GL_RGB16F, size, size);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER,
            mips > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    };

    const int envMips = 1 + (int)glm::floor(glm::log2((float)envCubemapSize_));
    allocCubemap(envCubemap_,     envCubemapSize_,     envMips);
    allocCubemap(irradianceCube_, irradianceCubeSize_, 1);
    allocCubemap(prefilterCube_,  prefilterCubeSize_,  prefilterMipLevels_);

    glCreateTextures(GL_TEXTURE_2D, 1, &brdfLUT_);
    glTextureStorage2D(brdfLUT_, 1, GL_RG16F, brdfLUTSize_, brdfLUTSize_);
    glTextureParameteri(brdfLUT_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(brdfLUT_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(brdfLUT_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(brdfLUT_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 4) Cube views (look down +X/-X/+Y/-Y/+Z/-Z), 90° perspective.
    const glm::mat4 captureProj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    const glm::vec3 origin(0.0f);
    const glm::mat4 captureViews[6] = {
        glm::lookAt(origin, glm::vec3( 1, 0, 0), glm::vec3(0,-1, 0)),
        glm::lookAt(origin, glm::vec3(-1, 0, 0), glm::vec3(0,-1, 0)),
        glm::lookAt(origin, glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1)),
        glm::lookAt(origin, glm::vec3( 0,-1, 0), glm::vec3(0, 0,-1)),
        glm::lookAt(origin, glm::vec3( 0, 0, 1), glm::vec3(0,-1, 0)),
        glm::lookAt(origin, glm::vec3( 0, 0,-1), glm::vec3(0,-1, 0)),
    };

    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    GLuint capFbo = 0;
    glCreateFramebuffers(1, &capFbo);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // 4a) Equirectangular HDRI -> envCubemap_
    {
        auto& sh = Shader::shaders["equirect_to_cube"];
        sh->Bind();
        sh->SetMat4("u_proj", captureProj);
        sh->SetInt("u_equirect", 0);
        glBindTextureUnit(0, envMap_hdri_->GetID());
        glViewport(0, 0, envCubemapSize_, envCubemapSize_);
        glBindFramebuffer(GL_FRAMEBUFFER, capFbo);
        for (int face = 0; face < 6; ++face) {
            glNamedFramebufferTextureLayer(capFbo, GL_COLOR_ATTACHMENT0,
                                           envCubemap_, 0, face);
            sh->SetMat4("u_view", captureViews[face]);
            RenderUnitCube(iblCubeVAO_);
        }
        glGenerateTextureMipmap(envCubemap_);
    }

    // 4b) envCubemap_ -> irradianceCube_
    {
        auto& sh = Shader::shaders["diffuse_irradiance"];
        sh->Bind();
        sh->SetMat4("u_proj", captureProj);
        sh->SetInt("u_envCube", 0);
        glBindTextureUnit(0, envCubemap_);
        glViewport(0, 0, irradianceCubeSize_, irradianceCubeSize_);
        for (int face = 0; face < 6; ++face) {
            glNamedFramebufferTextureLayer(capFbo, GL_COLOR_ATTACHMENT0,
                                           irradianceCube_, 0, face);
            sh->SetMat4("u_view", captureViews[face]);
            RenderUnitCube(iblCubeVAO_);
        }
    }

    // 4c) envCubemap_ -> prefilterCube_ (one set of 6 faces per mip).
    {
        auto& sh = Shader::shaders["prefilter_env"];
        sh->Bind();
        sh->SetMat4("u_proj", captureProj);
        sh->SetInt("u_envCube", 0);
        glBindTextureUnit(0, envCubemap_);
        for (int mip = 0; mip < prefilterMipLevels_; ++mip) {
            const int mipSize = (int)(prefilterCubeSize_ * glm::pow(0.5f, (float)mip));
            glViewport(0, 0, mipSize, mipSize);
            const float roughness = (float)mip / (float)(prefilterMipLevels_ - 1);
            sh->SetFloat("u_roughness", roughness);
            for (int face = 0; face < 6; ++face) {
                glNamedFramebufferTextureLayer(capFbo, GL_COLOR_ATTACHMENT0,
                                               prefilterCube_, mip, face);
                sh->SetMat4("u_view", captureViews[face]);
                RenderUnitCube(iblCubeVAO_);
            }
        }
    }

    // 4d) BRDF integration LUT (fullscreen tri reads gl_FragCoord -> uv).
    {
        auto& sh = Shader::shaders["brdf_lut"];
        sh->Bind();
        sh->SetVec2("u_resolution", glm::vec2((float)brdfLUTSize_, (float)brdfLUTSize_));
        glViewport(0, 0, brdfLUTSize_, brdfLUTSize_);
        glNamedFramebufferTexture(capFbo, GL_COLOR_ATTACHMENT0, brdfLUT_, 0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &capFbo);

    // Restore reasonable defaults. The pipeline expects vao_ bound and
    // depth-test/cull-face configured per-pass; we only undid them temporarily.
    glBindVertexArray(vao_);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    std::fprintf(stderr, "[ibl] envCube=%u irrCube=%u prefilter=%u brdfLUT=%u\n",
                 envCubemap_, irradianceCube_, prefilterCube_, brdfLUT_);
}

// ---------------------------------------------------------------------------
// Static scene API
// ---------------------------------------------------------------------------

void Engine::BeginStaticScene() {
    vertexBuffer_->Clear();
    indexBuffer_->Clear();
    staticMeshes_.clear();
    static_scene_open_ = true;
}

StaticMeshHandle Engine::UploadStaticMesh(const std::vector<Vertex>& vertices,
                                          const std::vector<uint32_t>& indices,
                                          int material_index) {
    assert(static_scene_open_);
    StaticMeshRecord rec;
    rec.vtx_alloc      = vertexBuffer_->Allocate(vertices.data(), vertices.size() * sizeof(Vertex));
    rec.idx_alloc      = indexBuffer_->Allocate(indices.data(), indices.size() * sizeof(uint32_t));
    rec.material_index = material_index;
    StaticMeshHandle h{ staticMeshes_.size() };
    staticMeshes_.push_back(rec);
    return h;
}

void Engine::RebuildMaterialsBuffer() {
    auto gpuMats = material_manager_.GetLinearBindless();
    if (gpuMats.empty()) {
        BindlessMaterial zero{};
        materialsBuffer_ = std::make_unique<StaticBuffer>(
            &zero, sizeof(BindlessMaterial), 0);
        return;
    }
    materialsBuffer_ = std::make_unique<StaticBuffer>(
        gpuMats.data(), gpuMats.size() * sizeof(BindlessMaterial), 0);
}

void Engine::EndStaticScene() {
    assert(static_scene_open_);
    static_scene_open_ = false;

    RebuildMaterialsBuffer();

    std::vector<DrawElementsIndirectCommand> cmds;
    cmds.reserve(staticMeshes_.size());
    GLuint baseInstance = 0;
    for (const auto& rec : staticMeshes_) {
        const auto& vi = vertexBuffer_->GetAlloc(rec.vtx_alloc);
        const auto& ii = indexBuffer_->GetAlloc(rec.idx_alloc);
        cmds.push_back({
            .count         = static_cast<GLuint>(ii.size / sizeof(uint32_t)),
            .instanceCount = 1,
            .firstIndex    = static_cast<GLuint>(ii.offset / sizeof(uint32_t)),
            .baseVertex    = static_cast<GLuint>(vi.offset / sizeof(Vertex)),
            .baseInstance  = baseInstance++,
        });
    }
    drawIndirectBuffer_ = std::make_unique<StaticBuffer>(
        cmds.data(), cmds.size() * sizeof(DrawElementsIndirectCommand), 0);
}

} // namespace rco::renderer
