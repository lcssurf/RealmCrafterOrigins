#include "login_screen.h"
#include "ui_texture.h"
#include <imgui.h>

namespace rco::ui {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

LoginScreen::LoginScreen(LoginCallbacks cb)
    : cb_(std::move(cb))
{}

// ---------------------------------------------------------------------------
// SetError / SetLoading
// ---------------------------------------------------------------------------

void LoginScreen::SetError(const std::string& msg) {
    error_msg_ = msg;
}

void LoginScreen::SetLoading(bool loading) {
    loading_ = loading;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void LoginScreen::Render() {
    const ImGuiIO& io = ImGui::GetIO();
    const float sw = io.DisplaySize.x;
    const float sh = io.DisplaySize.y;

    // Background texture.
    if (ImTextureID bg = g_tex.Menu("Login.PNG")) {
        ImGui::GetBackgroundDrawList()->AddImage(
            bg, {0.f, 0.f}, {sw, sh});
    }

    // Place the window in the centre of the display.
    const float cx = sw * 0.5f;
    const float cy = sh * 0.5f;

    constexpr float kW = 400.f;
    constexpr float kH = 310.f;

    ImGui::SetNextWindowPos({cx - kW * 0.5f, cy - kH * 0.5f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kW, kH}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoCollapse      |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##login", nullptr, flags);

    // ---------- Title ----------
    {
        const char* title = "RealmCrafter: Origins";
        const float title_w = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((kW - title_w) * 0.5f);
        ImGui::TextColored({0.95f, 0.78f, 0.30f, 1.f}, "%s", title);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---------- Tab bar ----------
    if (ImGui::BeginTabBar("##login_tabs")) {

        // ====== Login tab ======
        if (ImGui::BeginTabItem("Login")) {
            ImGui::Spacing();

            ImGui::Text("Username");
            ImGui::SetNextItemWidth(kW - 32.f);
            const bool user_enter = ImGui::InputText(
                "##user", username_, sizeof(username_),
                ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();

            ImGui::Text("Password");
            ImGui::SetNextItemWidth(kW - 32.f);
            const bool pass_enter = ImGui::InputText(
                "##pass", password_, sizeof(password_),
                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();
            ImGui::Spacing();

            // Centre the button
            ImGui::SetCursorPosX((kW - 120.f) * 0.5f);
            const bool clicked = ImGui::Button("Login", {120.f, 30.f});

            if ((clicked || user_enter || pass_enter) && !loading_) {
                error_msg_.clear();
                if (cb_.OnLogin) {
                    cb_.OnLogin(username_, password_);
                }
            }

            ImGui::EndTabItem();
        }

        // ====== Create Account tab ======
        if (ImGui::BeginTabItem("Create Account")) {
            ImGui::Spacing();

            ImGui::Text("Username");
            ImGui::SetNextItemWidth(kW - 32.f);
            ImGui::InputText("##reg_user", username_, sizeof(username_));

            ImGui::Spacing();

            ImGui::Text("Password");
            ImGui::SetNextItemWidth(kW - 32.f);
            ImGui::InputText("##reg_pass", password_, sizeof(password_),
                             ImGuiInputTextFlags_Password);

            ImGui::Spacing();

            ImGui::Text("Email");
            ImGui::SetNextItemWidth(kW - 32.f);
            const bool email_enter = ImGui::InputText(
                "##reg_email", email_, sizeof(email_),
                ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::SetCursorPosX((kW - 160.f) * 0.5f);
            const bool reg_clicked = ImGui::Button("Create Account", {160.f, 30.f});

            if ((reg_clicked || email_enter) && !loading_) {
                error_msg_.clear();
                if (cb_.OnRegister) {
                    cb_.OnRegister(username_, password_, email_);
                }
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // ---------- Error message ----------
    if (!error_msg_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored({0.95f, 0.30f, 0.30f, 1.f}, "%s", error_msg_.c_str());
    }

    // ---------- Loading spinner ----------
    if (loading_) {
        ImGui::Spacing();
        // Simple animated dots as a lightweight spinner substitute.
        const float t  = static_cast<float>(ImGui::GetTime());
        const int  dot = static_cast<int>(t * 3.0f) % 4;
        const char* spinners[] = {"Connecting.  ", "Connecting.. ", "Connecting...", "Connecting.  "};
        ImGui::TextColored({0.7f, 0.7f, 0.7f, 1.f}, "%s", spinners[dot]);
    }

    ImGui::End();
}

} // namespace rco::ui
