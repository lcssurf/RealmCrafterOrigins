#include "thumbnail_cache.h"
#include "asset_path.h"

// GLFW must be included after glad (glad.h is pulled in via thumbnail_cache.h)
#include <GLFW/glfw3.h>

#include <rco/renderer/model.h>
#include <rco/renderer/model_cache.h>
#include <rco/renderer/shader.h>

#include <stb_image.h>
#include <stb_image_write.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

namespace gue {

// ---------------------------------------------------------------------------
// Cache directory helpers
// ---------------------------------------------------------------------------

static bool ReadMtimeNs(const std::string& absPath, long long* outNs) {
    if (!outNs) return false;
    *outNs = 0;
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(absPath, ec);
    if (ec) return false;
    *outNs = (long long)mtime.time_since_epoch().count();
    return true;
}

std::filesystem::path ThumbnailCache::CacheDir() {
    std::filesystem::path dir = "thumbcache";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Remove legacy PNG thumbnails.
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".png")
            std::filesystem::remove(e.path(), ec);
    return dir;
}

std::string ThumbnailCache::CachePath(
        int id, std::filesystem::file_time_type mtime) const {
    // Bump when thumbnail rendering changes so old cached previews are rebuilt.
    static constexpr int kThumbBakeVersion = 2;
    auto ns = mtime.time_since_epoch().count();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%lld_%d_v%d.jpg",
                  (long long)ns, id, kThumbBakeVersion);
    return (cacheDir_ / buf).string();
}

GLuint ThumbnailCache::LoadFromDisk(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 3);
    if (!px) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(px);
    return tex;
}

// ---------------------------------------------------------------------------
// Init / Destroy
// ---------------------------------------------------------------------------

void ThumbnailCache::Init(GLFWwindow* mainWin) {
    if (workerStarted_) return;
    mainWin_  = mainWin;
    cacheDir_ = CacheDir();

    // Create the shared context on the MAIN thread — glfwCreateWindow must
    // never be called from a background thread on Windows (WGL restriction).
    // The worker thread will call glfwMakeContextCurrent(sharedCtx_).
    glfwWindowHint(GLFW_VISIBLE,                GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,  4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,  6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    sharedCtx_ = glfwCreateWindow(1, 1, "", nullptr, mainWin);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    if (!sharedCtx_) {
        std::fprintf(stderr, "[thumb] failed to create shared GL context\n");
        return;
    }

    workerStarted_ = true;
    worker_ = std::thread(&ThumbnailCache::WorkerLoop_, this);
}

void ThumbnailCache::Destroy() {
    shutdown_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    // Destroy the shared context on the main thread after the worker exits.
    if (sharedCtx_) { glfwDestroyWindow(sharedCtx_); sharedCtx_ = nullptr; }
    for (auto& [id, tex] : cache_)
        if (tex) glDeleteTextures(1, &tex);
    cache_.clear();
    cacheRelPath_.clear();
    cacheMtimeNs_.clear();
    cacheHasMtime_.clear();
    cacheGen_.clear();
}

// ---------------------------------------------------------------------------
// Get — main thread, called every frame
// ---------------------------------------------------------------------------

GLuint ThumbnailCache::Get(int model_id, const std::string& file_path_rel) {
    if (!workerStarted_) return 0;

    std::string abs = ResolveClientAsset(file_path_rel);
    long long mtimeNs = 0;
    const bool hasMtime = ReadMtimeNs(abs, &mtimeNs);

    auto it = cache_.find(model_id);
    if (it != cache_.end()) {
        bool stale = false;
        auto itRel = cacheRelPath_.find(model_id);
        auto itHasMtime = cacheHasMtime_.find(model_id);
        auto itMtime = cacheMtimeNs_.find(model_id);
        stale = stale || (itRel == cacheRelPath_.end()) || (itRel->second != file_path_rel);
        stale = stale || (itHasMtime == cacheHasMtime_.end()) || (itHasMtime->second != hasMtime);
        stale = stale || (itMtime == cacheMtimeNs_.end()) || (itMtime->second != mtimeNs);
        if (!stale) return it->second;

        if (it->second) glDeleteTextures(1, &it->second);
        cache_.erase(it);
        cacheRelPath_.erase(model_id);
        cacheMtimeNs_.erase(model_id);
        cacheHasMtime_.erase(model_id);
        cacheGen_.erase(model_id);
    }

    const uint64_t gen = nextGen_++;
    cache_[model_id] = 0;  // mark pending
    cacheRelPath_[model_id] = file_path_rel;
    cacheMtimeNs_[model_id] = mtimeNs;
    cacheHasMtime_[model_id] = hasMtime;
    cacheGen_[model_id] = gen;

    // Check disk cache first — skip the queue entirely if we have a valid jpg.
    std::error_code ec;
    if (hasMtime) {
        auto mtime = std::filesystem::last_write_time(abs, ec);
        if (!ec) {
            std::string jpg = CachePath(model_id, mtime);
            if (std::filesystem::exists(jpg, ec)) {
                GLuint tex = LoadFromDisk(jpg);
                if (tex) { cache_[model_id] = tex; return tex; }
            }
        }
    }

    // Enqueue for background rendering.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // Avoid duplicates.
        for (auto& j : jobQueue_) if (j.id == model_id && j.gen == gen) return 0;
        jobQueue_.push_back({model_id, gen, file_path_rel, abs});
    }
    cv_.notify_one();
    return 0;
}

// ---------------------------------------------------------------------------
// Tick — main thread, promotes completed textures
// ---------------------------------------------------------------------------

void ThumbnailCache::Tick() {
    std::lock_guard<std::mutex> lk(mutex_);
    while (!readyQueue_.empty()) {
        auto r = readyQueue_.front();
        readyQueue_.pop();
        auto itGen = cacheGen_.find(r.id);
        if (itGen == cacheGen_.end() || itGen->second != r.gen) {
            if (r.tex) glDeleteTextures(1, &r.tex);
            continue;
        }
        cache_[r.id] = r.tex;
    }
}

// ---------------------------------------------------------------------------
// WorkerLoop_ — background thread, owns shared GL context
// ---------------------------------------------------------------------------

void ThumbnailCache::WorkerLoop_() {
    if (!sharedCtx_) return;

    // Take ownership of the shared context on this thread.
    // The context was created on the main thread (glfwCreateWindow) but
    // current-context is per-thread, so we make it current here.
    glfwMakeContextCurrent(sharedCtx_);
    // glad function pointers are process-global — already loaded by main thread.

    // Build a tiny FBO for rendering.
    GLuint fbo = 0, depth = 0;
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kSize, kSize);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth);

    // Minimal forward shader — identical to preview_static but compiled here
    // so the worker thread doesn't depend on the main shader registry.
    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };

    const char* kVS = R"(
#version 460 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
uniform mat4 u_vp;
out vec3 vN; out vec2 vUV;
void main() { vN=aNormal; vUV=aUV; gl_Position=u_vp*vec4(aPos,1.0); }
)";
    const char* kFS = R"(
#version 460 core
in vec3 vN; in vec2 vUV;
out vec4 fragColor;
uniform sampler2D u_albedo;
uniform bool      u_hasAlbedo;
uniform vec3      u_albedoFactor;
uniform vec3      u_sunDir;
uniform vec3      u_sunColor;
uniform float     u_ambient;
void main() {
    vec3 base = u_hasAlbedo ? texture(u_albedo,vUV).rgb : u_albedoFactor;
    float ndl = max(dot(normalize(vN),-u_sunDir),0.0);
    vec3 col  = base*(u_ambient + ndl*u_sunColor);
    col = pow(clamp(col,0.0,1.0), vec3(1.0/2.2));
    fragColor = vec4(col,1.0);
}
)";

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);

    GLint locVP      = glGetUniformLocation(prog, "u_vp");
    GLint locAlbedo  = glGetUniformLocation(prog, "u_albedo");
    GLint locHasAlb  = glGetUniformLocation(prog, "u_hasAlbedo");
    GLint locFactor  = glGetUniformLocation(prog, "u_albedoFactor");
    GLint locSunDir  = glGetUniformLocation(prog, "u_sunDir");
    GLint locSunCol  = glGetUniformLocation(prog, "u_sunColor");
    GLint locAmb     = glGetUniformLocation(prog, "u_ambient");

    while (!shutdown_) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&]{ return shutdown_ || !jobQueue_.empty(); });
            if (shutdown_) break;
            job = jobQueue_.front();
            jobQueue_.pop_front();
        }

        // Check disk cache (mtime may have changed).
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(job.abs, ec);
        if (!ec) {
            std::string jpg = CachePath(job.id, mtime);
            if (std::filesystem::exists(jpg, ec)) {
                // Load from disk on the shared context — texture is visible
                // to the main context because objects are shared.
                GLuint tex = 0;
                stbi_set_flip_vertically_on_load(false);
                int w=0,h=0,ch=0;
                auto* px = stbi_load(jpg.c_str(), &w, &h, &ch, 3);
                if (px) {
                    glGenTextures(1, &tex);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,w,h,0,
                                 GL_RGB,GL_UNSIGNED_BYTE,px);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                    stbi_image_free(px);
                    glFlush();  // ensure texture is visible to main context
                    std::lock_guard<std::mutex> lk(mutex_);
                    readyQueue_.push({job.id, job.gen, tex});
                }
                continue;
            }
        }

        // Load the model (Assimp — potentially slow, but on background thread).
        auto mdl = rco::renderer::ModelCacheGet(job.abs, nullptr);
        if (!mdl || !mdl->IsLoaded()) continue;

        // Create colour texture for this thumbnail.
        GLuint colTex = 0;
        glGenTextures(1, &colTex);
        glBindTexture(GL_TEXTURE_2D, colTex);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,kSize,kSize,0,
                     GL_RGB,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,colTex,0);

        // Render.
        glViewport(0,0,kSize,kSize);
        glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glClearColor(0.12f,0.12f,0.14f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        glm::vec3 bmin=mdl->BoundsMin(), bmax=mdl->BoundsMax();
        if (bmin.x>bmax.x){bmin=glm::vec3(-0.5f);bmax=glm::vec3(0.5f,1.f,0.5f);}
        glm::vec3 ctr=(bmin+bmax)*0.5f;
        float rad=std::max(glm::length(bmax-bmin)*0.5f,0.01f);
        float dist=(rad/0.454f)*1.2f;
        // Use the opposite orbit side so asset thumbnails face the expected front.
        float yaw=glm::radians(140.f), pitch=glm::radians(22.f);
        glm::vec3 eye=ctr+glm::vec3(
            dist*std::cos(pitch)*std::sin(yaw),
            dist*std::sin(pitch),
            dist*std::cos(pitch)*std::cos(yaw));
        glm::mat4 vp=glm::perspective(glm::radians(55.f),1.f,rad*.01f,rad*20.f)
                    *glm::lookAt(eye,ctr,{0,1,0});

        glUseProgram(prog);
        glUniformMatrix4fv(locVP,1,GL_FALSE,glm::value_ptr(vp));
        glUniform3f(locSunDir,
            glm::normalize(glm::vec3(-0.4f,-1.f,-0.3f)).x,
            glm::normalize(glm::vec3(-0.4f,-1.f,-0.3f)).y,
            glm::normalize(glm::vec3(-0.4f,-1.f,-0.3f)).z);
        glUniform3f(locSunCol,1.f,0.95f,0.8f);
        glUniform1f(locAmb,0.30f);

        for (const auto& m : mdl->meshes()) {
            if (!m.vao || m.idx_count==0) continue;
            if (m.tex_albedo) {
                glBindTextureUnit(0,m.tex_albedo);
                glUniform1i(locHasAlb,1); glUniform1i(locAlbedo,0);
            } else {
                glUniform1i(locHasAlb,0);
                glUniform3f(locFactor,m.albedo_factor.x,
                            m.albedo_factor.y,m.albedo_factor.z);
            }
            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES,m.idx_count,GL_UNSIGNED_INT,nullptr);
        }
        glBindVertexArray(0);

        // Read pixels + save JPEG.
        std::vector<unsigned char> px(kSize*kSize*3);
        glReadPixels(0,0,kSize,kSize,GL_RGB,GL_UNSIGNED_BYTE,px.data());
        // Flip Y.
        for (int r=0;r<kSize/2;++r) {
            auto* a=px.data()+r*kSize*3;
            auto* b=px.data()+(kSize-1-r)*kSize*3;
            for (int x=0;x<kSize*3;++x) std::swap(a[x],b[x]);
        }
        if (!ec) {
            std::string jpg = CachePath(job.id, mtime);
            stbi_write_jpg(jpg.c_str(),kSize,kSize,3,px.data(),kJpgQ);
        }

        // The colTex is already in the shared context — main thread can use it
        // directly. Flush so the texture upload is visible across contexts.
        glFlush();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            readyQueue_.push({job.id, job.gen, colTex});
        }
    }

    glDeleteProgram(prog);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth);
    glfwMakeContextCurrent(nullptr);  // release context before thread exits
    // sharedCtx_ is destroyed by the main thread in Destroy() after join.
}

} // namespace gue
