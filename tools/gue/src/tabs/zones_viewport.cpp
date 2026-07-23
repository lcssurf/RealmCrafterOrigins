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

namespace {

// Builds the same Ry*Rx*Rz compound rotation the renderer uses for scenery
// (rot.x=pitch/X, rot.y=yaw/Y, rot.z=roll/Z — see
// ZoneRenderer's scenery matrix build, m = Ry then Rx then Rz).
glm::mat3 BuildEulerYXZDeg(const glm::vec3& rotDeg) {
    glm::mat4 m(1.f);
    m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
    return glm::mat3(m);
}

// Inverse of BuildEulerYXZDeg: recovers (pitch, yaw, roll) degrees from a
// rotation matrix built as Ry*Rx*Rz. Used after composing a world-space
// gizmo drag delta onto the drag-start matrix, so the result is written back
// into rot.x/y/z consistently with how the renderer will interpret them.
// Degenerates at the pitch = +-90 deg gimbal lock (rare for placed scenery);
// falls back to roll = 0 there, same convention as most DCC tools.
glm::vec3 DecomposeEulerYXZDeg(const glm::mat3& m) {
    float sx = std::clamp(-m[2][1], -1.f, 1.f);
    float pitch = std::asin(sx);
    float yaw, roll;
    const float cx = std::cos(pitch);
    if (cx > 1e-4f) {
        yaw  = std::atan2(m[2][0], m[2][2]);
        roll = std::atan2(m[0][1], m[1][1]);
    } else {
        // Gimbal lock: yaw and roll both rotate around the same resulting
        // axis — only their sum/difference is meaningful. Pin roll to 0.
        yaw  = std::atan2(-m[0][2], m[0][0]);
        roll = 0.f;
    }
    return glm::degrees(glm::vec3(pitch, yaw, roll));
}

} // namespace

// ─── Floating toolbar (overlaid inside viewport) ──────────────────────────────

void ZonesTab::DrawFloatingToolbar() {
    // Keyboard shortcuts (Q/W/E/R, F1-F4, T, Y) — suspended while the dev is
    // actively navigating with a mouse button held (RMB freelook/fly,
    // Alt+LMB orbit, Alt+RMB dolly, MMB pan). WASD (and Q/E/R specifically)
    // double as camera-fly keys during RMB freelook (see the "Freelook (RMB
    // held)" block above in DrawViewport), so leaving these tool shortcuts
    // live during navigation meant pressing W to fly forward also silently
    // swapped the active gizmo tool to Move. All of mouseLook_/altOrbit_/
    // altDolly_/mmbPan_ are resolved earlier in DrawViewport (this function
    // is called from partway through it), so they're already up to date for
    // this frame. See docs/TECH_DEBT.md "Viewport input priority chain".
    const bool navigating = mouseLook_ || altOrbit_ || altDolly_ || mmbPan_;
    if (vpHovered_ && !navigating) {
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

    // ── Terrain / Foliage brush cursor: hover raycast + circle overlay ────
    const bool foliageBrushMode = (zoneMode_ == kModeFoliage);
    if ((zoneMode_ == kModeTerrain || foliageBrushMode) && renderer_.terrain().Loaded()) {
        const float curBrushRadius = foliageBrushMode ? foliageRadius_ : brushRadius_;
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
            const bool foliageEraseHint = foliageBrushMode &&
                (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift));

            // Project a world-space ring onto screen space
            static constexpr int kSeg = 48;
            static constexpr float kPi2 = 6.28318530f;
            ImVec2 pts[kSeg + 1];
            int ptCount = 0;
            for (int i = 0; i <= kSeg; ++i) {
                float a = (float)i / kSeg * kPi2;
                glm::vec4 wp = {
                    brushHitPos_.x + std::cos(a) * curBrushRadius,
                    brushHitPos_.y + 0.25f,   // slight lift to avoid z-fighting
                    brushHitPos_.z + std::sin(a) * curBrushRadius,
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
                ImU32 ringCol = foliageEraseHint
                    ? IM_COL32(255, 80, 80, 230)    // red = foliage erase (Shift held)
                    : foliageBrushMode
                        ? IM_COL32(110, 235, 120, 230)  // green = foliage paint
                        : (brushMode_ == 4)
                            ? IM_COL32(255, 215, 50, 230)   // yellow for terrain paint
                            : IM_COL32(255, 255, 255, 230); // white for terrain sculpt
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
                ImU32 dotCol = foliageEraseHint
                    ? IM_COL32(255, 80, 80, 230)
                    : foliageBrushMode
                        ? IM_COL32(110, 235, 120, 230)
                        : (brushMode_ == 4) ? IM_COL32(255,215,50,230) : IM_COL32(255,255,255,230);
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
         zoneMode_ != kModeTerrain &&
         zoneMode_ != kModeFoliage);
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

    // ── Shared object raycast (selection hit-test + hover highlight) ───────
    // Ray-sphere / ray-AABB test against every pickable scene category.
    // Used BOTH by the click arbiter below (does this click land on an
    // object?) and, every frame, to drive a subtle "object under cursor"
    // hover highlight (see item 3) — one raycast serves both, since a click
    // frame's cursor position and that same frame's hover position are
    // identical.
    auto raycastObjects = [&](int& outID, int& outType) {
        outID = -1; outType = kSelNone;
        if (!vpHovered_) return;
        ImVec2 mp = ImGui::GetMousePos();
        float vpX = mp.x - vpOrigin_.x;
        float vpY = mp.y - vpOrigin_.y;
        float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
        float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
        float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
        glm::vec3 orig = cam_.pos;
        glm::vec3 dir  = cam_.NDCRay(ndcX, ndcY, aspect);

        auto raySphere = [](const glm::vec3& ro, const glm::vec3& rd,
                            const glm::vec3& center, float r) -> float {
            glm::vec3 oc = ro - center;
            float b = glm::dot(oc, rd);
            float c = glm::dot(oc, oc) - r*r;
            float disc = b*b - c;
            if (disc < 0.f) return -1.f;
            return -b - std::sqrt(disc);
        };
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
        for (auto& p : scene_.portals) {
            if (scene_.IsHidden(kSelPortal, p.id)) continue;
            glm::vec3 c = {p.pos.x, p.radius, p.pos.z};
            float t = raySphere(orig, dir, c, p.radius);
            if (t > 0.f && t < bestT) { bestT = t; outID = p.id; outType = kSelPortal; }
        }
        for (auto& t : scene_.triggers) {
            if (scene_.IsHidden(kSelTrigger, t.id)) continue;
            float tt = raySphere(orig, dir, {t.x, 0.f, t.z}, t.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = t.id; outType = kSelTrigger; }
        }
        for (auto& s : scene_.soundZones) {
            if (scene_.IsHidden(kSelSoundZone, s.id)) continue;
            float tt = raySphere(orig, dir, {s.x, 0.f, s.z}, s.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = s.id; outType = kSelSoundZone; }
        }
        for (auto& c : scene_.colBoxes) {
            if (scene_.IsHidden(kSelColBox, c.id)) continue;
            glm::vec3 half = c.scale * 0.5f;
            float tt = rayAABB(orig, dir, c.pos - half, c.pos + half);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = c.id; outType = kSelColBox; }
        }
        for (auto& s : scene_.colSpheres) {
            if (scene_.IsHidden(kSelColSphere, s.id)) continue;
            float tt = raySphere(orig, dir, s.pos, s.radius);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = s.id; outType = kSelColSphere; }
        }
        for (auto& w : scene_.waypoints) {
            if (scene_.IsHidden(kSelWaypoint, w.id)) continue;
            float tt = raySphere(orig, dir, w.pos, 0.8f);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = w.id; outType = kSelWaypoint; }
        }
        for (auto& n : scene_.npcs) {
            if (scene_.IsHidden(kSelNpc, n.id)) continue;
            glm::vec3 mn = n.pos - glm::vec3(0.35f, 0.f, 0.35f);
            glm::vec3 mx = n.pos + glm::vec3(0.35f, 1.0f, 0.35f);
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = n.id; outType = kSelNpc; }
        }
        for (auto& e : scene_.emitters) {
            if (scene_.IsHidden(kSelEmitter, e.id)) continue;
            glm::vec3 mn = e.pos - glm::vec3(0.4f, 0.f, 0.4f);
            glm::vec3 mx = e.pos + glm::vec3(0.4f, 1.5f, 0.4f);
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = e.id; outType = kSelEmitter; }
        }
        for (auto& w : scene_.water) {
            if (scene_.IsHidden(kSelWater, w.id)) continue;
            glm::vec3 mn = {w.pos.x - w.scale.x*0.5f, w.pos.y - 0.1f, w.pos.z - w.scale.y*0.5f};
            glm::vec3 mx = {w.pos.x + w.scale.x*0.5f, w.pos.y + 0.2f, w.pos.z + w.scale.y*0.5f};
            float tt = rayAABB(orig, dir, mn, mx);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = w.id; outType = kSelWater; }
        }
        for (auto& s : scene_.scenery) {
            if (scene_.IsHidden(kSelScenery, s.id)) continue;
            // s.scale is a MESH-SCALE MULTIPLIER (often far from 1 — many
            // models here use 0.02), not a world-space box size — using it
            // directly as a box (the old code) made the hit-test box tiny
            // for any model authored at a different native scale, which is
            // why scenery was unselectable while lights/water (whose scale
            // fields genuinely ARE world-space sizes) worked fine. Get the
            // model's real local-space bind-pose bounds and transform them
            // by the object's full pos/rot/scale (same matrix used to draw
            // it — see ZoneRenderer::RenderFramePBR_'s scenery loop).
            glm::vec3 lmn, lmx;
            if (renderer_.SceneryModelLocalBounds(s.id, lmn, lmx)) {
                glm::mat4 m = glm::translate(glm::mat4(1.f), s.pos);
                m = glm::rotate(m, glm::radians(s.rot.y), {0.f, 1.f, 0.f});
                m = glm::rotate(m, glm::radians(s.rot.x), {1.f, 0.f, 0.f});
                m = glm::rotate(m, glm::radians(s.rot.z), {0.f, 0.f, 1.f});
                m = glm::scale(m, s.scale);
                // Transform all 8 local corners and take the resulting
                // world AABB. The model's true rotated bounds are an
                // oriented box (OBB); this AABB is a conservative superset
                // of it — never smaller than the visible model, at most a
                // bit generous on a diagonal rotation — which is the right
                // trade-off for a click hit-test versus a full ray/OBB
                // intersection. See docs/TECH_DEBT.md "Viewport input
                // priority chain".
                glm::vec3 wmn(1e9f), wmx(-1e9f);
                for (int i = 0; i < 8; ++i) {
                    glm::vec3 corner((i & 1) ? lmx.x : lmn.x,
                                      (i & 2) ? lmx.y : lmn.y,
                                      (i & 4) ? lmx.z : lmn.z);
                    glm::vec3 wc = glm::vec3(m * glm::vec4(corner, 1.f));
                    wmn = glm::min(wmn, wc);
                    wmx = glm::max(wmx, wc);
                }
                float tt = rayAABB(orig, dir, wmn, wmx);
                if (tt > 0.f && tt < bestT) { bestT = tt; outID = s.id; outType = kSelScenery; }
            } else {
                // Model not loaded yet (or no actor) — fall back to the old
                // approximation so the object isn't completely unselectable
                // while it streams in, clamped to a sane minimum.
                glm::vec3 half = glm::max(glm::abs(s.scale) * 0.5f, glm::vec3(0.05f));
                float tt = rayAABB(orig, dir, s.pos - half, s.pos + half);
                if (tt > 0.f && tt < bestT) { bestT = tt; outID = s.id; outType = kSelScenery; }
            }
        }
        for (auto& l : scene_.lights) {
            if (scene_.IsHidden(kSelLight, l.id)) continue;
            float tt = raySphere(orig, dir, l.pos, 0.3f);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = l.id; outType = kSelLight; }
        }
        // Atmosphere volumes — same AABB-hit-test convention as colBoxes
        // (pos ± size/2), regardless of `shape` (sphere volumes still get a
        // box hit-test for editor click purposes — see docs/TECH_DEBT.md
        // "Atmosphere volumes").
        for (auto& v : scene_.atmosphereVolumes) {
            if (scene_.IsHidden(kSelAtmosphereVolume, v.id)) continue;
            glm::vec3 half = glm::max(glm::abs(v.size) * 0.5f, glm::vec3(0.05f));
            float tt = rayAABB(orig, dir, v.pos - half, v.pos + half);
            if (tt > 0.f && tt < bestT) { bestT = tt; outID = v.id; outType = kSelAtmosphereVolume; }
        }
    };

    // scnArmed: a scenery model is picked and ready to place on LMB click.
    // Computed here (rather than down by the placement block) so the click
    // arbiter below can see it.
    const bool scnArmed = (zoneMode_ == kModeScenery && scnModelId_ != 0
                           && xformMode_ == kXFormSelect && !mouseLook_);

    // paintToolActive mirrors the terrain/foliage brush blocks' OWN outer
    // gate exactly (zoneMode_ + !mouseLook_ + terrain loaded). Hoisted up
    // here (used to live only down by the click arbiter, further below) so
    // BOTH the click arbiter AND the object-hover suppression right below
    // read the exact same condition — reused, not duplicated. See
    // docs/TECH_DEBT.md "Viewport input priority chain".
    const bool paintToolActive = (zoneMode_ == kModeTerrain || zoneMode_ == kModeFoliage) &&
                                  !mouseLook_ && renderer_.terrain().Loaded();

    // Object hover — every frame, throttled to every-other-frame except on
    // an actual click (which always needs a same-frame-fresh result for the
    // arbiter below, so a click landing directly on an object still resolves
    // to Selection even while paintToolActive — see the arbiter's own
    // priority order: Selection outranks the zoneMode_ tool there). Cost:
    // one ray-sphere/ray-AABB test per pickable object in the scene, no
    // spatial structure — trivially cheap at ordinary GUE zone sizes (tens
    // to low hundreds of objects); the throttle exists so a very large zone
    // doesn't pay for it on every single render frame for a purely cosmetic
    // highlight.
    //
    // While a paint tool is active, the HOVER-ONLY refresh (not the
    // click-frame one) is suppressed outright: no object hover, no outline,
    // regardless of whether the cursor happens to be over an object — the
    // dev's whole mental model while sculpting/painting is "I'm painting",
    // and a highlight flickering under the brush cursor was pure noise. This
    // is a deliberately SIMPLER rule than the full click arbiter (which
    // still lets a literal click on an object win) — it only governs the
    // passive visual feedback, not what a click actually resolves to.
    const bool lmbClickedNow = vpHovered_ && ImGui::IsMouseClicked(0) && !altDown;
    int rcID = hoverObjID_, rcType = hoverObjType_;
    if (!vpHovered_ || (paintToolActive && !lmbClickedNow)) {
        rcID = -1; rcType = kSelNone;
        hoverObjID_ = -1; hoverObjType_ = kSelNone;
    } else if (lmbClickedNow || (hoverThrottleCounter_ % 2) == 0) {
        raycastObjects(rcID, rcType);
        hoverObjID_   = rcID;
        hoverObjType_ = rcType;
    }
    ++hoverThrottleCounter_;

    // World-space AABB lookup shared by the hover outline below. Approach
    // chosen: (a) screen-space wireframe box — project the object's world
    // AABB corners and draw the 12 edges as an ImGui overlay, same
    // screen-space-overlay technique as the terrain brush cursor above. NOT
    // (b) a true 3D silhouette outline (stencil/edge-detect render pass) —
    // that needs a new render pass wired into the deferred pipeline (extra
    // FBO/shader, depth-aware edge detection or a stencil-mask-and-outline
    // draw), real effort for a marginal usability gain over projecting the
    // box: the box already tells you unambiguously "this is the object under
    // the cursor" and follows its rotation/position/size, which is all this
    // needs to do. Reuses the same scenery real-model-bounds fix as the
    // raycast above (see docs/TECH_DEBT.md "Viewport input priority chain",
    // bug 126.1) instead of the flawed scale-as-size shortcut.
    auto worldAABBFor = [&](int type, int id, glm::vec3& outMin, glm::vec3& outMax) -> bool {
        switch (type) {
        case kSelPortal:
            for (auto& o : scene_.portals) if (o.id == id) {
                float r = std::max(0.05f, o.radius);
                glm::vec3 c{o.pos.x, o.radius, o.pos.z};
                outMin = c - glm::vec3(r); outMax = c + glm::vec3(r); return true;
            }
            break;
        case kSelTrigger:
            for (auto& o : scene_.triggers) if (o.id == id) {
                float r = std::max(0.05f, o.radius);
                glm::vec3 c{o.x, 0.f, o.z};
                outMin = c - glm::vec3(r, 0.6f, r); outMax = c + glm::vec3(r, 0.6f, r); return true;
            }
            break;
        case kSelSoundZone:
            for (auto& o : scene_.soundZones) if (o.id == id) {
                float r = std::max(0.05f, o.radius);
                glm::vec3 c{o.x, 0.f, o.z};
                outMin = c - glm::vec3(r, 0.6f, r); outMax = c + glm::vec3(r, 0.6f, r); return true;
            }
            break;
        case kSelColBox:
            for (auto& o : scene_.colBoxes) if (o.id == id) {
                glm::vec3 h = glm::max(glm::abs(o.scale) * 0.5f, glm::vec3(0.05f));
                outMin = o.pos - h; outMax = o.pos + h; return true;
            }
            break;
        case kSelColSphere:
            for (auto& o : scene_.colSpheres) if (o.id == id) {
                float r = std::max(0.05f, o.radius);
                outMin = o.pos - glm::vec3(r); outMax = o.pos + glm::vec3(r); return true;
            }
            break;
        case kSelWaypoint:
            for (auto& o : scene_.waypoints) if (o.id == id) {
                outMin = o.pos - glm::vec3(0.5f); outMax = o.pos + glm::vec3(0.5f); return true;
            }
            break;
        case kSelNpc:
            for (auto& o : scene_.npcs) if (o.id == id) {
                outMin = o.pos - glm::vec3(0.35f, 0.f, 0.35f);
                outMax = o.pos + glm::vec3(0.35f, 1.f, 0.35f); return true;
            }
            break;
        case kSelEmitter:
            for (auto& o : scene_.emitters) if (o.id == id) {
                outMin = o.pos - glm::vec3(0.4f, 0.f, 0.4f);
                outMax = o.pos + glm::vec3(0.4f, 1.5f, 0.4f); return true;
            }
            break;
        case kSelWater:
            for (auto& o : scene_.water) if (o.id == id) {
                outMin = {o.pos.x - o.scale.x * 0.5f, o.pos.y - 0.1f, o.pos.z - o.scale.y * 0.5f};
                outMax = {o.pos.x + o.scale.x * 0.5f, o.pos.y + 0.2f, o.pos.z + o.scale.y * 0.5f};
                return true;
            }
            break;
        case kSelLight:
            for (auto& o : scene_.lights) if (o.id == id) {
                outMin = o.pos - glm::vec3(0.3f); outMax = o.pos + glm::vec3(0.3f); return true;
            }
            break;
        case kSelAtmosphereVolume:
            for (auto& o : scene_.atmosphereVolumes) if (o.id == id) {
                glm::vec3 h = glm::max(glm::abs(o.size) * 0.5f, glm::vec3(0.05f));
                outMin = o.pos - h; outMax = o.pos + h; return true;
            }
            break;
        case kSelScenery:
            for (auto& o : scene_.scenery) if (o.id == id) {
                glm::vec3 lmn, lmx;
                if (renderer_.SceneryModelLocalBounds(o.id, lmn, lmx)) {
                    glm::mat4 m = glm::translate(glm::mat4(1.f), o.pos);
                    m = glm::rotate(m, glm::radians(o.rot.y), {0.f, 1.f, 0.f});
                    m = glm::rotate(m, glm::radians(o.rot.x), {1.f, 0.f, 0.f});
                    m = glm::rotate(m, glm::radians(o.rot.z), {0.f, 0.f, 1.f});
                    m = glm::scale(m, o.scale);
                    glm::vec3 wmn(1e9f), wmx(-1e9f);
                    for (int i = 0; i < 8; ++i) {
                        glm::vec3 corner((i & 1) ? lmx.x : lmn.x,
                                          (i & 2) ? lmx.y : lmn.y,
                                          (i & 4) ? lmx.z : lmn.z);
                        glm::vec3 wc = glm::vec3(m * glm::vec4(corner, 1.f));
                        wmn = glm::min(wmn, wc); wmx = glm::max(wmx, wc);
                    }
                    outMin = wmn; outMax = wmx;
                } else {
                    glm::vec3 h = glm::max(glm::abs(o.scale) * 0.5f, glm::vec3(0.05f));
                    outMin = o.pos - h; outMax = o.pos + h;
                }
                return true;
            }
            break;
        default: break;
        }
        return false;
    };

    // Draws the 12 edges of a world-space AABB projected to screen — used
    // for the hover outline (subtle: thin, low-opacity) below, and reusable
    // later for a bolder "selected" outline if this object type doesn't
    // already get one in the 3D scene (portals/triggers/colboxes/etc. do,
    // via ZoneRenderer::RenderFramePBR_'s tinted debug primitives; scenery
    // relies on the gizmo itself as its "selected" indicator).
    auto drawWorldBoxOutline = [&](const glm::vec3& mn, const glm::vec3& mx,
                                    ImU32 colOutline, ImU32 colShadow, float thickness) {
        glm::mat4 vpMat = cam_.Proj(vpSize_.x / std::max(vpSize_.y, 1.f)) * cam_.View();
        glm::vec3 corners[8];
        for (int i = 0; i < 8; ++i)
            corners[i] = glm::vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
        ImVec2 pts[8]; bool valid[8];
        for (int i = 0; i < 8; ++i) {
            glm::vec4 clip = vpMat * glm::vec4(corners[i], 1.f);
            valid[i] = clip.w > 0.0001f;
            if (valid[i]) {
                clip /= clip.w;
                pts[i] = {vpOrigin_.x + (clip.x * 0.5f + 0.5f) * vpSize_.x,
                          vpOrigin_.y + (-clip.y * 0.5f + 0.5f) * vpSize_.y};
            }
        }
        static const int kEdges[12][2] = {
            {0,1},{0,2},{0,4},{1,3},{1,5},{2,3},
            {2,6},{3,7},{4,5},{4,6},{5,7},{6,7},
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (auto& e : kEdges) {
            if (!valid[e[0]] || !valid[e[1]]) continue;
            dl->AddLine(pts[e[0]], pts[e[1]], colShadow, thickness + 2.f);
        }
        for (auto& e : kEdges) {
            if (!valid[e[0]] || !valid[e[1]]) continue;
            dl->AddLine(pts[e[0]], pts[e[1]], colOutline, thickness);
        }
    };

    // Screen-space bounding rect of an object's projected world AABB — used
    // by the marquee multi-select below (item 2: selects by projected AABB
    // overlap, not just the object's centre point, matching how most
    // editors' rectangle-select behave — an object partially inside the
    // drag rectangle still gets picked up).
    auto screenRectFor = [&](int type, int id, ImVec2& outMin, ImVec2& outMax) -> bool {
        glm::vec3 mn, mx;
        if (!worldAABBFor(type, id, mn, mx)) return false;
        glm::mat4 vpMat = cam_.Proj(vpSize_.x / std::max(vpSize_.y, 1.f)) * cam_.View();
        ImVec2 smn(1e9f, 1e9f), smx(-1e9f, -1e9f);
        bool any = false;
        for (int i = 0; i < 8; ++i) {
            glm::vec3 corner((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
            glm::vec4 clip = vpMat * glm::vec4(corner, 1.f);
            if (clip.w <= 0.0001f) continue;
            clip /= clip.w;
            float sx = vpOrigin_.x + (clip.x * 0.5f + 0.5f) * vpSize_.x;
            float sy = vpOrigin_.y + (-clip.y * 0.5f + 0.5f) * vpSize_.y;
            smn.x = std::min(smn.x, sx); smn.y = std::min(smn.y, sy);
            smx.x = std::max(smx.x, sx); smx.y = std::max(smx.y, sy);
            any = true;
        }
        if (!any) return false;
        outMin = smn; outMax = smx;
        return true;
    };

    // Subtle hover outline — skipped while something is already selected the
    // same way (the gizmo/selection's own feedback already covers that
    // object) or while dragging the gizmo/navigating. Deliberately thinner
    // and less opaque than a "selected" outline would be, so the two never
    // read as the same strength of feedback (see item 3 of this change).
    if (rcID >= 0 && !mouseLook_ && gizmoAxis_ < 0 &&
        !(rcID == selectedID_ && rcType == selectedType_)) {
        glm::vec3 mn, mx;
        if (worldAABBFor(rcType, rcID, mn, mx)) {
            drawWorldBoxOutline(mn, mx, IM_COL32(255, 220, 90, 165), IM_COL32(0, 0, 0, 90), 1.25f);
        }
    }

    // Bold "selected" outline — scenery only. Every OTHER selectable type
    // already gets its own strong selected-state feedback drawn straight
    // into the 3D scene (a red/white tint on its debug sphere/box/circle —
    // see ZoneRenderer::RenderFramePBR_'s per-type `sel` branches), but
    // scenery (an actual G-buffer-rendered mesh, not a debug primitive) has
    // none — its only prior "you selected this" feedback was the gizmo
    // itself, which doesn't appear while xformMode_ is still Select. Clearly
    // bolder than the hover outline above (thicker, more opaque, different
    // hue) so the two can never be mistaken for each other.
    if (selectedType_ == kSelScenery && selectedID_ >= 0) {
        glm::vec3 mn, mx;
        if (worldAABBFor(kSelScenery, selectedID_, mn, mx)) {
            drawWorldBoxOutline(mn, mx, IM_COL32(255, 255, 255, 235), IM_COL32(0, 0, 0, 150), 2.5f);
        }
    }

    // ── Unified LMB click-priority arbiter ──────────────────────────────
    // Decided once per press, held for the whole gesture. Priority: gizmo
    // handle > new-object placement > object selection (only if the ray
    // actually hit something) > the active zoneMode_ tool (terrain/foliage
    // brush, only while it would actually paint something) > marquee
    // multi-select (lowest priority — only when NOTHING else claimed the
    // click). Before this, each of those blocks tested only its own slice
    // of state (xformMode_ / zoneMode_ / scnArmed) independently, so e.g.
    // sculpting terrain (zoneMode_==kModeTerrain) and selecting an object
    // (previously gated only on xformMode_==kXFormSelect, which defaults to
    // true and nothing about entering Terrain mode changes it) could both
    // fire on the very same click. See docs/TECH_DEBT.md "Viewport input
    // priority chain".
    //
    // paintToolActive itself is computed earlier now (hoisted up alongside
    // the object-hover suppression above — see that comment) — an empty
    // click while Terrain/Foliage mode is active but the brush itself
    // wouldn't do anything (e.g. no terrain loaded yet) correctly falls
    // through to marquee instead of being silently swallowed as kLmbOwnerTool.
    if (lmbClickedNow) {
        if (xformMode_ != kXFormSelect && gizmoHoverAxis_ >= 0 && !mouseLook_) {
            lmbGestureOwner_ = kLmbOwnerGizmo;
        } else if (scnArmed) {
            lmbGestureOwner_ = kLmbOwnerPlacement;
        } else if (rcID >= 0) {
            lmbGestureOwner_ = kLmbOwnerSelection;
        } else if (paintToolActive) {
            lmbGestureOwner_ = kLmbOwnerTool;
        } else {
            lmbGestureOwner_ = kLmbOwnerMarquee;
        }
    } else if (!ImGui::IsMouseDown(0)) {
        lmbGestureOwner_ = kLmbOwnerNone;
    }

    // ── Marquee (rectangle) multi-select ────────────────────────────────
    // Only starts when the click arbiter above found nothing else to claim
    // it (lowest priority) — an empty click in Terrain/Foliage mode still
    // goes to the brush (paintToolActive), never the marquee.
    if (lmbClickedNow && lmbGestureOwner_ == kLmbOwnerMarquee) {
        marqueeActive_ = true;
        marqueeStart_  = ImGui::GetMousePos();
    }
    if (marqueeActive_) {
        ImVec2 cur  = ImGui::GetMousePos();
        ImVec2 rMin = {std::min(marqueeStart_.x, cur.x), std::min(marqueeStart_.y, cur.y)};
        ImVec2 rMax = {std::max(marqueeStart_.x, cur.x), std::max(marqueeStart_.y, cur.y)};

        if (ImGui::IsMouseDown(0)) {
            // Same screen-space-overlay technique as the brush cursor/hover
            // outline above — a translucent fill plus a crisp border.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(rMin, rMax, IM_COL32(90, 160, 255, 35));
            dl->AddRect(rMin, rMax, IM_COL32(130, 195, 255, 220), 0.f, 0, 1.5f);
        } else {
            // Released. A near-zero-area drag (a plain click that happened
            // to land on empty space) is treated as a no-op here — it still
            // falls through to the existing "empty click clears selection"
            // logic further down, unaffected by this block.
            marqueeActive_ = false;
            constexpr float kMinDragPx = 3.f;
            if ((rMax.x - rMin.x) >= kMinDragPx || (rMax.y - rMin.y) >= kMinDragPx) {
                // Standard editor modifiers: plain drag replaces the
                // selection, Shift adds to it, Ctrl removes from it.
                const bool addMode    = shiftDown;
                const bool removeMode = !shiftDown && ctrlSelect;

                struct MarqueeHit { int type; int id; };
                std::vector<MarqueeHit> hits;
                auto collect = [&](auto& vec, int type) {
                    for (auto& o : vec) {
                        ImVec2 smn, smx;
                        if (!screenRectFor(type, o.id, smn, smx)) continue;
                        if (smx.x < rMin.x || smn.x > rMax.x ||
                            smx.y < rMin.y || smn.y > rMax.y) continue;
                        hits.push_back({type, o.id});
                    }
                };
                collect(scene_.portals,    kSelPortal);
                collect(scene_.triggers,   kSelTrigger);
                collect(scene_.soundZones, kSelSoundZone);
                collect(scene_.colBoxes,   kSelColBox);
                collect(scene_.colSpheres, kSelColSphere);
                collect(scene_.waypoints,  kSelWaypoint);
                collect(scene_.npcs,       kSelNpc);
                collect(scene_.emitters,   kSelEmitter);
                collect(scene_.water,      kSelWater);
                collect(scene_.scenery,    kSelScenery);
                collect(scene_.lights,     kSelLight);
                collect(scene_.atmosphereVolumes, kSelAtmosphereVolume);

                if (!addMode && !removeMode) ClearSelection();
                for (auto& h : hits) {
                    if (removeMode) RemoveSelection(h.type, h.id);
                    else            AddSelection(h.type, h.id, false);
                }
                if (!hits.empty()) {
                    // Same auto-switch-to-Move rule as a single click (item 2
                    // of the previous pass) — only from Select, never
                    // overriding Rotate/Scale the dev chose on purpose.
                    if (xformMode_ == kXFormSelect) xformMode_ = kXFormMove;
                    std::snprintf(statusMsg_, sizeof(statusMsg_),
                                  "%s %d object(s) via marquee.",
                                  removeMode ? "Removed" : (addMode ? "Added" : "Selected"),
                                  (int)hits.size());
                }
            }
        }
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

    // Selection — LMB click, ANY xform mode (Unreal/Unity-style: clicking an
    // object always selects it, whichever gizmo tool is active) — only when
    // the click arbiter above decided this click belongs to selection (i.e.
    // neither the gizmo nor placement claimed it). Reuses the hit-test result
    // (rcID/rcType) from the shared raycastObjects() call above instead of
    // re-running the raycast.
    if (lmbGestureOwner_ == kLmbOwnerSelection) {
        int bestID = rcID, bestType = rcType;
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
            // Same Ctrl/Shift rules as the scene sidebar (zones_sidebar.cpp
            // DrawGroup, ~line 182-185: `if (ctrlSelect) ToggleSelection(...)
            // else SelectSingle(...)`), reusing the exact same
            // ToggleSelection/AddSelection/SelectSingle calls rather than
            // reimplementing the semantics here — so the two input paths
            // can't quietly drift apart over time. Ctrl = toggle just this
            // object, leaving the rest of the selection alone (identical to
            // the sidebar). Shift = additive (the sidebar's scenery-only
            // Shift range-select has no viewport equivalent — there's no
            // "visual order" between two objects clicked in 3D space — so
            // plain "add to selection" is the closest meaningful mirror of
            // what Shift means there: extend the selection rather than
            // replace it).
            if (ctrlSelect) {
                ToggleSelection(bestType, bestID);
            } else if (shiftDown) {
                AddSelection(bestType, bestID, /*makePrimary=*/true);
            } else {
                SelectSingle(bestType, bestID);
                // Auto-switch to Move on selection (Unreal-style: the move
                // gizmo appears immediately) — but ONLY on a plain click that
                // starts a NEW selection, and only from Select. Ctrl/Shift
                // clicks are extending/toggling an existing multi-selection,
                // not "I just picked something new to work on" — forcing a
                // tool swap there would be surprising, exactly what this
                // whole input-priority-chain pass has been trying to remove.
                // If the dev is already in Rotate or Scale, leave it alone
                // regardless: they picked that tool on purpose. See
                // docs/TECH_DEBT.md "Viewport input priority chain".
                if (xformMode_ == kXFormSelect) {
                    xformMode_ = kXFormMove;
                }
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
            case kSelLight:     zoneMode_ = kModeLight;     break;
            case kSelAtmosphereVolume: zoneMode_ = kModeAtmosphereVolume; break;
            default: break;
            }
        }
    } else if (lmbClickedNow && rcID < 0 && !ctrlSelect && !shiftDown &&
               lmbGestureOwner_ != kLmbOwnerGizmo && lmbGestureOwner_ != kLmbOwnerPlacement &&
               lmbGestureOwner_ != kLmbOwnerMarquee) {
        // Clicking empty space still clears the current selection (existing
        // UX, preserved) — this is a side effect, not a competing "action",
        // so it doesn't need to go through the owner gate above: it only
        // runs when nothing else claimed the click AND the raycast found no
        // object, i.e. exactly the fall-through-to-tool case. Excludes
        // kLmbOwnerMarquee: a marquee drag decides add/replace/remove itself
        // at release (Shift/Ctrl-aware) — clearing here on the PRESS frame
        // would wipe out the existing selection before a Shift+drag ever
        // got a chance to add to it. Also skipped with Ctrl OR Shift held
        // (missing the object and hitting empty space by accident shouldn't
        // blow away an existing multi-selection when a modifier signals the
        // dev meant to add/toggle, not replace).
        ClearSelection();
    }

    // ── Terrain brush (LMB-drag in Terrain mode) ─────────────────────────
    // Gated on lmbGestureOwner_==kLmbOwnerTool (lowest priority in the click
    // arbiter above) so a click that grabbed the gizmo, placed an object, or
    // selected something else this gesture does NOT also sculpt/paint.
    if (zoneMode_ == kModeTerrain && !mouseLook_ && renderer_.terrain().Loaded()) {
        if (vpHovered_ && ImGui::IsMouseDown(0) && lmbGestureOwner_ == kLmbOwnerTool) {
            // Capture one snapshot at the start of each brush stroke
            if (!terrainStrokeActive_) {
                terrainStrokeActive_ = true;
                TerrainSnapshot snap;
                snap.heights = renderer_.terrain().heightmap().heights;
                snap.splat   = renderer_.terrain().splatmap().AllData();
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
                                              (BrushFalloff)brushFalloff_,
                                              (BrushShape)brushShape_,
                                              brushHardness_, brushNoise_,
                                              brushErase_, brushMaxOpacity_);
                } else if (brushMode_ == 6) {
                    // UI index 6 = Splat Smooth — blurs painted weights,
                    // paralleling BrushMode::Smooth (index 2) for the
                    // heightmap. Not a BrushMode value: it operates on the
                    // splatmap, not the heightmap, via SmoothPaint.
                    renderer_.terrain().SmoothPaint(hit.x, hit.z, brushRadius_,
                                                    brushStrength_, dt,
                                                    (BrushFalloff)brushFalloff_,
                                                    (BrushShape)brushShape_,
                                                    brushHardness_, brushNoise_);
                } else {
                    // UI index 5 = Noise → BrushMode::Noise (enum value 4)
                    BrushMode bm = (brushMode_ == 5) ? BrushMode::Noise
                                                      : (BrushMode)brushMode_;
                    renderer_.terrain().ApplyBrush(hit.x, hit.z, brushRadius_,
                                                   brushStrength_, dt, bm,
                                                   brushFlattenH_,
                                                   (BrushFalloff)brushFalloff_,
                                                   (BrushShape)brushShape_,
                                                   brushHardness_, brushNoise_);
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

    // ── Foliage brush (LMB-drag in Foliage mode; Shift+LMB erases) ────────
    // Same owner gate as the terrain brush above.
    if (zoneMode_ == kModeFoliage && !mouseLook_ && renderer_.terrain().Loaded()) {
        if (vpHovered_ && ImGui::IsMouseDown(0) && lmbGestureOwner_ == kLmbOwnerTool) {
            ImVec2 mp  = ImGui::GetMousePos();
            float  vpX = mp.x - vpOrigin_.x;
            float  vpY = mp.y - vpOrigin_.y;
            float ndcX =  (vpX / vpSize_.x) * 2.f - 1.f;
            float ndcY = -((vpY / vpSize_.y) * 2.f - 1.f);
            float aspect = vpSize_.x / std::max(vpSize_.y, 1.f);
            glm::vec3 hit;
            if (renderer_.terrain().Raycast(cam_.pos, cam_.NDCRay(ndcX, ndcY, aspect), hit)) {
                bool erase = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                ApplyFoliageBrush(db, media, hit, dt, erase);
                foliageActive_ = true;
            }
        } else if (ImGui::IsMouseReleased(0)) {
            foliageActive_ = false;
            foliagePlaceAccum_ = 0.f;  // don't carry a stale budget into the next stroke
        }
        // Radius shortcuts (same keys as terrain brush)
        if (vpHovered_) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))  foliageRadius_ = std::max(1.f,  foliageRadius_ - 1.f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) foliageRadius_ = std::min(60.f, foliageRadius_ + 1.f);
        }
    }

    // Delete key
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        DeleteSelected(db);

    // F11 — debug dump: appends one report to terrain_debug_dump.txt in the
    // GUE's working directory — splatmap weights + material albedo array
    // (8 points/layer, texture-space) PLUS the actual rendered G-buffer
    // (gAlbedo/gNormal/gRMA at viewport center + the mouse cursor's current
    // pixel, screen-space) so weight vs. visual result can be compared.
    // Fixed 8-percentage screen points were unreliable (could land on sky,
    // not terrain) — center is a much safer bet, and pointing the mouse at
    // the exact visually-gray spot before pressing F11 lets the dev sample
    // that precise pixel. Investigation tool for the Phase 1 multi-material
    // terrain work — see docs/TECH_DEBT.md "Terrain multi-material
    // authoring (Phase 1)".
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && renderer_.terrain().Loaded()) {
        glm::ivec2 mousePx = {-1, -1};
        if (vpHovered_) {
            ImVec2 mp = ImGui::GetMousePos();
            mousePx = { (int)(mp.x - vpOrigin_.x), (int)(mp.y - vpOrigin_.y) };
        }
        renderer_.DumpDebugReport("terrain_debug_dump.txt", mousePx);
    }

    // Ctrl+Z — undo (terrain undo when in terrain mode, scene undo otherwise)
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (zoneMode_ == kModeTerrain && !terrainUndo_.empty()) {
            TerrainSnapshot cur;
            cur.heights = renderer_.terrain().heightmap().heights;
            cur.splat   = renderer_.terrain().splatmap().AllData();
            terrainRedo_.push_back(std::move(cur));
            auto& prev = terrainUndo_.back();
            renderer_.terrain().heightmap().heights = prev.heights;
            renderer_.terrain().heightmap().InitGPU();
            renderer_.terrain().splatmap().SetAllData(prev.splat);
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
            cur.splat   = renderer_.terrain().splatmap().AllData();
            if ((int)terrainUndo_.size() >= kMaxTerrainUndo)
                terrainUndo_.erase(terrainUndo_.begin());
            terrainUndo_.push_back(std::move(cur));
            auto& next = terrainRedo_.back();
            renderer_.terrain().heightmap().heights = next.heights;
            renderer_.terrain().heightmap().InitGPU();
            renderer_.terrain().splatmap().SetAllData(next.splat);
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
        case kSelLight:     cycle(scene_.lights,     kSelLight);     break;
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
        else if (selectedType_ == kSelAtmosphereVolume) { allowScale = 0b111; }

        // Tell the renderer to draw the active gizmo inside its forward pass
        // (so it lands on the same FBO as the rest of the scene). gz.axis is
        // assigned further down, once the hover test has run (item 3) — it
        // used to be gizmoAxis_ (only non-idle while actively dragging), so
        // the highlight never appeared before a click; now it also reflects
        // the hovered-but-not-yet-grabbed axis. renderer_.SetGizmo(gz) is
        // likewise called after that, once gz.axis is final.
        ZoneRenderer::GizmoState gz;
        gz.pos        = gizmoPos;
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

        // ── Hover test (item 3) — same hit-test math as the press-time latch
        // below, but run EVERY FRAME regardless of click, so the gizmo can be
        // highlighted before the user commits to a click. Kept as a separate
        // read-only pass (rather than merged into the press-time block) so
        // the existing, already-relied-upon grab logic below is untouched —
        // the cost is a small amount of duplicated geometry math between the
        // two, which is fine: it's a handful of dot products per axis/plane,
        // not a scene-wide search like the object raycast above.
        auto findHoverAxis = [&]() -> int {
            if (mouseLook_) return -1;
            if (xformMode_ == kXFormMove) {
                int best = -1; float bestDist = pickRadius;
                for (int a = 0; a < 3; ++a) {
                    float s, d; rayAxis(kAxes[a], s, d);
                    if (s >= -axisLen * 0.2f && s <= axisLen * 1.2f && d < bestDist) {
                        bestDist = d; best = a;
                    }
                }
                const float planeOff  = axisLen * 0.28f;
                const float planeSize = axisLen * 0.28f;
                struct PlanePick { int id, a, b; glm::vec3 n; };
                const PlanePick planes[] = {
                    {4, 0, 1, glm::normalize(glm::cross(kAxes[0], kAxes[1]))},
                    {5, 0, 2, glm::normalize(glm::cross(kAxes[0], kAxes[2]))},
                    {6, 1, 2, glm::normalize(glm::cross(kAxes[1], kAxes[2]))},
                };
                float bestPlaneT = 1e9f;
                for (const auto& pl : planes) {
                    glm::vec3 planeCenter = gizmoPos + kAxes[pl.a] * planeOff + kAxes[pl.b] * planeOff;
                    glm::vec3 hit; float t = 0.f;
                    if (!rayPlaneAt(planeCenter, pl.n, hit, t)) continue;
                    glm::vec3 local = hit - gizmoPos;
                    float ua = glm::dot(local, kAxes[pl.a]);
                    float ub = glm::dot(local, kAxes[pl.b]);
                    float mn = planeOff - planeSize * 0.5f;
                    float mx = planeOff + planeSize * 0.5f;
                    if (ua < mn || ua > mx || ub < mn || ub > mx) continue;
                    if (t < bestPlaneT) { bestPlaneT = t; best = pl.id; }
                }
                return best;
            } else if (xformMode_ == kXFormScale) {
                int best = -1; float bestDist = pickRadius;
                for (int a = 0; a < 3; ++a) {
                    if ((allowScale & (1u << a)) == 0) continue;
                    float s, d; rayAxis(kAxes[a], s, d);
                    if (s >= -axisLen * 0.2f && s <= axisLen * 1.2f && d < bestDist) {
                        bestDist = d; best = a;
                    }
                }
                glm::vec3 w = gizmoPos - ro;
                float t = glm::dot(w, rd);
                glm::vec3 closest = ro + rd * t;
                float d = glm::length(closest - gizmoPos);
                if (d < pickRadius * 0.65f) best = 3;
                return best;
            } else if (xformMode_ == kXFormRotate && allowRot) {
                int best = -1; float bestDist = axisLen * 0.08f;
                for (int a = 0; a < 3; ++a) {
                    if ((allowRot & (1u << a)) == 0) continue;
                    glm::vec3 hit = rayPlane(kAxes[a]);
                    float dist = std::abs(glm::length(hit - gizmoPos) - axisLen);
                    if (dist < bestDist) { bestDist = dist; best = a; }
                }
                return best;
            }
            return -1;
        };
        gizmoHoverAxis_ = (gizmoAxis_ < 0) ? findHoverAxis() : gizmoAxis_;
        gz.axis = gizmoHoverAxis_;
        renderer_.SetGizmo(gz);

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
            pushVec(scene_.spawnPoints,  kSelSpawnPoint);
            pushVec(scene_.playerSpawns, kSelPlayerSpawn);
            pushVec(scene_.atmosphereVolumes, kSelAtmosphereVolume);
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
                    // Same fix as the hit-test raycast above: s.scale alone
                    // is a mesh-scale multiplier, not a world-space size.
                    // Use the model's real local bounds (scaled) when
                    // available. Still axis-aligned and centered on s.pos —
                    // this function's (pos, symmetric half-extent) contract
                    // can't express rotation or an off-center box, but that's
                    // an acceptable approximation for face-snap sizing.
                    glm::vec3 lmn, lmx;
                    if (renderer_.SceneryModelLocalBounds(s.id, lmn, lmx)) {
                        outHalf = glm::max((lmx - lmn) * 0.5f * glm::abs(s.scale), glm::vec3(0.05f));
                    } else {
                        outHalf = glm::max(glm::abs(s.scale) * 0.5f, glm::vec3(0.05f));
                    }
                    return true;
                }
            } else if (st == kSelSpawnPoint) {
                for (const auto& sp : scene_.spawnPoints) if (sp.id == id) {
                    outPos = sp.pos;
                    outHalf = glm::vec3(std::max(0.05f, sp.radius), 0.75f, std::max(0.05f, sp.radius));
                    return true;
                }
            } else if (st == kSelPlayerSpawn) {
                for (const auto& ps : scene_.playerSpawns) if (ps.id == id) {
                    outPos  = ps.pos;
                    outHalf = glm::vec3(0.4f, 0.75f, 0.4f);
                    return true;
                }
            } else if (st == kSelAtmosphereVolume) {
                for (const auto& v : scene_.atmosphereVolumes) if (v.id == id) {
                    outPos  = v.pos;
                    outHalf = glm::max(glm::abs(v.size) * 0.5f, glm::vec3(0.05f));
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
            for (const auto& s : scene_.scenery) {
                // Same s.scale-as-world-size bug as the hit-test/tryGetBounds
                // fixes above — use the model's real bounds when loaded.
                glm::vec3 slmn, slmx;
                glm::vec3 sHalf = renderer_.SceneryModelLocalBounds(s.id, slmn, slmx)
                    ? glm::max((slmx - slmn) * 0.5f * glm::abs(s.scale), glm::vec3(0.05f))
                    : glm::max(glm::abs(s.scale) * 0.5f, glm::vec3(0.05f));
                testCandidate(kSelScenery, s.id, s.pos, sHalf);
            }
            for (const auto& sp : scene_.spawnPoints)  testCandidate(kSelSpawnPoint,  sp.id, sp.pos, glm::vec3(std::max(0.05f, sp.radius), 0.75f, std::max(0.05f, sp.radius)));
            for (const auto& ps : scene_.playerSpawns) testCandidate(kSelPlayerSpawn, ps.id, ps.pos, glm::vec3(0.4f, 0.75f, 0.4f));
            for (const auto& v : scene_.atmosphereVolumes)
                testCandidate(kSelAtmosphereVolume, v.id, v.pos, glm::max(glm::abs(v.size) * 0.5f, glm::vec3(0.05f)));

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
        // Gated on the click arbiter having already decided this click
        // belongs to the gizmo (see "Unified LMB click-priority arbiter"
        // near the top of this function) instead of re-deciding here — the
        // arbiter itself used last frame's gizmoHoverAxis_, computed just
        // above, so the two agree except in the vanishingly rare case where
        // the cursor crosses onto/off a handle in the exact same frame as
        // the click.
        if (lmbGestureOwner_ == kLmbOwnerGizmo && gizmoAxis_ < 0) {
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
                    gizmoScaleAccumUniform_ = 0.f;
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
                    // Fixed for the whole drag — see gizmoRotAxisWorld_ comment
                    // (zones.h) for why this can't be re-read live each frame.
                    gizmoRotAxisWorld_ = kAxes[best];
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
                        // Snap the ACCUMULATED DELTA since drag start, not
                        // the resulting absolute position — regardless of
                        // gizmo space. Snapping the absolute position instead
                        // (the old world-space-only behaviour) made the
                        // object jump a full grid cell the instant the
                        // continuous drag crossed a cell boundary, because
                        // the snap result depended on where the object WAS,
                        // not on how far the mouse had actually moved. See
                        // docs/TECH_DEBT.md "Viewport input priority chain".
                        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                            delta = std::round(delta / scnGridSize_) * scnGridSize_;
                        }
                        np = gizmoStartPos_ + kAxes[gizmoAxis_] * delta;
                        if (!useLocalAxes) {
                            // Object-to-object snapping is inherently
                            // absolute (it compares against other objects'
                            // world positions) — unaffected by the delta-vs-
                            // absolute grid-snap fix above.
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
                            // Same fix as the single-axis case above: snap
                            // the plane-local delta (da, db) since drag
                            // start, always — not the absolute np[a]/np[b].
                            glm::vec3 dv = delta;
                            float da = glm::dot(dv, kAxes[a]);
                            float db = glm::dot(dv, kAxes[b]);
                            if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                                da = std::round(da / scnGridSize_) * scnGridSize_;
                                db = std::round(db / scnGridSize_) * scnGridSize_;
                            }
                            np = gizmoStartPos_ + kAxes[a] * da + kAxes[b] * db;
                            if (!useLocalAxes) {
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
                        // gizmoScaleAccumUniform_ accumulates the RAW
                        // (unsnapped) growth since drag start — same idea as
                        // gizmoRotAccumDeg_ for rotate — so gizmoStartScale_
                        // can stay fixed at its drag-start value for the
                        // whole gesture instead of being re-based to the
                        // (snapped) result every frame. That re-basing was
                        // the bug: snapping the ABSOLUTE resulting scale
                        // every frame made a single frame's tiny mouse
                        // movement jump the size by a comparatively large
                        // grid step whenever the unsnapped value happened to
                        // cross a grid boundary. Snapping the DELTA since
                        // drag start instead (below) fixes that, matching
                        // the Move-gizmo fix above. See docs/TECH_DEBT.md
                        // "Viewport input priority chain".
                        float speed = ctrlDown ? 0.25f : (shiftDown ? 2.0f : 1.0f);
                        float dy = -ImGui::GetIO().MouseDelta.y * 0.01f * speed;
                        gizmoScaleAccumUniform_ += dy;
                        glm::vec3 s = gizmoStartScale_ * (1.f + gizmoScaleAccumUniform_);
                        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                            glm::vec3 sizeDelta = s - gizmoStartScale_;
                            sizeDelta.x = std::round(sizeDelta.x / scnGridSize_) * scnGridSize_;
                            sizeDelta.y = std::round(sizeDelta.y / scnGridSize_) * scnGridSize_;
                            sizeDelta.z = std::round(sizeDelta.z / scnGridSize_) * scnGridSize_;
                            s = gizmoStartScale_ + sizeDelta;
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
                        // gizmoStartScale_ intentionally NOT reassigned here
                        // anymore — it stays fixed at the drag-start value so
                        // gizmoScaleAccumUniform_ (and the peer `factor`
                        // computation above) both measure cumulative growth
                        // since drag start, not just the latest frame's step.
                    } else {
                        float s, d;
                        rayAxis(kAxes[gizmoAxis_], s, d);
                        float factor = (axisLen > 0.f) ? (s / axisLen) : 1.f;
                        factor = glm::clamp(factor, 0.01f, 100.f);
                        if (ctrlDown)       factor = 1.f + (factor - 1.f) * 0.25f;
                        else if (shiftDown) factor = 1.f + (factor - 1.f) * 2.0f;
                        glm::vec3 ns = gizmoStartScale_;
                        ns[gizmoAxis_] = gizmoStartScale_[gizmoAxis_] * factor;
                        // Snap the world-unit DELTA since drag start, not the
                        // absolute resulting size — same fix as uniform scale
                        // and the Move gizmo above.
                        if (scnSnapGrid_ && scnGridSize_ > 0.f) {
                            float sizeDelta = ns[gizmoAxis_] - gizmoStartScale_[gizmoAxis_];
                            sizeDelta = std::round(sizeDelta / scnGridSize_) * scnGridSize_;
                            ns[gizmoAxis_] = gizmoStartScale_[gizmoAxis_] + sizeDelta;
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
                    // rot.x/y/z are compound Euler angles (render applies
                    // Ry*Rx*Rz — see BuildEulerYXZDeg), not independent
                    // world axes, so the drag delta can't be added straight
                    // into rot[gizmoAxis_]: once the object has any other
                    // non-zero angle (yaw especially), that naively "adds to
                    // X" or "adds to Z" no longer matches a rotation around
                    // the world-space ring the user is actually dragging —
                    // the X and Z rings end up visually swapped. Fix: rotate
                    // the drag-start matrix by delta_deg around the FIXED
                    // world axis captured at mouse-down
                    // (gizmoRotAxisWorld_), then decompose back to Euler.
                    const glm::mat3 rotMat3 = glm::mat3(glm::rotate(
                        glm::mat4(1.f), glm::radians(delta_deg), gizmoRotAxisWorld_));
                    glm::vec3 rot = DecomposeEulerYXZDeg(rotMat3 * BuildEulerYXZDeg(gizmoStartRot_));
                    SetSelectedRot(rot);
                    if (!gizmoSelectionStart_.empty()) {
                        const int primaryType = selectedType_;
                        const int primaryID = selectedID_;
                        const glm::mat4 rotMat = glm::mat4(rotMat3);
                        for (const auto& st : gizmoSelectionStart_) {
                            if (st.type == primaryType && st.id == primaryID) continue;
                            selectedType_ = st.type;
                            selectedID_ = st.id;
                            if (st.hasRot) {
                                glm::vec3 peerRot = DecomposeEulerYXZDeg(
                                    rotMat3 * BuildEulerYXZDeg(st.rot));
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
        gizmoHoverAxis_ = -1;
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
    // (scnArmed itself is computed earlier, alongside the click arbiter, so
    // that arbiter can see it — see the "Unified LMB click-priority arbiter"
    // block above.) Still gated on the actual click frame (IsMouseClicked),
    // not just "this gesture's owner is Placement", so holding LMB down
    // doesn't place an object every single frame.
    if (lmbClickedNow && lmbGestureOwner_ == kLmbOwnerPlacement) {
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
            {"Point Light",      kModeLight      },
            {"Spawn Point",      kModeSpawnPoint },
            {"Atmosphere Volume", kModeAtmosphereVolume },
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
    if (tryVec(scene_.lights,    kSelLight))    return true;
    if (tryVec(scene_.water,     kSelWater))    return true;
    if (tryVec(scene_.scenery,     kSelScenery))    return true;
    if (tryVec(scene_.spawnPoints,  kSelSpawnPoint))  return true;
    if (tryVec(scene_.playerSpawns, kSelPlayerSpawn)) return true;
    if (tryVec(scene_.atmosphereVolumes, kSelAtmosphereVolume)) return true;
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
    trySet(scene_.lights,    kSelLight);
    trySet(scene_.water,     kSelWater);
    trySet(scene_.scenery,     kSelScenery);
    trySet(scene_.spawnPoints,  kSelSpawnPoint);
    trySet(scene_.playerSpawns, kSelPlayerSpawn);
    trySet(scene_.atmosphereVolumes, kSelAtmosphereVolume);
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
    case kSelLight:     sql = "UPDATE zone_lights      SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelWater:     sql = "UPDATE zone_water       SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelScenery:    sql = "UPDATE zone_scenery  SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelSpawnPoint:  sql = "UPDATE spawn_points  SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelPlayerSpawn: sql = "UPDATE player_spawns SET x=?,y=?,z=? WHERE id=?"; break;
    case kSelAtmosphereVolume: sql = "UPDATE zone_atmosphere_volumes SET pos_x=?,pos_y=?,pos_z=? WHERE id=?"; break;
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
    } else if (selectedType_ == kSelAtmosphereVolume) {
        for (auto& v : scene_.atmosphereVolumes) if (v.id == selectedID_) { out = v.size; return true; }
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
    } else if (selectedType_ == kSelAtmosphereVolume) {
        for (auto& v : scene_.atmosphereVolumes) if (v.id == selectedID_) { v.size = s; return; }
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
    } else if (selectedType_ == kSelAtmosphereVolume) {
        for (auto& v : scene_.atmosphereVolumes) if (v.id == selectedID_) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                "UPDATE zone_atmosphere_volumes SET size_x=?,size_y=?,size_z=? WHERE id=?",
                -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(st, 1, v.size.x);
                sqlite3_bind_double(st, 2, v.size.y);
                sqlite3_bind_double(st, 3, v.size.z);
                sqlite3_bind_int   (st, 4, v.id);
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
