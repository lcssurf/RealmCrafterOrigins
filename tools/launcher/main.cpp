#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <unistd.h>
  #include <limits.h>
#endif

namespace fs = std::filesystem;

static void SetCwdToExeDir() {
    fs::path exe;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return;
    exe = buf;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    exe = buf;
#endif
    std::error_code ec;
    fs::current_path(exe.parent_path(), ec);
}

static void GlfwErrorCb(int, const char* desc) {
    std::fprintf(stderr, "[glfw] %s\n", desc);
}

struct Project {
    std::string name;
    bool        has_db;  // any .db in server/ → server ran at least once
};

static std::vector<Project> ScanProjects(const fs::path& dir) {
    std::vector<Project> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory()) continue;
        Project p;
        p.name   = e.path().filename().string();
        p.has_db = false;
        fs::path sdir = e.path() / "server";
        if (fs::is_directory(sdir, ec))
            for (auto& f : fs::directory_iterator(sdir, ec))
                if (f.path().extension() == ".db") { p.has_db = true; break; }
        out.push_back(std::move(p));
    }
    return out;
}

int main() {
    SetCwdToExeDir();
    // cwd = dist/tools/ (anchored to exe dir)
    // Template:      ".."            → dist/
    // Projects root: "../../projects" → sibling of dist/ at repo root
    const fs::path kTemplateDir = "..";
    const fs::path kProjectsDir = "../../projects";

    {
        std::error_code ec;
        fs::create_directories(kProjectsDir, ec);
    }

    glfwSetErrorCallback(GlfwErrorCb);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* win = glfwCreateWindow(900, 520,
        "RCO Project Launcher", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::fprintf(stderr, "[glad] failed to load OpenGL\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 460");

    auto projects = ScanProjects(kProjectsDir);
    int  sel      = -1;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fb_w, fb_h;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        float fW = (float)fb_w;
        float fH = (float)fb_h;

        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({fW, fH});
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoDecoration    |
            ImGuiWindowFlags_NoMove          |
            ImGuiWindowFlags_NoResize        |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("RCO Project Launcher");
        {
            std::error_code ec;
            ImGui::TextDisabled("%s", fs::absolute(kProjectsDir, ec).string().c_str());
        }
        ImGui::Separator();

        float list_w = fW * 0.38f;

        ImGui::BeginChild("##list", {list_w, fH - 110.f}, true);
        for (int i = 0; i < (int)projects.size(); ++i) {
            const auto& p = projects[i];
            char lbl[256];
            std::snprintf(lbl, sizeof(lbl), "%s%s##%d",
                p.name.c_str(), p.has_db ? "" : "  [new]", i);
            if (ImGui::Selectable(lbl, sel == i))
                sel = i;
        }
        if (projects.empty())
            ImGui::TextDisabled("No projects yet. Click 'New Project'.");
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##detail", {fW - list_w - 24.f, fH - 110.f}, false);
        if (sel >= 0 && sel < (int)projects.size()) {
            const auto& p = projects[sel];
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TextDisabled(p.has_db
                ? "Database: exists (server ran before)"
                : "Database: none (created on first start)");
            ImGui::Separator();
            ImGui::TextDisabled("(start/editor/client — coming soon)");
        } else {
            ImGui::TextDisabled("Select a project to see actions.");
        }
        ImGui::EndChild();

        if (ImGui::Button("New Project")) { /* TODO commit 2 */ }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            projects = ScanProjects(kProjectsDir);
            sel = -1;
        }

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
