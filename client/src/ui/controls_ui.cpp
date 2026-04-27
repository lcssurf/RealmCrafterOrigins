#include "controls_ui.h"
#include <imgui.h>
#include <cstdio>

namespace rco::ui {

// ImGuiKey → canonical name used by InputSystem (must match the key strings in
// InputBinding). Only covers the set of keys that make sense to bind in-game.
struct KeyEntry { ImGuiKey key; const char* name; };
static const KeyEntry kRebindableKeys[] = {
    { ImGuiKey_A,            "A"         },
    { ImGuiKey_B,            "B"         },
    { ImGuiKey_C,            "C"         },
    { ImGuiKey_D,            "D"         },
    { ImGuiKey_E,            "E"         },
    { ImGuiKey_F,            "F"         },
    { ImGuiKey_G,            "G"         },
    { ImGuiKey_H,            "H"         },
    { ImGuiKey_I,            "I"         },
    { ImGuiKey_J,            "J"         },
    { ImGuiKey_K,            "K"         },
    { ImGuiKey_L,            "L"         },
    { ImGuiKey_M,            "M"         },
    { ImGuiKey_N,            "N"         },
    { ImGuiKey_O,            "O"         },
    { ImGuiKey_P,            "P"         },
    { ImGuiKey_Q,            "Q"         },
    { ImGuiKey_R,            "R"         },
    { ImGuiKey_S,            "S"         },
    { ImGuiKey_T,            "T"         },
    { ImGuiKey_U,            "U"         },
    { ImGuiKey_V,            "V"         },
    { ImGuiKey_W,            "W"         },
    { ImGuiKey_X,            "X"         },
    { ImGuiKey_Y,            "Y"         },
    { ImGuiKey_Z,            "Z"         },
    { ImGuiKey_0,            "0"         },
    { ImGuiKey_1,            "1"         },
    { ImGuiKey_2,            "2"         },
    { ImGuiKey_3,            "3"         },
    { ImGuiKey_4,            "4"         },
    { ImGuiKey_5,            "5"         },
    { ImGuiKey_6,            "6"         },
    { ImGuiKey_7,            "7"         },
    { ImGuiKey_8,            "8"         },
    { ImGuiKey_9,            "9"         },
    { ImGuiKey_Space,        "Space"     },
    { ImGuiKey_Enter,        "Enter"     },
    { ImGuiKey_Tab,          "Tab"       },
    { ImGuiKey_Backspace,    "Backspace" },
    { ImGuiKey_Delete,       "Delete"    },
    { ImGuiKey_UpArrow,      "Up"        },
    { ImGuiKey_DownArrow,    "Down"      },
    { ImGuiKey_LeftArrow,    "Left"      },
    { ImGuiKey_RightArrow,   "Right"     },
    { ImGuiKey_F1,           "F1"        },
    { ImGuiKey_F2,           "F2"        },
    { ImGuiKey_F3,           "F3"        },
    { ImGuiKey_F4,           "F4"        },
    { ImGuiKey_F5,           "F5"        },
    { ImGuiKey_F6,           "F6"        },
    { ImGuiKey_F7,           "F7"        },
    { ImGuiKey_F8,           "F8"        },
    { ImGuiKey_F9,           "F9"        },
    { ImGuiKey_F10,          "F10"       },
    { ImGuiKey_F11,          "F11"       },
    { ImGuiKey_F12,          "F12"       },
};
static constexpr int kRebindableKeyCount =
    static_cast<int>(sizeof(kRebindableKeys) / sizeof(kRebindableKeys[0]));

static const char* kTriggerLabels[] = { "press", "release", "hold", "double", "axis" };

void ControlsUI::Draw(const std::string& player_name) {
    if (!visible_ || !input_sys_) return;

    // ---- Key capture overlay ----
    if (capturing_) {
        // Draw a centred prompt
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(
            { io.DisplaySize.x * 0.5f - 180.f, io.DisplaySize.y * 0.5f - 22.f },
            ImGuiCond_Always);
        ImGui::SetNextWindowSize({ 360.f, 44.f }, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("##capture_overlay", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove       |
            ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetNextWindowFocus();
        ImGui::Text("Press a key to bind \"%s\"  (Esc = cancel)",
                    capture_action_.c_str());
        ImGui::End();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
            capturing_ = false;
        } else {
            // Build modifier string from held modifier keys
            auto BuildModifier = []() -> std::string {
                const ImGuiIO& io2 = ImGui::GetIO();
                bool ctrl  = io2.KeyCtrl;
                bool shift = io2.KeyShift;
                bool alt   = io2.KeyAlt;
                if (shift && ctrl)  return "Shift+Ctrl";
                if (shift && alt)   return "Shift+Alt";
                if (ctrl  && alt)   return "Ctrl+Alt";
                if (shift)          return "Shift";
                if (ctrl)           return "Ctrl";
                if (alt)            return "Alt";
                return "";
            };

            for (int i = 0; i < kRebindableKeyCount; ++i) {
                if (ImGui::IsKeyPressed(kRebindableKeys[i].key, /*repeat=*/false)) {
                    std::string new_mod = BuildModifier();
                    input_sys_->Rebind(capture_context_,
                                       capture_action_,
                                       capture_trigger_str_,
                                       kRebindableKeys[i].name,
                                       new_mod);
                    capturing_ = false;

                    // Save immediately
                    std::string save_path = "users/" + player_name + "/input.json";
                    input_sys_->SaveLocalOverrides(save_path);

                    if (new_mod.empty())
                        snprintf(status_, sizeof(status_),
                                 "Rebound %s -> %s",
                                 capture_action_.c_str(), kRebindableKeys[i].name);
                    else
                        snprintf(status_, sizeof(status_),
                                 "Rebound %s -> %s+%s",
                                 capture_action_.c_str(),
                                 new_mod.c_str(), kRebindableKeys[i].name);
                    break;
                }
            }
        }
        return; // do not draw the main window while capturing
    }

    // ---- Main window ----
    ImGui::SetNextWindowSize({ 520.f, 440.f }, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Configure Controls", &visible_)) {
        ImGui::End();
        return;
    }

    ImGui::Text("Context: gameplay");
    ImGui::SameLine();
    if (ImGui::Button("Reset to Default")) {
        input_sys_->ResetToDefault();
        // Remove the override file on reset
        std::string save_path = "users/" + player_name + "/input.json";
        input_sys_->SaveLocalOverrides(save_path);
        snprintf(status_, sizeof(status_), "Reset to default bindings.");
    }

    if (status_[0]) {
        ImGui::SameLine();
        ImGui::TextColored({ 0.3f, 1.f, 0.3f, 1.f }, "%s", status_);
    }

    ImGui::Separator();

    if (ImGui::BeginTable("##controls_tbl", 4,
        ImGuiTableFlags_Borders    |
        ImGuiTableFlags_RowBg      |
        ImGuiTableFlags_ScrollY,
        ImVec2(0.f, 340.f))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key",     ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed,  70.f);
        ImGui::TableSetupColumn("Rebind",  ImGuiTableColumnFlags_WidthFixed,  80.f);
        ImGui::TableHeadersRow();

        const auto bindings = input_sys_->GetBindings("gameplay");
        for (const auto& b : bindings) {
            if (!b.remappable) continue;

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(b.action.c_str());

            ImGui::TableSetColumnIndex(1);
            if (b.modifier.empty())
                ImGui::TextUnformatted(b.key.c_str());
            else
                ImGui::Text("%s+%s", b.modifier.c_str(), b.key.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(
                kTriggerLabels[static_cast<int>(b.trigger_type)]);

            ImGui::TableSetColumnIndex(3);
            char btn_lbl[64];
            snprintf(btn_lbl, sizeof(btn_lbl), "Rebind##%s_%s",
                     b.action.c_str(),
                     kTriggerLabels[static_cast<int>(b.trigger_type)]);
            if (ImGui::Button(btn_lbl)) {
                capturing_             = true;
                capture_action_        = b.action;
                capture_context_       = b.context;
                capture_trigger_str_   =
                    kTriggerLabels[static_cast<int>(b.trigger_type)];
                status_[0] = '\0'; // clear previous status
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace rco::ui
