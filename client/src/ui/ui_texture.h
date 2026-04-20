#pragma once
#include <glad/glad.h>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace rco::ui {

// Loads PNG/BMP files into OpenGL textures for use with ImGui::Image().
// Textures are cached by path and never freed (lifetime = application).
class UITextureCache {
public:
    // Returns an ImTextureID for the given file path, or 0 on failure.
    ImTextureID Load(const std::string& path) {
        auto it = cache_.find(path);
        if (it != cache_.end()) return it->second;

        GLuint tex = loadGL(path);
        ImTextureID id = (ImTextureID)(uintptr_t)tex;
        cache_[path] = id;
        return id;
    }

    // Convenience: load relative to assets/ui/
    ImTextureID GUI(const std::string& filename) {
        return Load("assets/ui/gui/" + filename);
    }
    ImTextureID Menu(const std::string& filename) {
        return Load("assets/ui/menu/" + filename);
    }
    ImTextureID Root(const std::string& filename) {
        return Load("assets/ui/" + filename);
    }

private:
    std::unordered_map<std::string, ImTextureID> cache_;

    static GLuint loadGL(const std::string& path);
};

// Global singleton — initialised in main.cpp.
extern UITextureCache g_tex;

} // namespace rco::ui
