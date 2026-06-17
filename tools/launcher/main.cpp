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
  #include <shellapi.h>
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

static bool ShouldSkip(const fs::path& rel) {
    for (auto& comp : rel) {
        std::string s = comp.string();
        if (s.empty() || s == ".") continue;
        std::string ext = comp.extension().string();
        if (ext == ".db")               return true;
        if (s.rfind("rco.db", 0) == 0) return true;
        if (ext == ".log")              return true;
        if (ext == ".py")               return true;
        if (s == "thumbcache")          return true;
        if (s == "imgui.ini")           return true;
    }
    return false;
}

static std::string CopyTemplate(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return "Cannot create destination: " + ec.message();

    for (auto& entry : fs::recursive_directory_iterator(src,
            fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        fs::path rel = fs::relative(entry.path(), src, ec);
        if (ec || ShouldSkip(rel)) continue;
        fs::path target = dst / rel;
        if (entry.is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (entry.is_regular_file(ec)) {
            fs::copy_file(entry.path(), target,
                fs::copy_options::overwrite_existing, ec);
            if (ec)
                return "Copy failed for " + rel.string() + ": " + ec.message();
        }
    }
    return "";
}

// Opens dir in Windows Explorer. No-op on non-Windows.
static void OpenInExplorer(const fs::path& dir) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"explore",
                  fs::absolute(dir).wstring().c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
#endif
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
        "RCO Project Manager", nullptr, nullptr);
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

    // New Project dialog state
    bool show_new_dlg = false;
    char new_name[128]{};
    std::string new_err;

    // Rename dialog state
    bool show_rename_dlg = false;
    char rename_buf[128]{};
    std::string rename_err;

    // Delete confirmation state
    bool show_delete_confirm = false;
    std::string delete_err;

    // General action error (shown under action buttons)
    std::string action_err;

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

        ImGui::Text("RCO Project Manager");
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
            if (ImGui::Selectable(lbl, sel == i)) {
                sel = i;
                action_err.clear();
            }
        }
        if (projects.empty())
            ImGui::TextDisabled("No projects yet. Click 'New Project'.");
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##detail", {fW - list_w - 24.f, fH - 110.f}, false);
        if (sel >= 0 && sel < (int)projects.size()) {
            const auto& p = projects[sel];
            fs::path pdir = fs::absolute(kProjectsDir) / p.name;

            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TextDisabled(p.has_db
                ? "Database: exists (server ran before)"
                : "Database: none (created on first start)");
            ImGui::TextDisabled("%s", pdir.string().c_str());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Open Folder
            if (ImGui::Button("Open Folder")) {
                action_err.clear();
                OpenInExplorer(pdir);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Open in Explorer");

            ImGui::Spacing();

            // Rename
            if (ImGui::Button("Rename")) {
                show_rename_dlg = true;
                std::strncpy(rename_buf, p.name.c_str(), sizeof(rename_buf) - 1);
                rename_err.clear();
                action_err.clear();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Rename project folder");

            ImGui::Spacing();

            // Delete
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.6f, 0.1f, 0.1f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.8f, 0.2f, 0.2f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.5f, 0.05f,0.05f,1.f});
            if (ImGui::Button("Delete Project")) {
                show_delete_confirm = true;
                delete_err.clear();
                action_err.clear();
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();
            ImGui::TextDisabled("Permanently delete project folder");

            if (!action_err.empty())
                ImGui::TextColored({1.f, 0.35f, 0.35f, 1.f}, "%s", action_err.c_str());

        } else {
            ImGui::TextDisabled("Select a project to see actions.");
        }
        ImGui::EndChild();

        if (ImGui::Button("New Project")) {
            show_new_dlg = true;
            new_name[0]  = 0;
            new_err.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            projects = ScanProjects(kProjectsDir);
            sel = -1;
            action_err.clear();
        }

        // ── Delete confirmation modal ──────────────────────────────────────
        if (show_delete_confirm && sel >= 0 && sel < (int)projects.size())
            ImGui::OpenPopup("Delete Project?##del");

        if (ImGui::BeginPopupModal("Delete Project?##del", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            // Guard: sel might have changed while popup was pending
            if (sel < 0 || sel >= (int)projects.size()) {
                show_delete_confirm = false;
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::Text("Permanently delete project '%s'?",
                            projects[sel].name.c_str());
                ImGui::TextColored({1.f,0.6f,0.2f,1.f},
                    "This cannot be undone.");
                if (!delete_err.empty())
                    ImGui::TextColored({1.f,0.35f,0.35f,1.f},
                        "%s", delete_err.c_str());

                ImGui::Spacing();
                if (ImGui::Button("Delete", {120, 0})) {
                    fs::path pdir = fs::absolute(kProjectsDir) / projects[sel].name;
                    std::error_code ec;
                    fs::remove_all(pdir, ec);
                    if (ec) {
                        delete_err = "Delete failed: " + ec.message();
                    } else {
                        projects = ScanProjects(kProjectsDir);
                        sel = -1;
                        show_delete_confirm = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", {120, 0})) {
                    show_delete_confirm = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }

        // ── Rename modal ───────────────────────────────────────────────────
        if (show_rename_dlg)
            ImGui::OpenPopup("Rename Project##ren");

        ImGui::SetNextWindowSize({400, 0});
        if (ImGui::BeginPopupModal("Rename Project##ren", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("New name:");
            ImGui::SetNextItemWidth(360.f);
            bool pressed_enter = ImGui::InputText("##rname", rename_buf,
                sizeof(rename_buf), ImGuiInputTextFlags_EnterReturnsTrue);
            if (!rename_err.empty())
                ImGui::TextColored({1.f,0.35f,0.35f,1.f},
                    "%s", rename_err.c_str());

            auto try_rename = [&]() {
                rename_err.clear();
                std::string n(rename_buf);
                if (n.empty()) { rename_err = "Name cannot be empty."; return; }
                if (n.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                    rename_err = "Invalid characters in name.";
                    return;
                }
                if (sel < 0 || sel >= (int)projects.size()) return;

                fs::path old_path = fs::absolute(kProjectsDir) / projects[sel].name;
                fs::path new_path = fs::absolute(kProjectsDir) / n;
                std::error_code ec;
                if (fs::exists(new_path, ec)) {
                    rename_err = "A project with that name already exists.";
                    return;
                }
                fs::rename(old_path, new_path, ec);
                if (ec) { rename_err = "Rename failed: " + ec.message(); return; }

                projects = ScanProjects(kProjectsDir);
                // Reselect by new name
                sel = -1;
                for (int i = 0; i < (int)projects.size(); ++i)
                    if (projects[i].name == n) { sel = i; break; }
                show_rename_dlg = false;
                ImGui::CloseCurrentPopup();
            };

            if (pressed_enter) try_rename();
            if (ImGui::Button("Rename", {120, 0})) try_rename();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) {
                show_rename_dlg = false;
                rename_err.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ── New Project modal ──────────────────────────────────────────────
        if (show_new_dlg)
            ImGui::OpenPopup("New Project##new");

        ImGui::SetNextWindowSize({400, 0});
        if (ImGui::BeginPopupModal("New Project##new", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Project name:");
            ImGui::SetNextItemWidth(360.f);
            bool pressed_enter = ImGui::InputText("##nname", new_name,
                sizeof(new_name), ImGuiInputTextFlags_EnterReturnsTrue);
            if (!new_err.empty())
                ImGui::TextColored({1.f, 0.35f, 0.35f, 1.f},
                    "%s", new_err.c_str());

            auto try_create = [&]() {
                new_err.clear();
                std::string n(new_name);
                if (n.empty()) { new_err = "Name cannot be empty."; return; }
                if (n.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                    new_err = "Invalid characters in name.";
                    return;
                }
                std::error_code ec;
                fs::path dest = fs::absolute(kProjectsDir) / n;
                if (fs::exists(dest, ec)) {
                    new_err = "A project with that name already exists.";
                    return;
                }
                std::string err = CopyTemplate(fs::absolute(kTemplateDir), dest);
                if (!err.empty()) { new_err = err; return; }

                projects = ScanProjects(kProjectsDir);
                sel = -1;
                for (int i = 0; i < (int)projects.size(); ++i)
                    if (projects[i].name == n) { sel = i; break; }
                show_new_dlg = false;
                ImGui::CloseCurrentPopup();
            };

            if (pressed_enter) try_create();
            if (ImGui::Button("Create", {120, 0})) try_create();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {120, 0})) {
                show_new_dlg = false;
                new_err.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
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
