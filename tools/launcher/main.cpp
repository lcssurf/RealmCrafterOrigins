#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
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
    bool        has_db;
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

// ─── Server process management ───────────────────────────────────────────────

struct ServerProc {
    PROCESS_INFORMATION pi{};
    bool                spawned      = false;
    std::atomic<bool>   ready{false};
    bool                timed_out    = false;
    float               start_time   = 0.f;
    std::string         project_name;
    HANDLE              pipe_read    = INVALID_HANDLE_VALUE;
    std::mutex          log_mtx;
    std::string         log_tail;    // capped at ~8 KB, protected by log_mtx
};

static ServerProc g_server;

// Reads server stderr/stdout from pipe until the write end is closed (server
// exits or stop is called). Sets g_server.ready when "server: listening on"
// appears (server.go:104 — emitted after quic.ListenAddr succeeds).
static void ServerPipeReader(HANDLE pipe, ServerProc* srv) {
    char buf[1024];
    DWORD nread;
    std::string line_buf;
    while (ReadFile(pipe, buf, sizeof(buf) - 1, &nread, nullptr) && nread > 0) {
        buf[nread] = 0;
        line_buf.append(buf, nread);
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos + 1);
            line_buf.erase(0, pos + 1);
            if (line.find("server: listening on") != std::string::npos)
                srv->ready.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(srv->log_mtx);
            srv->log_tail += line;
            if (srv->log_tail.size() > 8192)
                srv->log_tail.erase(0, srv->log_tail.size() - 4096);
        }
    }
    // Pipe write-end was closed (server exited or StopServer closed the handle).
    // Do NOT CloseHandle here — ServerProc owns pipe_read.
}

static bool StartServer(const fs::path& project_dir,
                        const std::string& project_name) {
    if (g_server.spawned) return false;

    fs::path exe_path = fs::absolute(project_dir / "server" / "server.exe");
    fs::path work_dir = fs::absolute(project_dir / "server");

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE pipe_r, pipe_w;
    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 0)) return false;
    // Ensure read end is not inherited by the child
    SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_w;
    si.hStdError  = pipe_w;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring cmd  = L"\"" + exe_path.wstring() + L"\" config.toml";
    std::wstring wdir = work_dir.wstring();
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr,
                             wdir.c_str(), &si, &pi);
    CloseHandle(pipe_w); // launcher doesn't write to the pipe

    if (!ok) { CloseHandle(pipe_r); return false; }

    g_server.pi           = pi;
    g_server.spawned      = true;
    g_server.ready        = false;
    g_server.timed_out    = false;
    g_server.start_time   = (float)glfwGetTime();
    g_server.project_name = project_name;
    g_server.pipe_read    = pipe_r;
    { std::lock_guard<std::mutex> lk(g_server.log_mtx); g_server.log_tail.clear(); }

    std::thread(ServerPipeReader, pipe_r, &g_server).detach();
    return true;
}

static void StopServer() {
    if (!g_server.spawned) return;
    TerminateProcess(g_server.pi.hProcess, 0);
    WaitForSingleObject(g_server.pi.hProcess, 3000);
    CloseHandle(g_server.pi.hProcess);
    CloseHandle(g_server.pi.hThread);
    // Closing pipe_read makes any pending ReadFile in the reader thread fail → thread exits.
    if (g_server.pipe_read != INVALID_HANDLE_VALUE) {
        CloseHandle(g_server.pipe_read);
        g_server.pipe_read = INVALID_HANDLE_VALUE;
    }
    g_server.pi           = {};
    g_server.spawned      = false;
    g_server.ready        = false;
    g_server.timed_out    = false;
    g_server.project_name.clear();
}

// Launches exe as a detached process (no pipe, no tracking). Used for GUE and
// client which the user manages independently after spawn.
static bool LaunchDetached(const fs::path& exe, const fs::path& work_dir) {
    std::wstring cmd  = L"\"" + fs::absolute(exe).wstring() + L"\"";
    std::wstring wdir = fs::absolute(work_dir).wstring();
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, wdir.c_str(), &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    return ok == TRUE;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    SetCwdToExeDir();
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

    GLFWwindow* win = glfwCreateWindow(980, 580,
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

    auto projects    = ScanProjects(kProjectsDir);
    int  sel         = -1;
    bool show_new_dlg = false;
    char new_name[128]{};
    std::string new_err;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fb_w, fb_h;
        glfwGetFramebufferSize(win, &fb_w, &fb_h);
        float fW = (float)fb_w;
        float fH = (float)fb_h;

        // Check server timeout (5 s) without holding any lock
        if (g_server.spawned && !g_server.ready && !g_server.timed_out) {
            if ((float)glfwGetTime() - g_server.start_time > 5.f)
                g_server.timed_out = true;
        }

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

        // Global server status bar
        if (g_server.spawned) {
            ImGui::SameLine(fW - 340.f);
            if (g_server.ready.load(std::memory_order_acquire))
                ImGui::TextColored({0.2f,1.f,0.2f,1.f},
                    "Server: READY  [%s]", g_server.project_name.c_str());
            else if (g_server.timed_out)
                ImGui::TextColored({1.f,0.6f,0.2f,1.f},
                    "Server: Timeout [%s]", g_server.project_name.c_str());
            else
                ImGui::TextColored({1.f,1.f,0.2f,1.f},
                    "Server: Starting... [%s]", g_server.project_name.c_str());
        }

        ImGui::Separator();

        float list_w = fW * 0.34f;

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
            fs::path pdir = fs::absolute(kProjectsDir) / p.name;

            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TextDisabled(p.has_db
                ? "Database: exists (server ran before)"
                : "Database: none (created on first start)");

            // ── Server ──────────────────────────────
            ImGui::Separator();
            ImGui::TextUnformatted("Server");
            ImGui::Spacing();

            bool server_for_this = g_server.spawned &&
                                   g_server.project_name == p.name;
            bool server_other    = g_server.spawned && !server_for_this;

            if (!g_server.spawned) {
                if (ImGui::Button("Start Server"))
                    StartServer(pdir, p.name);
            } else if (server_for_this) {
                bool rdy = g_server.ready.load(std::memory_order_acquire);
                if (rdy)
                    ImGui::TextColored({0.2f,1.f,0.2f,1.f}, "READY");
                else if (g_server.timed_out)
                    ImGui::TextColored({1.f,0.6f,0.2f,1.f}, "Timeout");
                else
                    ImGui::TextColored({1.f,1.f,0.2f,1.f}, "Starting...");
                ImGui::SameLine();
                if (ImGui::Button("Stop Server"))
                    StopServer();
            } else {
                // Different project's server is running
                ImGui::BeginDisabled();
                ImGui::Button("Start Server");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(stop '%s' first)", g_server.project_name.c_str());
            }

            // ── Editor (GUE) ─────────────────────────
            // GUE connects to SQLite directly — no server needed.
            ImGui::Separator();
            ImGui::TextUnformatted("Editor");
            ImGui::Spacing();
            if (ImGui::Button("Open GUE")) {
                LaunchDetached(pdir / "tools" / "rco_gue.exe",
                               pdir / "tools");
            }

            // ── Client ───────────────────────────────
            // Client connects to 127.0.0.1:7777 — requires server ready.
            ImGui::Separator();
            ImGui::TextUnformatted("Client");
            ImGui::Spacing();
            bool server_ready = server_for_this &&
                                g_server.ready.load(std::memory_order_acquire);
            if (!server_ready) ImGui::BeginDisabled();
            if (ImGui::Button("Play (Client)")) {
                LaunchDetached(pdir / "client" / "rco_client.exe",
                               pdir / "client");
            }
            if (!server_ready) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(start server first)");
            }

            // ── Server log ───────────────────────────
            if (server_for_this) {
                ImGui::Separator();
                ImGui::TextDisabled("Server log:");
                std::string log_copy;
                { std::lock_guard<std::mutex> lk(g_server.log_mtx);
                  log_copy = g_server.log_tail; }
                ImGui::BeginChild("##slog", {-1, 140.f}, true);
                ImGui::TextUnformatted(log_copy.c_str());
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.f);
                ImGui::EndChild();
            }

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
        }

        // --- New Project modal ---
        if (show_new_dlg)
            ImGui::OpenPopup("New Project##modal");

        ImGui::SetNextWindowSize({400, 0});
        if (ImGui::BeginPopupModal("New Project##modal", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Project name:");
            ImGui::SetNextItemWidth(360.f);
            bool pressed_enter = ImGui::InputText("##nname", new_name,
                sizeof(new_name), ImGuiInputTextFlags_EnterReturnsTrue);
            if (!new_err.empty())
                ImGui::TextColored({1.f, 0.35f, 0.35f, 1.f}, "%s", new_err.c_str());

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

    // Server keeps running when launcher closes (user's server process persists).
    // Release the handles so the OS doesn't wait on us; server is not killed.
    if (g_server.spawned) {
        if (g_server.pipe_read != INVALID_HANDLE_VALUE)
            CloseHandle(g_server.pipe_read);
        CloseHandle(g_server.pi.hProcess);
        CloseHandle(g_server.pi.hThread);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
