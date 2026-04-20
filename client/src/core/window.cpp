#include "window.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace rco {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Window::Window(int width, int height, const std::string& title)
    : width_(width), height_(height)
{
    if (!glfwInit()) {
        throw std::runtime_error("glfwInit failed");
    }

    // OpenGL 4.6 Core profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_FALSE);

    // Enable the debug context in debug builds so we get GL_DEBUG_OUTPUT.
#ifdef _DEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#else
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_FALSE);
#endif

    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 0);

    window_ = glfwCreateWindow(width_, height_, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // vsync

    // Store a pointer to this object for use in callbacks.
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, FramebufferSizeCallback);

    // Load OpenGL function pointers via GLAD.
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        glfwDestroyWindow(window_);
        glfwTerminate();
        throw std::runtime_error("gladLoadGLLoader failed");
    }

    // Enable GL debug output in debug builds.
#ifdef _DEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(GLDebugCallback, nullptr);
    // Suppress non-critical notifications to reduce noise.
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
                          GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
#endif

    // Sky-blue clear colour — matches a clear day.
    glClearColor(0.53f, 0.81f, 0.98f, 1.0f);

    // Sensible defaults.
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Initial viewport
    int fbW, fbH;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    glViewport(0, 0, fbW, fbH);

    std::printf("[window] OpenGL %s on %s\n",
                reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

// ---------------------------------------------------------------------------
// Per-frame operations
// ---------------------------------------------------------------------------

bool Window::ShouldClose() const {
    return glfwWindowShouldClose(window_) != 0;
}

void Window::BeginFrame() {
    glfwPollEvents();

    // Sync our cached dimensions in case the window was resized.
    glfwGetFramebufferSize(window_, &width_, &height_);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Window::EndFrame() {
    glfwSwapBuffers(window_);
}

void Window::SetTitle(const std::string& title) {
    glfwSetWindowTitle(window_, title.c_str());
}

// ---------------------------------------------------------------------------
// Static callbacks
// ---------------------------------------------------------------------------

void Window::FramebufferSizeCallback(GLFWwindow* window, int w, int h) {
    (void)window;
    glViewport(0, 0, w, h);
}

void Window::GLDebugCallback(GLenum source, GLenum type, GLuint id,
                              GLenum severity, GLsizei length,
                              const GLchar* message, const void* /*userParam*/)
{
    (void)source; (void)type; (void)id; (void)severity; (void)length;
    std::fprintf(stderr, "[GL] %s\n", message);
}

} // namespace rco
