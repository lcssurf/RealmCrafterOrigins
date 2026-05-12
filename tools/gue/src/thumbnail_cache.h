#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <deque>
#include <queue>
#include <string>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

struct GLFWwindow;

namespace gue {

// Renders 80×80 JPEG thumbnails for media models on a background thread that
// owns a shared OpenGL context. The main thread never blocks — it just checks
// a result queue each frame and uploads completed textures.
//
// Unreal-style: thumbnails are baked to "thumbcache/<id>_<mtime>.jpg" next to
// rco_gue.exe. On second open they load from disk instantly (no render needed).
class ThumbnailCache {
public:
    static constexpr int kSize = 80;
    static constexpr int kJpgQ = 82;

    // Must be called from the main thread before the first Get().
    // mainWin is the GLFW window whose GL context will be shared.
    void Init(GLFWwindow* mainWin);

    ~ThumbnailCache() { Destroy(); }

    // Returns the GL texture for model_id, or 0 if not yet ready.
    // Enqueues a render job if no valid cache file exists.
    // Safe to call every frame — no allocation after first call per model.
    GLuint Get(int model_id, const std::string& file_path_rel);

    // Check the result queue and promote completed thumbnails.
    // Call once per frame from the main (GL) thread.
    void Tick();

    void Destroy();

private:
    struct Job   { int id; std::string rel; std::string abs; };
    struct Ready { int id; GLuint tex; };

    // Worker thread entry point — owns the shared GL context.
    void WorkerLoop_();

    // Cache dir helpers
    static std::filesystem::path CacheDir();
    std::string CachePath(int id, std::filesystem::file_time_type mtime) const;
    GLuint LoadFromDisk(const std::string& path);

    // ── Shared state (protected by mutex_) ──────────────────────────────
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::deque<Job>         jobQueue_;
    std::queue<Ready>       readyQueue_;
    std::atomic<bool>       shutdown_{ false };

    // ── Main-thread state ───────────────────────────────────────────────
    std::unordered_map<int, GLuint> cache_;   // model_id → tex (0 = pending)
    std::filesystem::path           cacheDir_;
    GLFWwindow*                     mainWin_ = nullptr;

    // ── Background thread ───────────────────────────────────────────────
    std::thread  worker_;
    bool         workerStarted_ = false;
    GLFWwindow*  sharedCtx_     = nullptr;  // created on main thread, used on worker
};

} // namespace gue
