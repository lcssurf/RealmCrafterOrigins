#include "zones.h"
#include "media.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace gue {

// ─── Floating toolbar (overlaid inside viewport) ──────────────────────────────

void ZonesTab::DrawFloatingToolbar() {
    // Keyboard shortcuts (Q/W/E/R and F1-F4)
    if (vpHovered_) {
        const bool ctrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                           || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) xformMode_ = kXFormSelect;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) xformMode_ = kXFormMove;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) xformMode_ = kXFormRotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) xformMode_ = kXFormScale;
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) xformMode_ = kXFormSelect;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) xformMode_ = kXFormMove;
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) xformMode_ = kXFormRotate;
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) xformMode_ = kXFormScale;
        if (ImGui::IsKeyPressed(ImGuiKey_T, false))
            gizmoSpace_ = (gizmoSpace_ == kGizmoSpaceWorld) ? kGizmoSpaceLocal : kGizmoSpaceWorld;
        if (!ctrlDown && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            gizmoPivot_ = (gizmoPivot_ == kGizmoPivotOrigin) ? kGizmoPivotBase : kGizmoPivotOrigin;
    }

    // Overlay buttons at top-left of viewport
    ImGui::SetCursorScreenPos({vpOrigin_.x + 6.f, vpOrigin_.y + 6.f});

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    {3.f, 3.f});
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.12f, 0.12f, 0.14f, 0.88f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.30f, 0.35f, 0.95f});

    struct Tool { const char* label; const char* key; XFormMode mode; };
    static const Tool kTools[] = {
        {"Select", "Q", kXFormSelect},
        {"Move",   "W", kXFormMove  },
        {"Rotate", "E", kXFormRotate},
        {"Scale",  "R", kXFormScale },
    };
    for (auto& t : kTools) {
        bool active = (xformMode_ == t.mode);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        {0.22f, 0.52f, 0.88f, 1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.62f, 1.00f, 1.f});
        }
        if (ImGui::Button(t.label, {60.f, 22.f})) xformMode_ = t.mode;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  [%s]", t.label, t.key);
        if (active) ImGui::PopStyleColor(2);
        ImGui::SameLine(0, 3.f);
    }

    ImGui::SameLine(0, 8.f);
    const bool localSpace = (gizmoSpace_ == kGizmoSpaceLocal);
    if (localSpace) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.22f, 0.52f, 0.88f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.62f, 1.00f, 1.f});
    }
    if (ImGui::Button(localSpace ? "Local" : "World", {58.f, 22.f})) {
        gizmoSpace_ = localSpace ? kGizmoSpaceWorld : kGizmoSpaceLocal;
    }
    if (localSpace) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gizmo space [T]");

    ImGui::SameLine(0, 3.f);
    const bool basePivot = (gizmoPivot_ == kGizmoPivotBase);
    if (basePivot) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.22f, 0.52f, 0.88f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.62f, 1.00f, 1.f});
    }
    if (ImGui::Button(basePivot ? "Pivot:Base" : "Pivot:Origin", {88.f, 22.f})) {
        gizmoPivot_ = basePivot ? kGizmoPivotOrigin : kGizmoPivotBase;
    }
    if (basePivot) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gizmo pivot [Y]");

    // ── Render-mode toggle (Simple / Lit) ─────────────────────────────────
    ImGui::SameLine(0, 16.f);
    bool pbr = (renderer_.renderMode() == ZoneRenderer::kRenderPBR);
    if (pbr) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.95f, 0.55f, 0.10f, 1.f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1.00f, 0.65f, 0.20f, 1.f});
    }
    if (ImGui::Button(pbr ? "Lit" : "Simple", {60.f, 22.f})) {
        renderer_.SetRenderMode(pbr ? ZoneRenderer::kRenderSimple
                                    : ZoneRenderer::kRenderPBR);
    }
    if (pbr) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Viewport shading:\n"
                          "  Simple  — fast, flat lighting\n"
                          "  Lit     — full client PBR (shadows/SSAO/IBL)");

    // Camera state hint
    ImGui::SameLine(0, 16.f);
    if (mouseLook_) {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.4f, 1.f, 0.4f, 1.f});
        ImGui::TextUnformatted("● WASD fly");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8.f);
        ImGui::TextDisabled("spd %.0f", cam_.speed);
    } else if (altOrbit_) {
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f, 0.85f, 0.2f, 1.f});
        ImGui::TextUnformatted("● orbit");
        ImGui::PopStyleColor();
    } else if (mmbPan_) {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.7f, 0.7f, 1.f, 1.f});
        ImGui::TextUnformatted("● pan");
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("RMB click=place/context  RMB hold=fly  Alt+LMB=orbit");
    }

    // Armed-placement indicator — shows which model is queued + ESC to cancel
    if (zoneMode_ == kModeScenery && scnModelId_ != 0) {
        ImGui::SameLine(0, 16.f);
        ImGui::PushStyleColor(ImGuiCol_Text, {0.4f, 0.9f, 1.f, 1.f});
        ImGui::TextUnformatted("● Place:");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4.f);
        // Resolve model name
        if (vpHovered_) {
            const char* nm = "?";
            // We don't have media here — just show the id. The asset browser
            // already shows the name clearly, this is just a quick reminder.
            char buf[32]; std::snprintf(buf, sizeof(buf), "id=%d", scnModelId_);
            ImGui::TextUnformatted(buf);
        } else {
            char buf[32]; std::snprintf(buf, sizeof(buf), "id=%d", scnModelId_);
            ImGui::TextUnformatted(buf);
        }
        ImGui::SameLine(0, 6.f);
        if (ImGui::SmallButton("✕##disarm")) {
            zoneMode_   = kModePortal;
            scnModelId_ = 0;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cancel placement [ESC]");
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ─── Viewport (fills its parent child window) ────────────────────────────────

void ZonesTab::DrawViewport(sqlite3* db, MediaTab* media) {
    ImGuiIO& io = ImGui::GetIO();
    float    dt = io.DeltaTime;

    // Fill the entire child window — no padding math needed.
    ImVec2 vp = ImGui::GetContentRegionAvail();
    if (vp.x < 64) vp.x = 64;
    if (vp.y < 64) vp.y = 64;
    vpSize_ = vp;

    renderer_.Resize((int)vp.x, (int)vp.y);
    if (scene_.colVisDirty) {
        scene_.RebuildColVis(db, meshTriCache_);
        renderer_.UploadColVisBatch(scene_.colVis);
    }
    renderer_.RenderFrame(cam_, scene_, selectedID_, selectedType_, dt);

    // Render the FBO as an ImGui Image.  This is the primary widget — ImGui
    // tracks its rect for hover/click detection automatically.
    ImGui::Image(renderer_.GetTexture(), vp, {0.f, 1.f}, {1.f, 0.f});
    vpOrigin_  = ImGui::GetItemRectMin();   // screen-space top-left of the image
    vpHovered_ = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // ── Terrain brush cursor: hover raycast + circle overlay ─────────────
    if (zoneMode_ == kModeTerrain && renderer_.terrain().Loaded()) {
        float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        ImVec2 mp = ImGui::GetMousePos();
        float vpX = mp.x - vpOrigin_.x;
        float vpY = mp.y - vpOrigin_.y;
        if (vpHovered_ && vpX >= 0.f && vpY >= 0.f && vpX < vpSize_.x && vpY < vpSize_.y) {
            float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
            float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
            glm::vec3 ray = cam_.NDCRay(ndcX, ndcY, aspect);
            glm::vec3 hit;
            if (renderer_.terrain().Raycast(cam_.pos, ray, hit)) {
                brushHitPos_     = hit;
                brushHoverValid_ = true;
            } else {
                brushHoverValid_ = false;
            }
        } else {
            brushHoverValid_ = false;
        }

        // Draw 3D cursor circle as ImGui overlay on top of the viewport image
        if (brushHoverValid_) {
            glm::mat4 vpMat = cam_.Proj(aspect) * cam_.View();
            ImDrawList* dl  = ImGui::GetWindowDrawList();

            // Project a world-space ring onto screen space
            static constexpr int kSeg = 48;
            static constexpr float kPi2 = 6.28318530f;
            ImVec2 pts[kSeg + 1];
            int ptCount = 0;
            for (int i = 0; i <= kSeg; ++i) {
                float a = (float)i / kSeg * kPi2;
                glm::vec4 wp = {
                    brushHitPos_.x + std::cos(a) * brushRadius_,
                    brushHitPos_.y + 0.25f,   // slight lift to avoid z-fighting
                    brushHitPos_.z + std::sin(a) * brushRadius_,
                    1.0f
                };
                glm::vec4 clip = vpMat * wp;
                if (clip.w <= 0.0001f) continue;
                clip /= clip.w;
                if (clip.x < -1.5f || clip.x > 1.5f || clip.y < -1.5f || clip.y > 1.5f) continue;
                float sx = vpOrigin_.x + (clip.x * 0.5f + 0.5f) * vpSize_.x;
                float sy = vpOrigin_.y + (-clip.y * 0.5f + 0.5f) * vpSize_.y;
                pts[ptCount++] = {sx, sy};
            }
            if (ptCount > 2) {
                // Dark outline for contrast, then coloured ring
                dl->AddPolyline(pts, ptCount, IM_COL32(0, 0, 0, 140), ImDrawFlags_None, 3.5f);
                ImU32 ringCol = (brushMode_ == 4)
                    ? IM_COL32(255, 215, 50, 230)   // yellow for paint
                    : IM_COL32(255, 255, 255, 230);  // white for sculpt
                dl->AddPolyline(pts, ptCount, ringCol, ImDrawFlags_None, 1.5f);
            }

            // Centre cross-hair
            glm::vec4 cen4 = vpMat * glm::vec4(brushHitPos_, 1.f);
            if (cen4.w > 0.f) {
                cen4 /= cen4.w;
                float cx = vpOrigin_.x + (cen4.x * 0.5f + 0.5f) * vpSize_.x;
                float cy = vpOrigin_.y + (-cen4.y * 0.5f + 0.5f) * vpSize_.y;
                float cs = 4.f;
                dl->AddLine({cx - cs, cy}, {cx + cs, cy}, IM_COL32(0,0,0,140), 2.5f);
                dl->AddLine({cx, cy - cs}, {cx, cy + cs}, IM_COL32(0,0,0,140), 2.5f);
                ImU32 dotCol = (brushMode_ == 4) ? IM_COL32(255,215,50,230) : IM_COL32(255,255,255,230);
                dl->AddLine({cx - cs, cy}, {cx + cs, cy}, dotCol, 1.5f);
                dl->AddLine({cx, cy - cs}, {cx, cy + cs}, dotCol, 1.5f);
            }
        }
    } else {
        brushHoverValid_ = false;
    }

    // ── Camera controls — RC 1.26 style ─────────────────────────────────
    //
    //   SPACE hold          → mouselook on (cursor hidden, mouse rotates view)
    //   SPACE + LMB         → move forward
    //   SPACE + RMB         → move backward
    //   MMB drag            → pan (horizontal = strafe, vertical = up/down)
    //   Shift (held)        → 3× speed boost on ALL movement (keys/pan/zoom)
    //   Numpad 8/2/4/6      → forward / back / strafe left / strafe right
    // ── Unreal-style viewport camera ─────────────────────────────────────────
    //
    //  RMB held          → freelook (mouse rotates camera)
    //  RMB + WASD        → fly: W/S forward/back, A/D strafe left/right
    //  RMB + Q/E         → fly: Q down, E up
    //  RMB + Shift       → 3× speed boost
    //  RMB + Scroll      → adjust fly speed (like Unreal's scroll-to-change-speed)
    //  Scroll (no RMB)   → dolly zoom along forward axis
    //  Alt + LMB drag    → orbit around cam target (look pivot)
    //  Alt + RMB drag    → dolly zoom (drag right = zoom in)
    //  MMB drag          → pan (translate camera plane)
    //  F                 → focus on selection
    //
    // Terrain mode (no RMB): scroll = brush radius, Shift+scroll = strength

    const bool rmbDown    = ImGui::IsMouseDown(1);
    const bool altDown    = ImGui::IsKeyDown(ImGuiKey_LeftAlt)
                         || ImGui::IsKeyDown(ImGuiKey_RightAlt);
    const bool shiftDown  = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                         || ImGui::IsKeyDown(ImGuiKey_RightShift);
    const bool ctrlSelect = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                         || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

    // Right-click has dual use:
    // 1) placement/context in zone editing
    // 2) freelook/fly camera
    //
    // We resolve this like Unreal: start a pending RMB gesture on press,
    // promote to fly once drag/time threshold is crossed, otherwise treat as
    // a click action on release.
    const bool scnArmedIntent = (zoneMode_ == kModeScenery &&
                                 scnModelId_ != 0 &&
                                 xformMode_ == kXFormSelect);
    const bool wantsRmbAddMenu = (xformMode_ == kXFormSelect && !scnArmedIntent);
    const bool wantsRmbDirectPlace =
        (!scnArmedIntent &&
         zoneMode_ != kModeEnviro &&
         zoneMode_ != kModeOther &&
         zoneMode_ != kModeTerrain);
    bool rmbClickAction = false;
    ImVec2 rmbClickPos = ImGui::GetMousePos();

    const bool rmbPressedInViewport = vpHovered_ && ImGui::IsMouseClicked(1) && !altDown;
    if (rmbPressedInViewport) {
        rmbGesturePending_ = true;
        rmbGestureDidFly_ = false;
        rmbGestureStartPos_ = ImGui::GetMousePos();
        rmbGestureStartTime_ = ImGui::GetTime();
    }

    if (rmbGesturePending_ && rmbDown && !altDown) {
        const ImVec2 mp = ImGui::GetMousePos();
        const float dx = mp.x - rmbGestureStartPos_.x;
        const float dy = mp.y - rmbGestureStartPos_.y;
        const float moveSq = dx * dx + dy * dy;
        const double heldSec = ImGui::GetTime() - rmbGestureStartTime_;
        constexpr float kRmbHoldMoveThresholdPx = 4.0f;
        constexpr double kRmbHoldTimeThresholdSec = 0.12;
        if (moveSq >= (kRmbHoldMoveThresholdPx * kRmbHoldMoveThresholdPx) ||
            heldSec >= kRmbHoldTimeThresholdSec) {
            rmbGestureDidFly_ = true;
        }
    }

    // On release, if the gesture never became fly, execute click behavior.
    if (rmbGesturePending_ && !rmbDown && rmbWasDown_) {
        if (!rmbGestureDidFly_) {
            const ImVec2 mp = ImGui::GetMousePos();
            const bool releaseInViewport =
                mp.x >= vpOrigin_.x && mp.x <= (vpOrigin_.x + vpSize_.x) &&
                mp.y >= vpOrigin_.y && mp.y <= (vpOrigin_.y + vpSize_.y);
            if (releaseInViewport) {
                rmbClickAction = true;
                rmbClickPos = mp;
            }
        }
        rmbGesturePending_ = false;
        rmbGestureDidFly_ = false;
    }

    mouseLook_ = rmbGesturePending_ && rmbGestureDidFly_ && rmbDown && !altDown;
    if (mouseLook_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    // MMB pan
    if (vpHovered_ && ImGui::IsMouseClicked(2)) mmbPan_ = true;
    if (!ImGui::IsMouseDown(2))                 mmbPan_ = false;

    // Alt+LMB orbit
    if (vpHovered_ && ImGui::IsMouseClicked(0) && altDown) {
        // Set orbit target = raycast hit or a point 20 units ahead
        ImVec2 mp = ImGui::GetMousePos();
        glm::vec3 hit;
        float ax = mp.x - vpOrigin_.x, ay = mp.y - vpOrigin_.y;
        float asp = vpSize_.x / std::max(vpSize_.y, 1.f);
        float nx =  (ax / vpSize_.x) * 2.f - 1.f;
        float ny = -((ay / vpSize_.y) * 2.f - 1.f);
        glm::vec3 ray = cam_.NDCRay(nx, ny, asp);
        if (renderer_.terrain().Raycast(cam_.pos, ray, hit))
            orbitTarget_ = hit;
        else
            orbitTarget_ = cam_.pos + cam_.Forward() * 20.f;
        altOrbit_ = true;
    }
    if (!ImGui::IsMouseDown(0) || !altDown) altOrbit_ = false;

    // Alt+RMB dolly zoom
    if (vpHovered_ && ImGui::IsMouseClicked(1) && altDown) altDolly_ = true;
    if (!ImGui::IsMouseDown(1) || !altDown)                altDolly_ = false;

    if (vpHovered_ || mouseLook_ || mmbPan_ || altOrbit_ || altDolly_) {
        const float baseSpeed = cam_.speed;
        const float curSpeed  = shiftDown ? baseSpeed * 3.f : baseSpeed;

        // ── Freelook (RMB held) ──────────────────────────────────────────
        if (mouseLook_ && !altDown) {
            // WASD fly
            bool fwd   = ImGui::IsKeyDown(ImGuiKey_W);
            bool back  = ImGui::IsKeyDown(ImGuiKey_S);
            bool left  = ImGui::IsKeyDown(ImGuiKey_A);
            bool right = ImGui::IsKeyDown(ImGuiKey_D);
            bool up    = ImGui::IsKeyDown(ImGuiKey_E);
            bool down  = ImGui::IsKeyDown(ImGuiKey_Q);

            // Scroll while RMB = adjust fly speed (Unreal behaviour)
            if (io.MouseWheel != 0.f) {
                cam_.speed = std::clamp(
                    cam_.speed * std::pow(1.2f, io.MouseWheel), 1.f, 2000.f);
            }

            cam_.Update(dt, fwd, back, left, right, up, down,
                        io.MouseDelta.x, io.MouseDelta.y);
        }

        // ── Alt + LMB orbit ───────────────────────────────────────────────
        if (altOrbit_) {
            float dist = glm::length(cam_.pos - orbitTarget_);
            cam_.yaw   -= io.MouseDelta.x * cam_.sens;
            cam_.pitch += io.MouseDelta.y * cam_.sens;
            cam_.pitch  = std::clamp(cam_.pitch, -89.f, 89.f);
            cam_.pos    = orbitTarget_ - cam_.Forward() * dist;
        }

        // ── Alt + RMB dolly ───────────────────────────────────────────────
        if (altDolly_) {
            float delta = io.MouseDelta.x * 0.05f * curSpeed;
            cam_.pos += cam_.Forward() * delta;
        }

        // ── MMB pan ───────────────────────────────────────────────────────
        if (mmbPan_) {
            float scale = 0.015f * curSpeed / 20.f;
            cam_.pos -= cam_.Right()                  * (io.MouseDelta.x * scale);
            cam_.pos += glm::vec3(0.f, 1.f, 0.f)     * (io.MouseDelta.y * scale);
        }

        // ── Scroll (no RMB, no Alt) ───────────────────────────────────────
        if (!mouseLook_ && !altDolly_) {
            bool terrainScroll = (zoneMode_ == kModeTerrain && vpHovered_);
            if (io.MouseWheel != 0.f) {
                if (terrainScroll) {
                    if (shiftDown)
                        brushStrength_ = std::clamp(brushStrength_ + io.MouseWheel * 0.1f, 0.05f, 3.f);
                    else
                        brushRadius_   = std::clamp(brushRadius_   + io.MouseWheel * 2.f,  1.f, 80.f);
                } else {
                    // Adaptive dolly: faster when far from terrain
                    float kZoom = 0.12f * std::max(cam_.speed, 5.f);
                    cam_.pos += cam_.Forward() * (io.MouseWheel * kZoom);
                }
            }
        }
    }

    // mouseLook_ stays latched from the RMB gesture resolver above.

    // Selection — LMB click (select mode)
    if (vpHovered_ && ImGui::IsMouseClicked(0) && xformMode_ == kXFormSelect) {
        ImVec2 mp = ImGui::GetMousePos();
        float vpX = mp.x - vpOrigin_.x;
        float vpY = mp.y - vpOrigin_.y;
        float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
        float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
        float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        glm::vec3 orig = cam_.pos;
        glm::vec3 dir  = cam_.NDCRay(ndcX, ndcY, aspect);

        // Ray-sphere intersection helper
        auto raySphere = [](const glm::vec3& ro, const glm::vec3& rd,
                            const glm::vec3& center, float r) -> float {
            glm::vec3 oc = ro - center;
            float b = glm::dot(oc, rd);
            float c = glm::dot(oc, oc) - r*r;
            float disc = b*b - c;
            if (disc < 0.f) return -1.f;
            return -b - std::sqrt(disc);
        };
        // Ray-AABB intersection helper
        auto rayAABB = [](const glm::vec3& ro, const glm::vec3& rd,
                          const glm::vec3& mn, const glm::vec3& mx) -> float {
            glm::vec3 invD = 1.f / rd;
            glm::vec3 t0   = (mn - ro) * invD;
            glm::vec3 t1   = (mx - ro) * invD;
            glm::vec3 tmin = glm::min(t0, t1);
            glm::vec3 tmax = glm::max(t0, t1);
            float tenter = std::max({tmin.x, tmin.y, tmin.z});
            float texit  = std::min({tmax.x, tmax.y, tmax.z});
            if (texit < tenter || texit < 0.f) return -1.f;
            return tenter;
        };

        float bestT = 1e9f;
        int   bestID   = -1;
        int   bestType = kSelNone;

        // Test portals
        for (auto& p : scene_.portals) {
            glm::vec3 c = {p.pos.x, p.radius, p.pos.z};
            float t = raySphere(orig, dir, c, p.radius);
            if (t > 0.f && t < bestT) { bestT = t; bestID = p.id; bestType = kSelPortal; }
        }
        // Test triggers
        for (auto& t : scene_.triggers) {
            float r = t.radius;
            float tt = raySphere(orig, dir, {t.x, 0.f, t.z}, r);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = t.id; bestType = kSelTrigger; }
        }
        // Test sound zones
        for (auto& s : scene_.soundZones) {
            float tt = raySphere(orig, dir, {s.x, 0.f, s.z}, s.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = s.id; bestType = kSelSoundZone; }
        }
        // Test colboxes
        for (auto& c : scene_.colBoxes) {
            glm::vec3 half = c.scale * 0.5f;
            float tt = rayAABB(orig, dir, c.pos - half, c.pos + half);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = c.id; bestType = kSelColBox; }
        }
        // Test colspheres
        for (auto& s : scene_.colSpheres) {
            float tt = raySphere(orig, dir, s.pos, s.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = s.id; bestType = kSelColSphere; }
        }
        // Test waypoints
        for (auto& w : scene_.waypoints) {
            float tt = raySphere(orig, dir, w.pos, 0.8f);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = w.id; bestType = kSelWaypoint; }
        }
        // Test NPCs
        for (auto& n : scene_.npcs) {
            glm::vec3 mn = n.pos - glm::vec3(0.35f, 0.f, 0.35f);
            glm::vec3 mx = n.pos + glm::vec3(0.35f, 1.0f, 0.35f);
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = n.id; bestType = kSelNpc; }
        }
        // Test emitters
        for (auto& e : scene_.emitters) {
            glm::vec3 mn = e.pos - glm::vec3(0.4f, 0.f, 0.4f);
            glm::vec3 mx = e.pos + glm::vec3(0.4f, 1.5f, 0.4f);
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = e.id; bestType = kSelEmitter; }
        }
        // Test water
        for (auto& w : scene_.water) {
            glm::vec3 mn = {w.pos.x - w.scale.x*0.5f, w.pos.y - 0.1f, w.pos.z - w.scale.y*0.5f};
            glm::vec3 mx = {w.pos.x + w.scale.x*0.5f, w.pos.y + 0.2f, w.pos.z + w.scale.y*0.5f};
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = w.id; bestType = kSelWater; }
        }
        // Test scenery (approximate AABB from scale)
        for (auto& s : scene_.scenery) {
            glm::vec3 half = s.scale * 0.5f;
            float tt = rayAABB(orig, dir, s.pos - half, s.pos + half);
            if (tt > 0.f && tt < bestT) { bestT = tt; bestID = s.id; bestType = kSelScenery; }
        }

        if (bestID >= 0) {
            // Waypoint link mode: clicking a waypoint sets nextA or nextB on the current selection
            if (wpLinkMode_ && bestType == kSelWaypoint &&
                selectedType_ == kSelWaypoint && selectedID_ >= 0 && bestID != selectedID_) {
                for (auto& w : scene_.waypoints) {
                    if (w.id == selectedID_) {
                        if (wpLinkB_) w.nextB = bestID; else w.nextA = bestID;
                        sqlite3_stmt* s = nullptr;
                        sqlite3_prepare_v2(db,
                            "UPDATE zone_waypoints SET next_a=?,next_b=? WHERE id=?",
                            -1, &s, nullptr);
                        sqlite3_bind_int(s,1,w.nextA); sqlite3_bind_int(s,2,w.nextB);
                        sqlite3_bind_int(s,3,w.id);
                        sqlite3_step(s); sqlite3_finalize(s);
                        std::snprintf(statusMsg_, sizeof(statusMsg_),
                                      "Linked WP #%d → %s WP #%d.", selectedID_,
                                      wpLinkB_ ? "B" : "A", bestID);
                        break;
                    }
                }
                wpLinkMode_ = false;
            } else {
                if (ctrlSelect) {
                    ToggleSelection(bestType, bestID);
                } else {
                    SelectSingle(bestType, bestID);
                }
                // Switch to mode matching selected object type
                switch (bestType) {
                case kSelPortal:    zoneMode_ = kModePortal;    break;
                case kSelTrigger:   zoneMode_ = kModeTrigger;   break;
                case kSelSoundZone: zoneMode_ = kModeSoundZone; break;
                case kSelColBox:    zoneMode_ = kModeColBox;    break;
                case kSelColSphere: zoneMode_ = kModeColSphere; break;
                case kSelWaypoint:  zoneMode_ = kModeWaypoint;  break;
                case kSelNpc:       zoneMode_ = kModeNPC;       break;
                case kSelEmitter:   zoneMode_ = kModeEmitters;  break;
                case kSelWater:     zoneMode_ = kModeWater;     break;
                default: break;
                }
            }
        } else {
            if (!ctrlSelect) {
                ClearSelection();
            }
        }
    }

    // ── Terrain brush (LMB-drag in Terrain mode) ─────────────────────────
    if (zoneMode_ == kModeTerrain && !mouseLook_ && renderer_.terrain().Loaded()) {
        if (vpHovered_ && ImGui::IsMouseDown(0)) {
            // Capture one snapshot at the start of each brush stroke
            if (!terrainStrokeActive_) {
                terrainStrokeActive_ = true;
                TerrainSnapshot snap;
                snap.heights = renderer_.terrain().heightmap().heights;
                snap.splat   = renderer_.terrain().splatmap().data;
                if ((int)terrainUndo_.size() >= kMaxTerrainUndo)
                    terrainUndo_.erase(terrainUndo_.begin());
                terrainUndo_.push_back(std::move(snap));
                terrainRedo_.clear();
            }

            ImVec2 mp  = ImGui::GetMousePos();
            float  vpX = mp.x - vpOrigin_.x;
            float  vpY = mp.y - vpOrigin_.y;
            float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
            float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
            float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
            glm::vec3 origin = cam_.pos;
            glm::vec3 dir    = cam_.NDCRay(ndcX, ndcY, aspect);
            glm::vec3 hit;
            if (renderer_.terrain().Raycast(origin, dir, hit)) {
                if (brushMode_ == 4) {
                    renderer_.terrain().Paint(hit.x, hit.z, brushRadius_,
                                              brushStrength_, dt, brushMaterial_,
                                              (BrushFalloff)brushFalloff_);
                } else {
                    // UI index 5 = Noise → BrushMode::Noise (enum value 4)
                    BrushMode bm = (brushMode_ == 5) ? BrushMode::Noise
                                                      : (BrushMode)brushMode_;
                    renderer_.terrain().ApplyBrush(hit.x, hit.z, brushRadius_,
                                                   brushStrength_, dt, bm,
                                                   brushFlattenH_,
                                                   (BrushFalloff)brushFalloff_);
                }
                brushActive_ = true;
            }
        } else if (ImGui::IsMouseReleased(0)) {
            terrainStrokeActive_ = false;
            brushActive_ = false;
        }
        // Radius shortcuts
        if (vpHovered_) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))  brushRadius_ = std::max(1.f,  brushRadius_ - 2.f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) brushRadius_ = std::min(80.f, brushRadius_ + 2.f);
        }
    }

    // Delete key
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        DeleteSelected(db);

    // Ctrl+Z — undo (terrain undo when in terrain mode, scene undo otherwise)
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (zoneMode_ == kModeTerrain && !terrainUndo_.empty()) {
            TerrainSnapshot cur;
            cur.heights = renderer_.terrain().heightmap().heights;
            cur.splat   = renderer_.terrain().splatmap().data;
            terrainRedo_.push_back(std::move(cur));
            auto& prev = terrainUndo_.back();
            renderer_.terrain().heightmap().heights = prev.heights;
            renderer_.terrain().heightmap().InitGPU();
            renderer_.terrain().splatmap().data  = prev.splat;
            renderer_.terrain().splatmap().dirty = true;
            terrainUndo_.pop_back();
        } else {
            Undo(db);
        }
    }
    // Ctrl+Y — redo (terrain only)
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (zoneMode_ == kModeTerrain && !terrainRedo_.empty()) {
            TerrainSnapshot cur;
            cur.heights = renderer_.terrain().heightmap().heights;
            cur.splat   = renderer_.terrain().splatmap().data;
            if ((int)terrainUndo_.size() >= kMaxTerrainUndo)
                terrainUndo_.erase(terrainUndo_.begin());
            terrainUndo_.push_back(std::move(cur));
            auto& next = terrainRedo_.back();
            renderer_.terrain().heightmap().heights = next.heights;
            renderer_.terrain().heightmap().InitGPU();
            renderer_.terrain().splatmap().data  = next.splat;
            renderer_.terrain().splatmap().dirty = true;
            terrainRedo_.pop_back();
        }
    }

    // Ctrl+D — duplicate
    if (vpHovered_ && io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_D, false))
        DuplicateSelected(db, media);
    if (vpHovered_ && io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_C, false))
        CopySelected();
    if (vpHovered_ && io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_V, false))
        PasteSelected(db, media);

    // Tab — cycle to next object of same type
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        auto cycle = [&](auto& vec, int selType) {
            if (vec.empty()) return;
            int idx = -1;
            for (int i = 0; i < (int)vec.size(); ++i)
                if (vec[i].id == selectedID_) { idx = i; break; }
            idx = (idx + 1) % (int)vec.size();
            SelectSingle(selType, vec[idx].id);
        };
        switch (selectedType_) {
        case kSelPortal:    cycle(scene_.portals,    kSelPortal);    break;
        case kSelTrigger:   cycle(scene_.triggers,   kSelTrigger);   break;
        case kSelSoundZone: cycle(scene_.soundZones, kSelSoundZone); break;
        case kSelColBox:    cycle(scene_.colBoxes,   kSelColBox);    break;
    case kSelColSphere: cycle(scene_.colSpheres, kSelColSphere); break;
        case kSelWaypoint:  cycle(scene_.waypoints,  kSelWaypoint);  break;
        case kSelNpc:       cycle(scene_.npcs,       kSelNpc);       break;
        case kSelEmitter:   cycle(scene_.emitters,   kSelEmitter);   break;
        case kSelWater:     cycle(scene_.water,      kSelWater);     break;
        case kSelScenery:   cycle(scene_.scenery,    kSelScenery);   break;
        }
    }

    // Keyboard nudge (arrow keys on XZ, PageUp/PageDown on Y).
    // Active only when a transform mode is selected and no gizmo drag is active.
    if (vpHovered_ && !mouseLook_ && gizmoAxis_ < 0 && selectedID_ >= 0 &&
        selectedType_ != kSelNone && xformMode_ != kXFormSelect) {
        glm::vec3 pos;
        if (SelectedPos(pos)) {
            const bool ctrlNudge  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                                 || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            const bool shiftNudge = ImGui::IsKeyDown(ImGuiKey_LeftShift)
                                 || ImGui::IsKeyDown(ImGuiKey_RightShift);
            glm::vec3 pre = pos;
            float step = (scnSnapGrid_ && scnGridSize_ > 0.f) ? scnGridSize_ : 0.25f;
            if (shiftNudge) step *= 4.f;
            if (ctrlNudge)  step *= 0.25f;
            bool moved = false;
            bool movedXZ = false;

            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  { pos.x -= step; moved = true; movedXZ = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) { pos.x += step; moved = true; movedXZ = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))    { pos.z -= step; moved = true; movedXZ = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  { pos.z += step; moved = true; movedXZ = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))     { pos.y += step; moved = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))   { pos.y -= step; moved = true; }

            if (moved) {
                if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                    pos.x = std::round(pos.x / scnGridSize_) * scnGridSize_;
                    pos.y = std::round(pos.y / scnGridSize_) * scnGridSize_;
                    pos.z = std::round(pos.z / scnGridSize_) * scnGridSize_;
                }
                if (selectedType_ == kSelScenery && scnAlignGround_ &&
                    movedXZ && renderer_.terrain().Loaded()) {
                    pos.y = renderer_.terrain().heightmap().SampleWorld(pos.x, pos.z);
                }
                SetSelectedPos(pos);
                PushUndo(kUndoMove, selectedType_, selectedID_, pre);
                PersistSelectedPos(db);
            }
        }
    }

    // ── Gizmos: Move / Rotate / Scale ────────────────────────────────────
    // All three share hit-testing against the object's origin; only the
    // meaning of "drag along axis X" differs: translate / rotate / scale.
    glm::vec3 selPos;
    const bool haveSel = (xformMode_ != kXFormSelect && selectedID_ >= 0 &&
                          selectedType_ != kSelNone && SelectedPos(selPos));

    if (haveSel) {
        const glm::mat4 viewProj =
            cam_.Proj(vpSize_.x / std::max(vpSize_.y, 1.f)) * cam_.View();
        glm::vec3 kAxes[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
        bool useLocalAxes = false;
        if (gizmoSpace_ == kGizmoSpaceLocal) {
            glm::vec3 rotDeg;
            if (SelectedRot(rotDeg)) {
                glm::mat4 r(1.f);
                r = glm::rotate(r, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
                r = glm::rotate(r, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
                r = glm::rotate(r, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
                kAxes[0] = glm::normalize(glm::vec3(r * glm::vec4(1,0,0,0)));
                kAxes[1] = glm::normalize(glm::vec3(r * glm::vec4(0,1,0,0)));
                kAxes[2] = glm::normalize(glm::vec3(r * glm::vec4(0,0,1,0)));
                useLocalAxes = true;
            }
        }

        glm::vec3 gizmoPos = selPos;
        if (gizmoPivot_ == kGizmoPivotBase) {
            float pivotDown = 0.f;
            if (selectedType_ == kSelScenery || selectedType_ == kSelColBox) {
                glm::vec3 scl;
                if (SelectedScale(scl)) pivotDown = std::max(0.f, scl.y * 0.5f);
            } else if (selectedType_ == kSelColSphere) {
                glm::vec3 scl;
                if (SelectedScale(scl)) pivotDown = std::max(0.f, scl.x);
            }
            if (pivotDown > 0.f) gizmoPos -= kAxes[1] * pivotDown;
        }

        const float axisLen    = ZoneRenderer::GizmoAxisLength(gizmoPos, cam_.pos);
        const float pickRadius = axisLen * 0.28f;
        const bool  ctrlDown   = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                              || ImGui::IsKeyDown(ImGuiKey_RightCtrl);

        // Which axes does this object support for rotate / scale?
        unsigned allowRot   = 0, allowScale = 0;
        if (selectedType_ == kSelScenery) { allowRot = 0b111; allowScale = 0b111; }
        else if (selectedType_ == kSelNpc)     { allowRot = 0b010; }
        else if (selectedType_ == kSelColBox)    { allowScale = 0b111; }
        else if (selectedType_ == kSelColSphere) { allowScale = 0b001; }   // uniform — X drives radius
        else if (selectedType_ == kSelWater)     { allowScale = 0b101; }

        // Tell the renderer to draw the active gizmo inside its forward pass
        // (so it lands on the same FBO as the rest of the scene).
        ZoneRenderer::GizmoState gz;
        gz.pos        = gizmoPos;
        gz.axis       = gizmoAxis_;
        gz.use_local_axes = useLocalAxes;
        gz.local_axes[0] = kAxes[0];
        gz.local_axes[1] = kAxes[1];
        gz.local_axes[2] = kAxes[2];
        if (xformMode_ == kXFormMove) {
            gz.mode = ZoneRenderer::kGizmoMove;
            gz.allow_axes = 0b111;
        } else if (xformMode_ == kXFormRotate && allowRot) {
            gz.mode = ZoneRenderer::kGizmoRotate;
            gz.allow_axes = allowRot;
        } else if (xformMode_ == kXFormScale && allowScale) {
            gz.mode = ZoneRenderer::kGizmoScale;
            gz.allow_axes = allowScale;
        }
        renderer_.SetGizmo(gz);

        // Build current ray
        ImVec2 mp  = ImGui::GetMousePos();
        float  mx  = mp.x - vpOrigin_.x;
        float  my  = mp.y - vpOrigin_.y;
        float  ndcX =  (mx / vpSize_.x) * 2.f - 1.f;
        float  ndcY = -((my / vpSize_.y) * 2.f - 1.f);
        float  aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        glm::vec3 ro = cam_.pos;
        glm::vec3 rd = cam_.NDCRay(ndcX, ndcY, aspect);

        // Closest-approach `s` on axis line through selPos, plus ray↔axis dist.
        auto rayAxis = [&](const glm::vec3& ad, float& s, float& dist) {
            glm::vec3 w = ro - gizmoPos;
            float b = glm::dot(rd, ad);
            float d = glm::dot(rd, w);
            float e = glm::dot(ad, w);
            float denom = 1.f - b * b;
            if (denom < 1e-6f) { s = 0; dist = 1e9f; return; }
            s = (e - b * d) / denom;
            float t = (b * e - d) / denom;
            dist = glm::length((ro + rd * t) - (gizmoPos + ad * s));
        };

        // Ray-plane intersection: returns hit point. plane passes through
        // selPos with normal `n`. Falls back to selPos if ray is parallel.
        auto rayPlane = [&](const glm::vec3& n) -> glm::vec3 {
            float denom = glm::dot(rd, n);
            if (std::abs(denom) < 1e-6f) return gizmoPos;
            float t = glm::dot(gizmoPos - ro, n) / denom;
            return ro + rd * t;
        };

        auto rayPlaneAt = [&](const glm::vec3& planePoint, const glm::vec3& n,
                              glm::vec3& outHit, float& outT) -> bool {
            float denom = glm::dot(rd, n);
            if (std::abs(denom) < 1e-6f) return false;
            float t = glm::dot(planePoint - ro, n) / denom;
            if (t < 0.f) return false;
            outT = t;
            outHit = ro + rd * t;
            return true;
        };

        // Signed angle of a point on the plane perpendicular to axis `a`.
        auto angleOnRing = [&](int a, const glm::vec3& pt) {
            glm::vec3 v = pt - gizmoPos;
            // Pick the same basis used when drawing the ring.
            glm::vec3 u = (std::abs(kAxes[a].y) > 0.8f) ? glm::vec3(1,0,0)
                                                        : glm::vec3(0,1,0);
            glm::vec3 t = glm::normalize(glm::cross(kAxes[a], u));
            glm::vec3 b = glm::normalize(glm::cross(kAxes[a], t));
            return std::atan2(glm::dot(v, b), glm::dot(v, t));
        };

        auto snapToGrid = [&](float v) -> float {
            if (!scnSnapGrid_ || scnGridSize_ <= 0.f) return v;
            return std::round(v / scnGridSize_) * scnGridSize_;
        };

        auto snapToObjectAxis = [&](float v, int axis) -> float {
            if (useLocalAxes) return v;
            if (!scnObjSnap_ || scnObjSnapDist_ <= 0.f) return v;
            float bestVal   = v;
            float bestDelta = scnObjSnapDist_;
            auto consider = [&](const glm::vec3& p) {
                float d = std::abs(p[axis] - v);
                if (d < bestDelta) {
                    bestDelta = d;
                    bestVal   = p[axis];
                }
            };
            auto pushVec = [&](const auto& vec, int selType) {
                for (const auto& o : vec) {
                    if (selType == selectedType_ && o.id == selectedID_) continue;
                    consider(o.pos);
                }
            };

            pushVec(scene_.portals,     kSelPortal);
            pushVec(scene_.colBoxes,    kSelColBox);
            pushVec(scene_.colSpheres,  kSelColSphere);
            pushVec(scene_.waypoints,   kSelWaypoint);
            pushVec(scene_.npcs,        kSelNpc);
            pushVec(scene_.emitters,    kSelEmitter);
            pushVec(scene_.water,       kSelWater);
            pushVec(scene_.scenery,     kSelScenery);
            pushVec(scene_.spawnPoints, kSelSpawnPoint);
            for (const auto& t : scene_.triggers) {
                if (selectedType_ == kSelTrigger && t.id == selectedID_) continue;
                consider(glm::vec3(t.x, 0.f, t.z));
            }
            for (const auto& s : scene_.soundZones) {
                if (selectedType_ == kSelSoundZone && s.id == selectedID_) continue;
                consider(glm::vec3(s.x, 0.f, s.z));
            }

            return bestVal;
        };

        auto tryGetBounds = [&](int st, int id, glm::vec3& outPos, glm::vec3& outHalf) -> bool {
            if (st == kSelPortal) {
                for (const auto& p : scene_.portals) if (p.id == id) {
                    outPos = p.pos;
                    outHalf = glm::vec3(std::max(0.05f, p.radius));
                    return true;
                }
            } else if (st == kSelTrigger) {
                for (const auto& t : scene_.triggers) if (t.id == id) {
                    outPos = glm::vec3(t.x, 0.f, t.z);
                    outHalf = glm::vec3(std::max(0.05f, t.radius), 0.6f, std::max(0.05f, t.radius));
                    return true;
                }
            } else if (st == kSelSoundZone) {
                for (const auto& s : scene_.soundZones) if (s.id == id) {
                    outPos = glm::vec3(s.x, 0.f, s.z);
                    outHalf = glm::vec3(std::max(0.05f, s.radius), 0.6f, std::max(0.05f, s.radius));
                    return true;
                }
            } else if (st == kSelColBox) {
                for (const auto& c : scene_.colBoxes) if (c.id == id) {
                    outPos = c.pos;
                    outHalf = glm::max(glm::abs(c.scale) * 0.5f, glm::vec3(0.05f));
                    return true;
                }
            } else if (st == kSelColSphere) {
                for (const auto& s : scene_.colSpheres) if (s.id == id) {
                    outPos = s.pos;
                    outHalf = glm::vec3(std::max(0.05f, s.radius));
                    return true;
                }
            } else if (st == kSelWaypoint) {
                for (const auto& w : scene_.waypoints) if (w.id == id) {
                    outPos = w.pos;
                    outHalf = glm::vec3(0.5f);
                    return true;
                }
            } else if (st == kSelNpc) {
                for (const auto& n : scene_.npcs) if (n.id == id) {
                    outPos = n.pos;
                    outHalf = glm::vec3(0.35f, 0.5f, 0.35f);
                    return true;
                }
            } else if (st == kSelEmitter) {
                for (const auto& e : scene_.emitters) if (e.id == id) {
                    outPos = e.pos;
                    outHalf = glm::vec3(0.4f, 0.75f, 0.4f);
                    return true;
                }
            } else if (st == kSelWater) {
                for (const auto& w : scene_.water) if (w.id == id) {
                    outPos = w.pos;
                    outHalf = glm::vec3(std::max(0.05f, w.scale.x * 0.5f), 0.15f, std::max(0.05f, w.scale.y * 0.5f));
                    return true;
                }
            } else if (st == kSelScenery) {
                for (const auto& s : scene_.scenery) if (s.id == id) {
                    outPos = s.pos;
                    outHalf = glm::max(glm::abs(s.scale) * 0.5f, glm::vec3(0.05f));
                    return true;
                }
            } else if (st == kSelSpawnPoint) {
                for (const auto& sp : scene_.spawnPoints) if (sp.id == id) {
                    outPos = sp.pos;
                    outHalf = glm::vec3(std::max(0.05f, sp.radius), 0.75f, std::max(0.05f, sp.radius));
                    return true;
                }
            }
            return false;
        };

        auto tryFaceSnap = [&](glm::vec3& objPos, glm::vec3& outNormal,
                               unsigned axisMask) -> bool {
            if (!scnFaceSnap_ || scnFaceSnapDist_ <= 0.f || useLocalAxes) return false;

            glm::vec3 selPosCur, selHalf;
            if (!tryGetBounds(selectedType_, selectedID_, selPosCur, selHalf)) return false;

            bool found = false;
            float bestGap = scnFaceSnapDist_ + 1.f;
            int bestAxis = -1;
            float bestSign = 0.f;
            float bestCoord = 0.f;

            auto testCandidate = [&](int cType, int cId, const glm::vec3& cPos, const glm::vec3& cHalf) {
                if (cType == selectedType_ && cId == selectedID_) return;
                glm::vec3 d = objPos - cPos;
                for (int a = 0; a < 3; ++a) {
                    if ((axisMask & (1u << a)) == 0) continue;
                    int b = (a + 1) % 3;
                    int c = (a + 2) % 3;
                    if (std::abs(d[b]) > (selHalf[b] + cHalf[b] + 0.05f)) continue;
                    if (std::abs(d[c]) > (selHalf[c] + cHalf[c] + 0.05f)) continue;
                    float gap = std::abs(d[a]) - (selHalf[a] + cHalf[a]);
                    if (gap < 0.f || gap > scnFaceSnapDist_) continue;
                    if (gap < bestGap) {
                        found = true;
                        bestGap = gap;
                        bestAxis = a;
                        bestSign = (d[a] >= 0.f) ? 1.f : -1.f;
                        bestCoord = cPos[a] + bestSign * (selHalf[a] + cHalf[a]);
                    }
                }
            };

            for (const auto& p : scene_.portals)      testCandidate(kSelPortal, p.id, p.pos, glm::vec3(std::max(0.05f, p.radius)));
            for (const auto& t : scene_.triggers)     testCandidate(kSelTrigger, t.id, glm::vec3(t.x, 0.f, t.z), glm::vec3(std::max(0.05f, t.radius), 0.6f, std::max(0.05f, t.radius)));
            for (const auto& s : scene_.soundZones)   testCandidate(kSelSoundZone, s.id, glm::vec3(s.x, 0.f, s.z), glm::vec3(std::max(0.05f, s.radius), 0.6f, std::max(0.05f, s.radius)));
            for (const auto& c : scene_.colBoxes)     testCandidate(kSelColBox, c.id, c.pos, glm::max(glm::abs(c.scale) * 0.5f, glm::vec3(0.05f)));
            for (const auto& s : scene_.colSpheres)   testCandidate(kSelColSphere, s.id, s.pos, glm::vec3(std::max(0.05f, s.radius)));
            for (const auto& w : scene_.waypoints)    testCandidate(kSelWaypoint, w.id, w.pos, glm::vec3(0.5f));
            for (const auto& n : scene_.npcs)         testCandidate(kSelNpc, n.id, n.pos, glm::vec3(0.35f, 0.5f, 0.35f));
            for (const auto& e : scene_.emitters)     testCandidate(kSelEmitter, e.id, e.pos, glm::vec3(0.4f, 0.75f, 0.4f));
            for (const auto& w : scene_.water)        testCandidate(kSelWater, w.id, w.pos, glm::vec3(std::max(0.05f, w.scale.x * 0.5f), 0.15f, std::max(0.05f, w.scale.y * 0.5f)));
            for (const auto& s : scene_.scenery)      testCandidate(kSelScenery, s.id, s.pos, glm::max(glm::abs(s.scale) * 0.5f, glm::vec3(0.05f)));
            for (const auto& sp : scene_.spawnPoints) testCandidate(kSelSpawnPoint, sp.id, sp.pos, glm::vec3(std::max(0.05f, sp.radius), 0.75f, std::max(0.05f, sp.radius)));

            if (!found || bestAxis < 0) return false;
            objPos[bestAxis] = bestCoord;
            outNormal = glm::vec3(0.f);
            outNormal[bestAxis] = bestSign;
            return true;
        };

        auto captureGizmoSelectionStart = [&]() {
            gizmoSelectionStart_.clear();
            const auto refs = ActiveSelection();
            if (refs.size() <= 1) return;
            const int primaryType = selectedType_;
            const int primaryID = selectedID_;
            for (const auto& ref : refs) {
                if (ref.type == kSelNone || ref.id < 0) continue;
                selectedType_ = ref.type;
                selectedID_ = ref.id;
                GizmoSelectionStart st;
                st.type = ref.type;
                st.id = ref.id;
                st.hasPos = SelectedPos(st.pos);
                st.hasRot = SelectedRot(st.rot);
                st.hasScale = SelectedScale(st.scale);
                gizmoSelectionStart_.push_back(st);
            }
            selectedType_ = primaryType;
            selectedID_ = primaryID;
        };

        // ── Press: hit-test and latch onto an axis ────────────────────────
        if (vpHovered_ && ImGui::IsMouseClicked(0) && gizmoAxis_ < 0 && !mouseLook_) {
            if (xformMode_ == kXFormMove) {
                int   best = -1;
                float bestDist = pickRadius;
                float bestS    = 0.f;
                unsigned allow = 0b111u;
                for (int a = 0; a < 3; ++a) {
                    if ((allow & (1u << a)) == 0) continue;
                    float s, d;
                    rayAxis(kAxes[a], s, d);
                    if (s >= -axisLen * 0.2f && s <= axisLen * 1.2f && d < bestDist) {
                        bestDist = d; best = a; bestS = s;
                    }
                }

                // Plane handles: XY/XZ/YZ.
                // ids: 4=XY, 5=XZ, 6=YZ.
                const float planeOff  = axisLen * 0.28f;
                const float planeSize = axisLen * 0.28f;
                struct PlanePick { int id, a, b; glm::vec3 n; };
                const PlanePick planes[] = {
                    {4, 0, 1, glm::normalize(glm::cross(kAxes[0], kAxes[1]))},
                    {5, 0, 2, glm::normalize(glm::cross(kAxes[0], kAxes[2]))},
                    {6, 1, 2, glm::normalize(glm::cross(kAxes[1], kAxes[2]))},
                };
                float bestPlaneT = 1e9f;
                glm::vec3 bestPlaneHit = gizmoPos;
                for (const auto& pl : planes) {
                    glm::vec3 planeCenter = gizmoPos + kAxes[pl.a] * planeOff + kAxes[pl.b] * planeOff;
                    glm::vec3 hit;
                    float t = 0.f;
                    if (!rayPlaneAt(planeCenter, pl.n, hit, t)) continue;
                    glm::vec3 local = hit - gizmoPos;
                    float ua = glm::dot(local, kAxes[pl.a]);
                    float ub = glm::dot(local, kAxes[pl.b]);
                    float mn = planeOff - planeSize * 0.5f;
                    float mx = planeOff + planeSize * 0.5f;
                    if (ua < mn || ua > mx || ub < mn || ub > mx) continue;
                    if (t < bestPlaneT) {
                        bestPlaneT = t;
                        best = pl.id;
                        bestPlaneHit = hit;
                    }
                }

                if (best >= 0) {
                    gizmoAxis_       = best;
                    gizmoStartPos_   = gizmoPos;
                    gizmoStartObjPos_= selPos;
                    gizmoStartS_     = bestS;
                    gizmoStartHit_   = bestPlaneHit;
                    gizmoMoveRotChanged_ = false;
                    gizmoPrePos_     = selPos;
                    glm::vec3 rot;   SelectedRot(rot);     gizmoStartRot_   = rot;   gizmoPreRot_ = rot;
                    glm::vec3 scl;   SelectedScale(scl);   gizmoStartScale_ = scl;   gizmoPreScale_ = scl;
                    captureGizmoSelectionStart();
                }
            } else if (xformMode_ == kXFormScale) {
                int   best = -1;
                float bestDist = pickRadius;
                float bestS    = 0.f;
                unsigned allow = allowScale;
                for (int a = 0; a < 3; ++a) {
                    if ((allow & (1u << a)) == 0) continue;
                    float s, d;
                    rayAxis(kAxes[a], s, d);
                    if (s >= -axisLen * 0.2f && s <= axisLen * 1.2f && d < bestDist) {
                        bestDist = d; best = a; bestS = s;
                    }
                }
                // Centre handle for uniform scale — hit if the ray passes near
                // the origin itself.
                {
                    float s0, d0; rayAxis(glm::vec3(0, 1, 0), s0, d0);
                    // Cheaper: distance ray ↔ point
                    glm::vec3 w = gizmoPos - ro;
                    float t = glm::dot(w, rd);
                    glm::vec3 closest = ro + rd * t;
                    float d = glm::length(closest - gizmoPos);
                    if (d < pickRadius * 0.65f) { best = 3; bestS = 0.f; }
                }
                if (best >= 0) {
                    gizmoAxis_       = best;
                    gizmoStartPos_   = gizmoPos;
                    gizmoStartObjPos_= selPos;
                    gizmoStartS_     = bestS;
                    gizmoPrePos_     = selPos;
                    glm::vec3 rot;   SelectedRot(rot);     gizmoStartRot_   = rot;   gizmoPreRot_ = rot;
                    glm::vec3 scl;   SelectedScale(scl);   gizmoStartScale_ = scl;   gizmoPreScale_ = scl;
                    captureGizmoSelectionStart();
                }
            } else if (xformMode_ == kXFormRotate && allowRot) {
                int   best = -1;
                float bestDist = axisLen * 0.08f;
                for (int a = 0; a < 3; ++a) {
                    if ((allowRot & (1u << a)) == 0) continue;
                    glm::vec3 hit = rayPlane(kAxes[a]);
                    float dist = std::abs(glm::length(hit - gizmoPos) - axisLen);
                    if (dist < bestDist) { bestDist = dist; best = a; }
                }
                if (best >= 0) {
                    gizmoAxis_     = best;
                    gizmoStartObjPos_ = selPos;
                    gizmoStartS_   = angleOnRing(best, rayPlane(kAxes[best]));
                    gizmoLastAngle_ = gizmoStartS_;
                    gizmoRotAccumDeg_ = 0.f;
                    glm::vec3 rot; SelectedRot(rot); gizmoStartRot_ = rot; gizmoPreRot_ = rot;
                    captureGizmoSelectionStart();
                }
            }
        }

        // ── Drag / release ───────────────────────────────────────────────
        if (gizmoAxis_ >= 0) {
            if (ImGui::IsMouseDown(0)) {
                if (xformMode_ == kXFormMove) {
                    glm::vec3 np = gizmoStartPos_;
                    if (gizmoAxis_ >= 0 && gizmoAxis_ <= 2) {
                        float s, d;
                        rayAxis(kAxes[gizmoAxis_], s, d);
                        float delta = (s - gizmoStartS_);
                        if (ctrlDown)       delta *= 0.25f;
                        else if (shiftDown) delta *= 2.0f;
                        if (useLocalAxes && scnSnapGrid_ && scnGridSize_ > 0.f) {
                            delta = std::round(delta / scnGridSize_) * scnGridSize_;
                        }
                        np = gizmoStartPos_ + kAxes[gizmoAxis_] * delta;
                        if (!useLocalAxes) {
                            np[gizmoAxis_] = snapToGrid(np[gizmoAxis_]);
                            np[gizmoAxis_] = snapToObjectAxis(np[gizmoAxis_], gizmoAxis_);
                        }
                    } else if (gizmoAxis_ >= 4 && gizmoAxis_ <= 6) {
                        int a = 0, b = 1;
                        glm::vec3 n = glm::normalize(glm::cross(kAxes[0], kAxes[1]));
                        if (gizmoAxis_ == 4) { a = 0; b = 1; n = glm::normalize(glm::cross(kAxes[0], kAxes[1])); }
                        if (gizmoAxis_ == 5) { a = 0; b = 2; n = glm::normalize(glm::cross(kAxes[0], kAxes[2])); }
                        if (gizmoAxis_ == 6) { a = 1; b = 2; n = glm::normalize(glm::cross(kAxes[1], kAxes[2])); }
                        glm::vec3 hit;
                        float t = 0.f;
                        if (rayPlaneAt(gizmoStartPos_, n, hit, t)) {
                            glm::vec3 delta = hit - gizmoStartHit_;
                            if (ctrlDown)       delta *= 0.25f;
                            else if (shiftDown) delta *= 2.0f;
                            np = gizmoStartPos_ + delta;
                            if (useLocalAxes) {
                                glm::vec3 dv = np - gizmoStartPos_;
                                float da = glm::dot(dv, kAxes[a]);
                                float db = glm::dot(dv, kAxes[b]);
                                if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                                    da = std::round(da / scnGridSize_) * scnGridSize_;
                                    db = std::round(db / scnGridSize_) * scnGridSize_;
                                }
                                np = gizmoStartPos_ + kAxes[a] * da + kAxes[b] * db;
                            } else {
                                np[a] = snapToGrid(np[a]);
                                np[b] = snapToGrid(np[b]);
                                np[a] = snapToObjectAxis(np[a], a);
                                np[b] = snapToObjectAxis(np[b], b);
                            }
                        }
                    }
                    glm::vec3 newObjPos = gizmoStartObjPos_ + (np - gizmoStartPos_);
                    if (selectedType_ == kSelScenery && scnAlignGround_ &&
                        gizmoAxis_ != 1 && gizmoAxis_ != 4 && gizmoAxis_ != 6 &&
                        renderer_.terrain().Loaded()) {
                        newObjPos.y = renderer_.terrain().heightmap().SampleWorld(newObjPos.x, newObjPos.z);
                    }
                    glm::vec3 snapNormal(0.f);
                    unsigned faceSnapAxisMask = 0b111u;
                    if (gizmoAxis_ >= 0 && gizmoAxis_ <= 2) {
                        faceSnapAxisMask = (1u << gizmoAxis_);
                    } else if (gizmoAxis_ == 4) {        // XY
                        faceSnapAxisMask = (1u << 0) | (1u << 1);
                    } else if (gizmoAxis_ == 5) {        // XZ
                        faceSnapAxisMask = (1u << 0) | (1u << 2);
                    } else if (gizmoAxis_ == 6) {        // YZ
                        faceSnapAxisMask = (1u << 1) | (1u << 2);
                    }
                    if (tryFaceSnap(newObjPos, snapNormal, faceSnapAxisMask)) {
                        const bool wantAutoRotate = (scnAlignNormal_ || scnAutoRotate_);
                        if (wantAutoRotate && selectedType_ == kSelScenery &&
                            std::abs(snapNormal.y) < 0.5f) {
                            glm::vec3 rot;
                            if (SelectedRot(rot)) {
                                auto wrapDeg = [](float a) -> float {
                                    while (a >= 180.f) a -= 360.f;
                                    while (a < -180.f) a += 360.f;
                                    return a;
                                };
                                auto absDeltaDeg = [&](float a, float b) -> float {
                                    return std::abs(wrapDeg(a - b));
                                };

                                float baseYaw = rot.y;
                                if (std::abs(snapNormal.x) > std::abs(snapNormal.z)) {
                                    baseYaw = (snapNormal.x > 0.f) ? -90.f : 90.f;
                                } else {
                                    baseYaw = (snapNormal.z > 0.f) ? 180.f : 0.f;
                                }

                                float newYaw = baseYaw;
                                if (scnAutoRotate_) {
                                    float step = (scnRotSnap_ > 0.001f) ? scnRotSnap_ : 90.f;
                                    if (step < 0.25f) step = 0.25f;
                                    int maxK = std::max(1, (int)std::ceil(180.f / step));
                                    float bestErr = 1e9f;
                                    for (int k = -maxK; k <= maxK; ++k) {
                                        float cand = baseYaw + step * (float)k;
                                        float err = absDeltaDeg(cand, rot.y);
                                        if (err < bestErr) {
                                            bestErr = err;
                                            newYaw = cand;
                                        }
                                    }
                                }

                                if (absDeltaDeg(newYaw, rot.y) > 0.001f) {
                                    rot.y = newYaw;
                                    SetSelectedRot(rot);
                                    gizmoMoveRotChanged_ = true;
                                }
                            }
                        }
                    }
                    SetSelectedPos(newObjPos);
                    if (!gizmoSelectionStart_.empty()) {
                        const int primaryType = selectedType_;
                        const int primaryID = selectedID_;
                        const glm::vec3 deltaObj = newObjPos - gizmoStartObjPos_;
                        for (const auto& st : gizmoSelectionStart_) {
                            if (!st.hasPos) continue;
                            if (st.type == primaryType && st.id == primaryID) continue;
                            selectedType_ = st.type;
                            selectedID_ = st.id;
                            glm::vec3 peerPos = st.pos + deltaObj;
                            if (st.type == kSelScenery && scnAlignGround_ &&
                                gizmoAxis_ != 1 && gizmoAxis_ != 4 && gizmoAxis_ != 6 &&
                                renderer_.terrain().Loaded()) {
                                peerPos.y = renderer_.terrain().heightmap().SampleWorld(peerPos.x, peerPos.z);
                            }
                            SetSelectedPos(peerPos);
                        }
                        selectedType_ = primaryType;
                        selectedID_ = primaryID;
                    }
                } else if (xformMode_ == kXFormScale) {
                    if (gizmoAxis_ == 3) {
                        // Uniform: drag vertically on screen. Use mouse ΔY.
                        float speed = ctrlDown ? 0.25f : (shiftDown ? 2.0f : 1.0f);
                        float dy = -ImGui::GetIO().MouseDelta.y * 0.01f * speed;
                        glm::vec3 s = gizmoStartScale_ * (1.f + dy);
                        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                            s.x = std::round(s.x / scnGridSize_) * scnGridSize_;
                            s.y = std::round(s.y / scnGridSize_) * scnGridSize_;
                            s.z = std::round(s.z / scnGridSize_) * scnGridSize_;
                        }
                        s = glm::max(s, glm::vec3(0.01f));
                        SetSelectedScale(s);
                        if (!gizmoSelectionStart_.empty()) {
                            const int primaryType = selectedType_;
                            const int primaryID = selectedID_;
                            glm::vec3 factor(1.f);
                            factor.x = (std::abs(gizmoStartScale_.x) > 1e-5f) ? (s.x / gizmoStartScale_.x) : 1.f;
                            factor.y = (std::abs(gizmoStartScale_.y) > 1e-5f) ? (s.y / gizmoStartScale_.y) : 1.f;
                            factor.z = (std::abs(gizmoStartScale_.z) > 1e-5f) ? (s.z / gizmoStartScale_.z) : 1.f;
                            for (const auto& st : gizmoSelectionStart_) {
                                if (st.type == primaryType && st.id == primaryID) continue;
                                selectedType_ = st.type;
                                selectedID_ = st.id;
                                if (st.hasScale) {
                                    glm::vec3 ns = glm::max(st.scale * factor, glm::vec3(0.01f));
                                    SetSelectedScale(ns);
                                }
                                if (st.hasPos) {
                                    glm::vec3 rel = st.pos - gizmoStartObjPos_;
                                    glm::vec3 newPos = gizmoStartObjPos_ + rel * factor;
                                    SetSelectedPos(newPos);
                                }
                            }
                            selectedType_ = primaryType;
                            selectedID_ = primaryID;
                        }
                        gizmoStartScale_ = s;   // accumulate on next frame
                    } else {
                        float s, d;
                        rayAxis(kAxes[gizmoAxis_], s, d);
                        float factor = (axisLen > 0.f) ? (s / axisLen) : 1.f;
                        factor = glm::clamp(factor, 0.01f, 100.f);
                        if (ctrlDown)       factor = 1.f + (factor - 1.f) * 0.25f;
                        else if (shiftDown) factor = 1.f + (factor - 1.f) * 2.0f;
                        glm::vec3 ns = gizmoStartScale_;
                        ns[gizmoAxis_] = gizmoStartScale_[gizmoAxis_] * factor;
                        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                            ns[gizmoAxis_] = std::round(ns[gizmoAxis_] / scnGridSize_) * scnGridSize_;
                        }
                        ns = glm::max(ns, glm::vec3(0.01f));
                        SetSelectedScale(ns);
                        if (!gizmoSelectionStart_.empty()) {
                            const int primaryType = selectedType_;
                            const int primaryID = selectedID_;
                            glm::vec3 factor(1.f);
                            factor[gizmoAxis_] =
                                (std::abs(gizmoStartScale_[gizmoAxis_]) > 1e-5f)
                                    ? (ns[gizmoAxis_] / gizmoStartScale_[gizmoAxis_])
                                    : 1.f;
                            for (const auto& st : gizmoSelectionStart_) {
                                if (st.type == primaryType && st.id == primaryID) continue;
                                selectedType_ = st.type;
                                selectedID_ = st.id;
                                if (st.hasScale) {
                                    glm::vec3 peerScale = st.scale;
                                    peerScale[gizmoAxis_] = std::max(0.01f, st.scale[gizmoAxis_] * factor[gizmoAxis_]);
                                    SetSelectedScale(peerScale);
                                }
                                if (st.hasPos) {
                                    glm::vec3 rel = st.pos - gizmoStartObjPos_;
                                    rel[gizmoAxis_] *= factor[gizmoAxis_];
                                    SetSelectedPos(gizmoStartObjPos_ + rel);
                                }
                            }
                            selectedType_ = primaryType;
                            selectedID_ = primaryID;
                        }
                    }
                } else if (xformMode_ == kXFormRotate) {
                    glm::vec3 hit = rayPlane(kAxes[gizmoAxis_]);
                    float a = angleOnRing(gizmoAxis_, hit);
                    float delta = a - gizmoLastAngle_;
                    static constexpr float kPi = 3.14159265359f;
                    static constexpr float kTau = 6.28318530718f;
                    while (delta >  kPi) delta -= kTau;
                    while (delta < -kPi) delta += kTau;
                    float speed = ctrlDown ? 0.25f : (shiftDown ? 2.0f : 1.0f);
                    gizmoRotAccumDeg_ += glm::degrees(delta) * speed;
                    gizmoLastAngle_ = a;
                    float delta_deg = gizmoRotAccumDeg_;
                    if (scnRotSnap_ > 0.f && !ctrlDown) {
                        delta_deg = std::round(delta_deg / scnRotSnap_) * scnRotSnap_;
                    }
                    glm::vec3 rot = gizmoStartRot_;
                    rot[gizmoAxis_] += delta_deg;
                    SetSelectedRot(rot);
                    if (!gizmoSelectionStart_.empty()) {
                        const int primaryType = selectedType_;
                        const int primaryID = selectedID_;
                        const glm::mat4 rotMat = glm::rotate(
                            glm::mat4(1.f), glm::radians(delta_deg), kAxes[gizmoAxis_]);
                        for (const auto& st : gizmoSelectionStart_) {
                            if (st.type == primaryType && st.id == primaryID) continue;
                            selectedType_ = st.type;
                            selectedID_ = st.id;
                            if (st.hasRot) {
                                glm::vec3 peerRot = st.rot;
                                peerRot[gizmoAxis_] += delta_deg;
                                SetSelectedRot(peerRot);
                            }
                            if (st.hasPos) {
                                glm::vec3 rel = st.pos - gizmoStartObjPos_;
                                glm::vec3 newRel = glm::vec3(rotMat * glm::vec4(rel, 0.f));
                                SetSelectedPos(gizmoStartObjPos_ + newRel);
                            }
                        }
                        selectedType_ = primaryType;
                        selectedID_ = primaryID;
                    }
                }
            } else {
                // Release: persist + undo entry for the modified transform.
                if (xformMode_ == kXFormMove) {
                    if (gizmoSelectionStart_.empty()) {
                        PushUndo(kUndoMove,   selectedType_, selectedID_, gizmoPrePos_);
                        PersistSelectedPos(db);
                        glm::vec3 curRot;
                        if (gizmoMoveRotChanged_ && SelectedRot(curRot) &&
                            glm::length(curRot - gizmoPreRot_) > 0.001f) {
                            PushUndo(kUndoRotate, selectedType_, selectedID_, gizmoPreRot_);
                            PersistSelectedRot(db);
                        }
                        std::snprintf(statusMsg_, sizeof(statusMsg_), "Moved id=%d.", selectedID_);
                    } else {
                        const int primaryType = selectedType_;
                        const int primaryID = selectedID_;
                        int movedCount = 0;
                        for (const auto& st : gizmoSelectionStart_) {
                            selectedType_ = st.type;
                            selectedID_ = st.id;
                            glm::vec3 curPos;
                            if (st.hasPos && SelectedPos(curPos) &&
                                glm::length(curPos - st.pos) > 0.0005f) {
                                PushUndo(kUndoMove, st.type, st.id, st.pos);
                                PersistSelectedPos(db);
                                movedCount++;
                            }
                            glm::vec3 curRot;
                            if (gizmoMoveRotChanged_ && st.hasRot && SelectedRot(curRot) &&
                                glm::length(curRot - st.rot) > 0.001f) {
                                PushUndo(kUndoRotate, st.type, st.id, st.rot);
                                PersistSelectedRot(db);
                            }
                        }
                        selectedType_ = primaryType;
                        selectedID_ = primaryID;
                        std::snprintf(statusMsg_, sizeof(statusMsg_), "Moved %d object(s).", std::max(movedCount, 1));
                    }
                    gizmoMoveRotChanged_ = false;
                } else if (xformMode_ == kXFormRotate) {
                    if (gizmoSelectionStart_.empty()) {
                        PushUndo(kUndoRotate, selectedType_, selectedID_, gizmoPreRot_);
                        PersistSelectedRot(db);
                        std::snprintf(statusMsg_, sizeof(statusMsg_), "Rotated id=%d.", selectedID_);
                    } else {
                        const int primaryType = selectedType_;
                        const int primaryID = selectedID_;
                        int changedCount = 0;
                        for (const auto& st : gizmoSelectionStart_) {
                            selectedType_ = st.type;
                            selectedID_ = st.id;
                            glm::vec3 curRot;
                            if (st.hasRot && SelectedRot(curRot) &&
                                glm::length(curRot - st.rot) > 0.001f) {
                                PushUndo(kUndoRotate, st.type, st.id, st.rot);
                                PersistSelectedRot(db);
                                changedCount++;
                            }
                            glm::vec3 curPos;
                            if (st.hasPos && SelectedPos(curPos) &&
                                glm::length(curPos - st.pos) > 0.0005f) {
                                PushUndo(kUndoMove, st.type, st.id, st.pos);
                                PersistSelectedPos(db);
                            }
                        }
                        selectedType_ = primaryType;
                        selectedID_ = primaryID;
                        std::snprintf(statusMsg_, sizeof(statusMsg_), "Rotated %d object(s).", std::max(changedCount, 1));
                    }
                } else if (xformMode_ == kXFormScale) {
                    if (gizmoSelectionStart_.empty()) {
                        PushUndo(kUndoScale,  selectedType_, selectedID_, gizmoPreScale_);
                        PersistSelectedScale(db);
                        std::snprintf(statusMsg_, sizeof(statusMsg_), "Scaled id=%d.", selectedID_);
                    } else {
                        const int primaryType = selectedType_;
                        const int primaryID = selectedID_;
                        int changedCount = 0;
                        for (const auto& st : gizmoSelectionStart_) {
                            selectedType_ = st.type;
                            selectedID_ = st.id;
                            glm::vec3 curScale;
                            if (st.hasScale && SelectedScale(curScale) &&
                                glm::length(curScale - st.scale) > 0.0005f) {
                                PushUndo(kUndoScale, st.type, st.id, st.scale);
                                PersistSelectedScale(db);
                                changedCount++;
                            }
                            glm::vec3 curPos;
                            if (st.hasPos && SelectedPos(curPos) &&
                                glm::length(curPos - st.pos) > 0.0005f) {
                                PushUndo(kUndoMove, st.type, st.id, st.pos);
                                PersistSelectedPos(db);
                            }
                        }
                        selectedType_ = primaryType;
                        selectedID_ = primaryID;
                        std::snprintf(statusMsg_, sizeof(statusMsg_), "Scaled %d object(s).", std::max(changedCount, 1));
                    }
                }
                gizmoAxis_ = -1;
                gizmoSelectionStart_.clear();
            }
        }
    } else {
        gizmoAxis_ = -1;
        gizmoMoveRotChanged_ = false;
        gizmoSelectionStart_.clear();
        renderer_.SetGizmo({});   // clear — nothing to draw this frame
    }

    // Floating toolbar overlaid on top-left of viewport
    DrawFloatingToolbar();

    // Right-click context menu: "Add Object" when in select mode,
    // ── Placement: LMB direct when a model is armed in the asset browser ───
    // If kModeScenery is active and a model is selected, LMB places without
    // any context menu. This mirrors Unreal: pick asset → click to place.
    // RMB still opens the context menu for placing other object types.
    const bool scnArmed = (zoneMode_ == kModeScenery && scnModelId_ != 0
                           && xformMode_ == kXFormSelect && !mouseLook_);
    if (vpHovered_ && ImGui::IsMouseClicked(0) && scnArmed) {
        ImVec2 mp = ImGui::GetMousePos();
        glm::vec3 pos = RaycastScene(mp.x - vpOrigin_.x, mp.y - vpOrigin_.y);
        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
            pos.x = std::round(pos.x / scnGridSize_) * scnGridSize_;
            pos.z = std::round(pos.z / scnGridSize_) * scnGridSize_;
        }
        PlaceObject(pos, db, media);
        // Keep mode armed so the user can keep clicking to place more.
    }

    // RMB context / placement action (resolved on release when gesture did not become fly).
    if (rmbClickAction && !mouseLook_) {
        float vpX = rmbClickPos.x - vpOrigin_.x;
        float vpY = rmbClickPos.y - vpOrigin_.y;
        if (wantsRmbAddMenu && !scnArmed) {
            pendingPlacePos_ = RaycastScene(vpX, vpY);
            ImGui::OpenPopup("##vp_add_ctx");
        } else if (wantsRmbDirectPlace && !scnArmed) {
            PlaceObject(RaycastScene(vpX, vpY), db, media);
        }
    }

    // ESC disarms scenery placement mode.
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)
        && zoneMode_ == kModeScenery) {
        zoneMode_   = kModePortal;   // back to neutral
        scnModelId_ = 0;
    }

    // Cursor hint when armed
    if (scnArmed) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 mp = ImGui::GetMousePos();
        dl->AddCircle(mp, 8.f, IM_COL32(100,200,255,220), 16, 1.5f);
        dl->AddLine({mp.x-10,mp.y},{mp.x+10,mp.y}, IM_COL32(100,200,255,220), 1.f);
        dl->AddLine({mp.x,mp.y-10},{mp.x,mp.y+10}, IM_COL32(100,200,255,220), 1.f);
    }

    // F key: focus camera on selected object
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_F, false))
        FocusOnSelected();

    // ── Add-object context menu (RMB when no model armed) ────────────────
    ImGui::SetNextWindowSize({160.f, 0.f});
    if (ImGui::BeginPopup("##vp_add_ctx")) {
        ImGui::PushStyleColor(ImGuiCol_Text, {0.6f, 0.8f, 1.f, 1.f});
        ImGui::TextUnformatted("Add object");
        ImGui::PopStyleColor();
        ImGui::Separator();

        struct AddEntry { const char* label; ZoneMode mode; };
        static const AddEntry kEntries[] = {
            {"Portal",           kModePortal     },
            {"Trigger",          kModeTrigger    },
            {"Sound Zone",       kModeSoundZone  },
            {"Collision Box",    kModeColBox     },
            {"Collision Sphere", kModeColSphere  },
            {"Waypoint",         kModeWaypoint   },
            {"NPC",              kModeNPC        },
            {"Water",            kModeWater      },
            {"Emitter",          kModeEmitters   },
            {"Spawn Point",      kModeSpawnPoint },
        };
        for (auto& e : kEntries) {
            if (ImGui::MenuItem(e.label)) {
                zoneMode_ = e.mode;
                PlaceObject(pendingPlacePos_, db, media);
            }
        }
        ImGui::EndPopup();
    }

    // Keep previous RMB state for press/release edge detection on next frame.
    rmbWasDown_ = rmbDown;
}

// ─── Selected-object transform helpers ──────────────────────────────────────

bool ZonesTab::SelectedPos(glm::vec3& out) const {
    if (selectedID_ < 0) return false;
    auto tryVec = [&](const auto& vec, int st) -> bool {
        if (selectedType_ != st) return false;
        for (auto& o : vec) if (o.id == selectedID_) { out = o.pos; return true; }
        return false;
    };
    if (tryVec(scene_.portals,   kSelPortal))   return true;
    if (tryVec(scene_.colBoxes,   kSelColBox))    return true;
    if (tryVec(scene_.colSpheres, kSelColSphere)) return true;
    if (tryVec(scene_.waypoints, kSelWaypoint)) return true;
    if (tryVec(scene_.npcs,      kSelNpc))      return true;
    if (tryVec(scene_.emitters,  kSelEmitter))  return true;
    if (tryVec(scene_.water,     kSelWater))    return true;
    if (tryVec(scene_.scenery,     kSelScenery))    return true;
    if (tryVec(scene_.spawnPoints, kSelSpawnPoint)) return true;
    if (selectedType_ == kSelTrigger)
        for (auto& t : scene_.triggers) if (t.id == selectedID_) { out = {t.x, 0, t.z}; return true; }
    if (selectedType_ == kSelSoundZone)
        for (auto& s : scene_.soundZones) if (s.id == selectedID_) { out = {s.x, 0, s.z}; return true; }
    return false;
}

void ZonesTab::SetSelectedPos(const glm::vec3& pos) {
    auto trySet = [&](auto& vec, int st) {
        if (selectedType_ != st) return;
        for (auto& o : vec) if (o.id == selectedID_) o.pos = pos;
    };
    trySet(scene_.portals,   kSelPortal);
    trySet(scene_.colBoxes,   kSelColBox);
    trySet(scene_.colSpheres, kSelColSphere);
    trySet(scene_.waypoints, kSelWaypoint);
    trySet(scene_.npcs,      kSelNpc);
    trySet(scene_.emitters,  kSelEmitter);
    trySet(scene_.water,     kSelWater);
    trySet(scene_.scenery,     kSelScenery);
    trySet(scene_.spawnPoints, kSelSpawnPoint);
    if (selectedType_ == kSelTrigger)
        for (auto& t : scene_.triggers)
            if (t.id == selectedID_) { t.x = pos.x; t.z = pos.z; }
    if (selectedType_ == kSelSoundZone)
        for (auto& s : scene_.soundZones)
            if (s.id == selectedID_) { s.x = pos.x; s.z = pos.z; }
}

void ZonesTab::PersistSelectedPos(sqlite3* db) {
    glm::vec3 pos;
    if (!SelectedPos(pos)) return;

    const char* sql = nullptr;
    bool three = true;   // false = 2D tables (x,z only)
    switch (selectedType_) {
    case kSelPortal:    sql = "UPDATE area_portals     SET x=?,z=?     WHERE id=?"; three = false; break;
    case kSelTrigger:   sql = "UPDATE area_triggers    SET x=?,z=?     WHERE id=?"; three = false; break;
    case kSelSoundZone: sql = "UPDATE area_sound_zones SET x=?,z=?     WHERE id=?"; three = false; break;
    case kSelColBox:    sql = "UPDATE zone_colboxes    SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelColSphere: sql = "UPDATE zone_colspheres  SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelWaypoint:  sql = "UPDATE zone_waypoints   SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelNpc:       sql = "UPDATE npc_spawns       SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelEmitter:   sql = "UPDATE zone_emitters    SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelWater:     sql = "UPDATE zone_water       SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelScenery:    sql = "UPDATE zone_scenery  SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelSpawnPoint: sql = "UPDATE spawn_points  SET x=?,y=?,z=? WHERE id=?"; break;
    default: return;
    }

    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) return;
    int idx = 1;
    sqlite3_bind_double(s, idx++, pos.x);
    if (three) sqlite3_bind_double(s, idx++, pos.y);
    sqlite3_bind_double(s, idx++, pos.z);
    sqlite3_bind_int(s, idx, selectedID_);
    sqlite3_step(s);
    sqlite3_finalize(s);

    // Rebuild collision preview when transforms can affect baked collision.
    switch (selectedType_) {
    case kSelScenery:
    case kSelColBox:
    case kSelColSphere:
        scene_.colVisDirty = true;
        break;
    default:
        break;
    }
}

// ─── Rotation helpers ───────────────────────────────────────────────────────
//
// Only scenery carries full Euler rotation. NPCs have a single yaw value —
// we return it as (0, yaw, 0) so the gizmo code can treat rotation uniformly.

bool ZonesTab::SelectedRot(glm::vec3& out) const {
    if (selectedID_ < 0) return false;
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) { out = s.rot; return true; }
    } else if (selectedType_ == kSelNpc) {
        for (auto& n : scene_.npcs) if (n.id == selectedID_) {
            out = glm::vec3(0.f, n.yaw, 0.f); return true;
        }
    }
    return false;
}

void ZonesTab::SetSelectedRot(const glm::vec3& rot) {
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) { s.rot = rot; return; }
    } else if (selectedType_ == kSelNpc) {
        for (auto& n : scene_.npcs) if (n.id == selectedID_) { n.yaw = rot.y; return; }
    }
}

void ZonesTab::PersistSelectedRot(sqlite3* db) {
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_scenery SET pitch=?,yaw=?,roll=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, s.rot.x);
                sqlite3_bind_double(st, 2, s.rot.y);
                sqlite3_bind_double(st, 3, s.rot.z);
                sqlite3_bind_int   (st, 4, s.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            scene_.colVisDirty = true;
            return;
        }
    } else if (selectedType_ == kSelNpc) {
        for (auto& n : scene_.npcs) if (n.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "UPDATE npc_spawns SET yaw=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, n.yaw);
                sqlite3_bind_int   (st, 2, n.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            return;
        }
    }
}

// ─── Scale helpers ──────────────────────────────────────────────────────────
//
// Scenery and ColBox have 3-axis scale. Water has 2-axis (XZ plane) — we
// report .y as 0 and use only x/z on write. Others don't scale — return false.

bool ZonesTab::SelectedScale(glm::vec3& out) const {
    if (selectedID_ < 0) return false;
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) { out = s.scale; return true; }
    } else if (selectedType_ == kSelColBox) {
        for (auto& c : scene_.colBoxes) if (c.id == selectedID_) { out = c.scale; return true; }
    } else if (selectedType_ == kSelColSphere) {
        for (auto& s : scene_.colSpheres) if (s.id == selectedID_) {
            out = glm::vec3(s.radius); return true;
        }
    } else if (selectedType_ == kSelWater) {
        for (auto& w : scene_.water) if (w.id == selectedID_) {
            out = glm::vec3(w.scale.x, 1.f, w.scale.y); return true;
        }
    }
    return false;
}

void ZonesTab::SetSelectedScale(const glm::vec3& s) {
    if (selectedType_ == kSelScenery) {
        for (auto& o : scene_.scenery) if (o.id == selectedID_) { o.scale = s; return; }
    } else if (selectedType_ == kSelColBox) {
        for (auto& c : scene_.colBoxes) if (c.id == selectedID_) { c.scale = s; return; }
    } else if (selectedType_ == kSelColSphere) {
        for (auto& cs : scene_.colSpheres) if (cs.id == selectedID_) { cs.radius = s.x; return; }
    } else if (selectedType_ == kSelWater) {
        for (auto& w : scene_.water) if (w.id == selectedID_) {
            w.scale.x = s.x; w.scale.y = s.z; return;
        }
    }
}

void ZonesTab::PersistSelectedScale(sqlite3* db) {
    if (selectedType_ == kSelScenery) {
        for (auto& s : scene_.scenery) if (s.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_scenery SET sx=?,sy=?,sz=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, s.scale.x);
                sqlite3_bind_double(st, 2, s.scale.y);
                sqlite3_bind_double(st, 3, s.scale.z);
                sqlite3_bind_int   (st, 4, s.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            scene_.colVisDirty = true;
            return;
        }
    } else if (selectedType_ == kSelColBox) {
        for (auto& c : scene_.colBoxes) if (c.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_colboxes SET scale_x=?,scale_y=?,scale_z=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, c.scale.x);
                sqlite3_bind_double(st, 2, c.scale.y);
                sqlite3_bind_double(st, 3, c.scale.z);
                sqlite3_bind_int   (st, 4, c.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            scene_.colVisDirty = true;
            return;
        }
    } else if (selectedType_ == kSelColSphere) {
        for (auto& cs : scene_.colSpheres) if (cs.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_colspheres SET radius=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, cs.radius);
                sqlite3_bind_int   (st, 2, cs.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            scene_.colVisDirty = true;
            return;
        }
    } else if (selectedType_ == kSelWater) {
        for (auto& w : scene_.water) if (w.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_water SET scale_x=?,scale_z=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, w.scale.x);
                sqlite3_bind_double(st, 2, w.scale.y);
                sqlite3_bind_int   (st, 3, w.id);
                sqlite3_step(st); sqlite3_finalize(st);
            }
            return;
        }
    }
}

// ─── FocusOnSelected ─────────────────────────────────────────────────────────

void ZonesTab::FocusOnSelected() {
    glm::vec3 pos;
    if (!SelectedPos(pos)) return;
    cam_.pos   = pos + glm::vec3(0.f, 12.f, 12.f);
    cam_.pitch = 30.f;
    std::snprintf(statusMsg_, sizeof(statusMsg_), "Focused on id=%d.", selectedID_);
}

} // namespace gue
