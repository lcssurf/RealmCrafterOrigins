#pragma once

#include <string>
#include <functional>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace rco {

// ---------------------------------------------------------------------------
// Window — wraps GLFW window creation, OpenGL context initialisation and the
// per-frame begin/end pair.
// ---------------------------------------------------------------------------
class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    // Non-copyable, non-movable
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    // Returns true when the user has requested the window to close.
    bool ShouldClose() const;

    // Call at the start of each frame: poll GLFW events and clear the back-buffer.
    void BeginFrame();

    // Call at the end of each frame: swap front/back buffers.
    void EndFrame();

    void SetTitle(const std::string& title);

    int          Width()  const { return width_;  }
    int          Height() const { return height_; }
    GLFWwindow*  Handle() const { return window_; }

private:
    GLFWwindow* window_ = nullptr;
    int         width_;
    int         height_;

    static void FramebufferSizeCallback(GLFWwindow* window, int w, int h);
    static void GLDebugCallback(GLenum source, GLenum type, GLuint id,
                                GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam);
};

} // namespace rco
