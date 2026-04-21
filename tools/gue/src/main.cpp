#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <array>
#include <filesystem>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <unistd.h>
  #include <limits.h>
#endif

#include "tabs/items.h"
#include "tabs/spells.h"
#include "tabs/actors.h"
#include "tabs/areas.h"
#include "tabs/media.h"

// Anchor cwd to the exe's directory so sibling paths (../server/rco.db) resolve
// correctly regardless of how the tool was launched.
static void SetCwdToExeDir() {
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return;
    exe = buf;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    exe = buf;
#endif
    std::error_code ec;
    std::filesystem::current_path(exe.parent_path(), ec);
}

static void glfwErrorCb(int, const char* desc) {
    std::fprintf(stderr, "[glfw] %s\n", desc);
}

static sqlite3* tryOpen(const char* path) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK)
        return db;
    if (db) sqlite3_close(db);
    return nullptr;
}

static sqlite3* openDB(std::string& outPath) {
    // Canonical layout: dist/tools/rco_gue.exe ↔ dist/server/rco.db.
    // After SetCwdToExeDir() we are in dist/tools, so the DB is one level up
    // and in the server sibling. Also try the two legacy paths for dev setups.
    const char* candidates[] = {
        "../server/rco.db",     // canonical
        "rco.db",               // same dir (dev override)
        "../../dist/server/rco.db", // running from a build/ subfolder
    };
    for (auto p : candidates) {
        if (sqlite3* db = tryOpen(p)) { outPath = p; return db; }
    }
    return nullptr;
}

int main() {
    SetCwdToExeDir();

    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* win = glfwCreateWindow(1200, 700,
        "RCO — Grand Unified Editor", nullptr, nullptr);
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
    io.IniFilename = nullptr; // don't litter cwd with imgui.ini
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 460");

    std::string dbPath;
    sqlite3*    db        = openDB(dbPath);
    bool        showDbDlg = (db == nullptr); // open path dialog immediately if not found
    char        dbPathBuf[512] = {};
    if (!dbPath.empty()) std::strncpy(dbPathBuf, dbPath.c_str(), sizeof(dbPathBuf)-1);

    auto applyDb = [&]() {
        if (db) { sqlite3_close(db); db = nullptr; }
        db = tryOpen(dbPathBuf);
        if (db) {
            dbPath = dbPathBuf;
            sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
            showDbDlg = false;
        }
    };

    if (db) {
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    }

    gue::ItemsTab  itemsTab;
    gue::SpellsTab spellsTab;
    gue::ActorsTab actorsTab;
    gue::AreasTab  areasTab;
    gue::MediaTab  mediaTab;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("##gue_root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_MenuBar);

        // Menu bar
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (db)
                    ImGui::TextDisabled("Connected: %s", dbPath.c_str());
                else
                    ImGui::TextColored({1.f,0.4f,0.4f,1.f}, "No database");
                ImGui::Separator();
                if (ImGui::MenuItem("Change DB path...")) showDbDlg = true;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // DB path dialog (shown on startup if auto-detect fails, or via menu)
        if (showDbDlg) {
            ImGui::SetNextWindowSize({520, 130}, ImGuiCond_Always);
            ImGui::SetNextWindowPos(
                {vp->Pos.x + vp->Size.x*0.5f - 260.f,
                 vp->Pos.y + vp->Size.y*0.5f - 65.f},
                ImGuiCond_Always);
            ImGui::Begin("Database path", nullptr,
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted("Enter full path to rco.db:");
            ImGui::SetNextItemWidth(-1);
            bool enter = ImGui::InputText("##dbpath", dbPathBuf, sizeof(dbPathBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Spacing();
            if (ImGui::Button("Connect", {120, 0}) || enter) applyDb();
            ImGui::SameLine();
            if (db && ImGui::Button("Cancel", {80, 0})) showDbDlg = false;
            if (!db && dbPathBuf[0])
                ImGui::TextColored({1.f,0.4f,0.4f,1.f}, "Could not open: %s", dbPathBuf);
            else if (!db)
                ImGui::TextColored({1.f,0.8f,0.2f,1.f},
                    "Auto-detect failed. Paste the full path to rco.db above.");
            ImGui::End();
        }

        // Main content
        if (db && ImGui::BeginTabBar("##main_tabs")) {
            if (ImGui::BeginTabItem("Items")) {
                itemsTab.Draw(db);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Spells")) {
                spellsTab.Draw(db);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Media")) {
                mediaTab.Draw(db);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Actors")) {
                actorsTab.Draw(db, &mediaTab);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Areas")) {
                areasTab.Draw(db);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        } else if (!db && !showDbDlg) {
            ImGui::TextColored({1.f,0.4f,0.4f,1.f}, "No database connected.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Open...")) showDbDlg = true;
        }

        ImGui::End();

        int fbW, fbH;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.12f, 0.12f, 0.12f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    if (db) sqlite3_close(db);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
