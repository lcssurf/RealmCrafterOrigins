#include "char_select.h"
#include "ui_texture.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>

namespace rco::ui {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

CharSelect::CharSelect(CharSelectCallbacks cb)
    : cb_(std::move(cb))
{}

// ---------------------------------------------------------------------------
// SetCharacters / SetError
// ---------------------------------------------------------------------------

void CharSelect::SetCharacters(const std::vector<rco::CharacterInfo>& chars) {
    characters_ = chars;
}

void CharSelect::SetError(const std::string& msg) {
    error_msg_ = msg;
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

const rco::CharacterInfo* CharSelect::FindChar(int slot) const {
    for (const auto& c : characters_) {
        if (c.slot == slot) return &c;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Render — entry point
// ---------------------------------------------------------------------------

void CharSelect::Render(int screenW, int screenH) {
    // Background texture.
    if (ImTextureID bg = g_tex.Menu("Character Selection.PNG")) {
        ImGui::GetBackgroundDrawList()->AddImage(
            bg, {0.f, 0.f},
            {static_cast<float>(screenW), static_cast<float>(screenH)});
    }

    // Full-screen invisible background panel so ImGui captures all input.
    ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({static_cast<float>(screenW),
                              static_cast<float>(screenH)}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGuiWindowFlags bg_flags =
        ImGuiWindowFlags_NoDecoration  |
        ImGuiWindowFlags_NoMove        |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoInputs;

    ImGui::Begin("##bg", nullptr, bg_flags);
    ImGui::End();

    // ---------- Main panel ----------
    constexpr float kPanelW = 820.f;
    constexpr float kPanelH = 560.f;
    const float     cx      = screenW * 0.5f;
    const float     cy      = screenH * 0.5f;

    ImGui::SetNextWindowPos({cx - kPanelW * 0.5f, cy - kPanelH * 0.5f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({kPanelW, kPanelH}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.90f);

    ImGuiWindowFlags panel_flags =
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoCollapse      |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##charselect", nullptr, panel_flags);

    // Title
    {
        const char* title = "Select Your Character";
        const float tw    = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((kPanelW - tw) * 0.5f);
        ImGui::TextColored({0.95f, 0.78f, 0.30f, 1.f}, "%s", title);
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Grid of 9 slots
    RenderSlotGrid();

    // Detail / action area for the selected slot
    if (selected_slot_ >= 0 && !show_create_) {
        RenderCharDetail(screenW, screenH);
    }

    // Create form (shown when an empty slot is clicked)
    if (show_create_) {
        RenderCreateForm(screenW, screenH);
    }

    // Error message
    if (!error_msg_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored({0.95f, 0.30f, 0.30f, 1.f}, "%s", error_msg_.c_str());
    }

    // Logout button — bottom left
    ImGui::SetCursorPos({8.f, kPanelH - 36.f});
    if (ImGui::Button("Logout", {100.f, 28.f})) {
        if (cb_.OnLogout) cb_.OnLogout();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Slot grid  (3 columns × 3 rows = 9 slots, 0-indexed)
// ---------------------------------------------------------------------------

void CharSelect::RenderSlotGrid() {
    constexpr int   kCols     = 3;
    constexpr int   kRows     = 3;
    constexpr float kCellW    = 210.f;
    constexpr float kCellH    = 90.f;
    constexpr float kPadding  = 12.f;

    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const int slot = row * kCols + col;

            if (col > 0) ImGui::SameLine(0.f, kPadding);

            const rco::CharacterInfo* ch = FindChar(slot);

            // Build label
            char label[128];
            if (ch) {
                std::snprintf(label, sizeof(label),
                    "%s\n%s %s  Lv %d\n%s",
                    ch->name.c_str(),
                    ch->race.c_str(),
                    ch->charClass.c_str(),
                    ch->level,
                    ch->area.c_str());
            } else {
                std::snprintf(label, sizeof(label), "[ Empty ]");
            }

            // Highlight if selected
            if (slot == selected_slot_) {
                ImGui::PushStyleColor(ImGuiCol_Button,        {0.30f, 0.55f, 0.80f, 1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.40f, 0.65f, 0.90f, 1.f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.20f, 0.45f, 0.70f, 1.f});
            }

            char btn_id[32];
            std::snprintf(btn_id, sizeof(btn_id), "##slot%d", slot);

            // We render an invisible button behind a child region for layout.
            ImGui::BeginGroup();

            const bool clicked = ImGui::Button(btn_id, {kCellW, kCellH});
            // Overlay text inside the button area
            {
                ImVec2 btnMin = ImGui::GetItemRectMin();
                ImVec2 btnMax = ImGui::GetItemRectMax();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                // Slot number badge
                char badge[8];
                std::snprintf(badge, sizeof(badge), "#%d", slot + 1);
                dl->AddText({btnMin.x + 4.f, btnMin.y + 4.f},
                            IM_COL32(180, 180, 180, 200), badge);
                // Character info (multiline via newline in label)
                dl->AddText({btnMin.x + 8.f, btnMin.y + 20.f},
                            ch ? IM_COL32(220, 220, 220, 255)
                               : IM_COL32(120, 120, 120, 200),
                            label);
                (void)btnMax;
            }

            ImGui::EndGroup();

            if (slot == selected_slot_) {
                ImGui::PopStyleColor(3);
            }

            if (clicked) {
                selected_slot_ = slot;
                show_create_   = (ch == nullptr);
                error_msg_.clear();
            }
        }

        ImGui::Spacing();
    }
}

// ---------------------------------------------------------------------------
// Detail panel for a filled slot
// ---------------------------------------------------------------------------

void CharSelect::RenderCharDetail(int /*screenW*/, int /*screenH*/) {
    const rco::CharacterInfo* ch = FindChar(selected_slot_);
    if (!ch) return;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored({0.9f, 0.9f, 0.5f, 1.f},
        "%s  —  %s %s  (Level %d)",
        ch->name.c_str(), ch->race.c_str(), ch->charClass.c_str(), ch->level);
    ImGui::Text("Area: %s    HP: %d / %d",
        ch->area.c_str(), ch->health, ch->healthMax);

    ImGui::Spacing();

    if (ImGui::Button("Enter World", {160.f, 34.f})) {
        error_msg_.clear();
        if (cb_.OnSelect) cb_.OnSelect(selected_slot_);
    }

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,        {0.70f, 0.20f, 0.20f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.85f, 0.30f, 0.30f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.60f, 0.15f, 0.15f, 1.f});

    if (ImGui::Button("Delete", {100.f, 34.f})) {
        confirm_delete_slot_ = selected_slot_;
        ImGui::OpenPopup("Confirm Delete");
    }

    ImGui::PopStyleColor(3);

    // Delete confirmation popup
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?  This cannot be undone.", ch->name.c_str());
        ImGui::Spacing();

        if (ImGui::Button("Delete", {100.f, 0.f})) {
            if (cb_.OnDelete) cb_.OnDelete(confirm_delete_slot_);
            confirm_delete_slot_ = -1;
            selected_slot_       = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", {100.f, 0.f})) {
            confirm_delete_slot_ = -1;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Create character form (shown when an empty slot is clicked)
// ---------------------------------------------------------------------------

void CharSelect::RenderCreateForm(int /*screenW*/, int /*screenH*/) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored({0.9f, 0.9f, 0.5f, 1.f}, "Create New Character  (slot %d)", selected_slot_ + 1);
    ImGui::Spacing();

    // Name
    ImGui::Text("Name");
    ImGui::SetNextItemWidth(240.f);
    ImGui::InputText("##new_name", new_name_, sizeof(new_name_));

    ImGui::SameLine();

    // Race
    ImGui::Text("Race");
    ImGui::SetNextItemWidth(120.f);
    ImGui::Combo("##new_race", &new_race_, kRaces, 4);

    ImGui::SameLine();

    // Class
    ImGui::Text("Class");
    ImGui::SetNextItemWidth(120.f);
    ImGui::Combo("##new_class", &new_class_, kClasses, 4);

    ImGui::Spacing();

    // Gender
    ImGui::Text("Gender:");
    ImGui::SameLine();
    ImGui::RadioButton("Male",   &new_gender_, 0); ImGui::SameLine();
    ImGui::RadioButton("Female", &new_gender_, 1);

    ImGui::Spacing();

    if (ImGui::Button("Create", {120.f, 30.f})) {
        error_msg_.clear();
        if (std::strlen(new_name_) == 0) {
            error_msg_ = "Please enter a character name.";
        } else {
            if (cb_.OnCreate) {
                cb_.OnCreate(
                    selected_slot_,
                    new_name_,
                    kRaces[new_race_],
                    kClasses[new_class_],
                    new_gender_);
            }
            // Reset form
            std::memset(new_name_, 0, sizeof(new_name_));
            new_race_    = 0;
            new_class_   = 0;
            new_gender_  = 0;
            show_create_ = false;
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", {80.f, 30.f})) {
        show_create_   = false;
        selected_slot_ = -1;
        error_msg_.clear();
    }
}

} // namespace rco::ui
