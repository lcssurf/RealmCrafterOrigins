#pragma once

#include <string>
#include <functional>

namespace rco::ui {

// ---------------------------------------------------------------------------
// Callbacks fired by LoginScreen UI interactions
// ---------------------------------------------------------------------------
struct LoginCallbacks {
    std::function<void(const std::string& user, const std::string& pass)>
        OnLogin;

    std::function<void(const std::string& user,
                       const std::string& pass,
                       const std::string& email)>
        OnRegister;
};

// ---------------------------------------------------------------------------
// LoginScreen — ImGui login / account creation panel
// ---------------------------------------------------------------------------
class LoginScreen {
public:
    explicit LoginScreen(LoginCallbacks cb);

    // Render the ImGui window.  Call every frame while in Login state.
    void Render();

    // Display a localised error message beneath the form.
    void SetError(const std::string& msg);

    // Show/hide a "Connecting…" spinner.
    void SetLoading(bool loading);

private:
    LoginCallbacks cb_;

    // Input buffers (NUL-terminated)
    char username_[33]{};   // max 32 chars
    char password_[65]{};   // max 64 chars
    char email_[257]{};     // max 256 chars

    std::string error_msg_;
    bool        loading_       = false;
    bool        show_register_ = false;  // unused — kept for future tab toggle
};

} // namespace rco::ui
