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
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) xformMode_ = kXFormSelect;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) xformMode_ = kXFormMove;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) xformMode_ = kXFormRotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) xformMode_ = kXFormScale;
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) xformMode_ = kXFormSelect;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) xformMode_ = kXFormMove;
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) xformMode_ = kXFormRotate;
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) xformMode_ = kXFormScale;
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

    // Right-click has dual use:
    // 1) placement/context in zone editing
    // 2) freelook/fly camera
    // If this click is for placement/context, suppress fly until RMB release.
    const bool scnArmedIntent = (zoneMode_ == kModeScenery &&
                                 scnModelId_ != 0 &&
                                 xformMode_ == kXFormSelect);
    const bool wantsRmbAddMenu = (xformMode_ == kXFormSelect && !scnArmedIntent);
    const bool wantsRmbDirectPlace =
        (!scnArmedIntent &&
         zoneMode_ != kModeEnviro &&
         zoneMode_ != kModeOther &&
         zoneMode_ != kModeTerrain);
    const bool rmbPlacementClick =
        (vpHovered_ && ImGui::IsMouseClicked(1) && !altDown &&
         (wantsRmbAddMenu || wantsRmbDirectPlace));

    if (!rmbDown) suppressMouseLookUntilRmbRelease_ = false;
    if (rmbPlacementClick) suppressMouseLookUntilRmbRelease_ = true;

    // RMB engage / disengage freelook
    if (vpHovered_ && ImGui::IsMouseClicked(1) && !altDown &&
        !suppressMouseLookUntilRmbRelease_) {
        mouseLook_ = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    if (!rmbDown) mouseLook_ = false;

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
            cam_.yaw   += io.MouseDelta.x * cam_.sens;
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

    // mouseLook_ used downstream to suppress selection / placement clicks.
    // Redefine: true while RMB is held (freelook active, not alt-dolly).
    mouseLook_ = rmbDown && !altDown && !suppressMouseLookUntilRmbRelease_;

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
                selectedID_   = bestID;
                selectedType_ = bestType;
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
            selectedID_   = -1;
            selectedType_ = kSelNone;
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
    if (vpHovered_ && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        DuplicateSelected(db);

    // Tab — cycle to next object of same type
    if (vpHovered_ && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        auto cycle = [&](auto& vec, int selType) {
            if (vec.empty()) return;
            int idx = -1;
            for (int i = 0; i < (int)vec.size(); ++i)
                if (vec[i].id == selectedID_) { idx = i; break; }
            idx = (idx + 1) % (int)vec.size();
            selectedID_   = vec[idx].id;
            selectedType_ = selType;
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

    // ── Gizmos: Move / Rotate / Scale ────────────────────────────────────
    // All three share hit-testing against the object's origin; only the
    // meaning of "drag along axis X" differs: translate / rotate / scale.
    glm::vec3 selPos;
    const bool haveSel = (xformMode_ != kXFormSelect && selectedID_ >= 0 &&
                          selectedType_ != kSelNone && SelectedPos(selPos));

    if (haveSel) {
        const glm::mat4 viewProj =
            cam_.Proj(vpSize_.x / std::max(vpSize_.y, 1.f)) * cam_.View();
        const glm::vec3 kAxes[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
        const float axisLen    = ZoneRenderer::GizmoAxisLength(selPos, cam_.pos);
        const float pickRadius = axisLen * 0.18f;

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
        gz.pos        = selPos;
        gz.axis       = gizmoAxis_;
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
            glm::vec3 w = ro - selPos;
            float b = glm::dot(rd, ad);
            float d = glm::dot(rd, w);
            float e = glm::dot(ad, w);
            float denom = 1.f - b * b;
            if (denom < 1e-6f) { s = 0; dist = 1e9f; return; }
            s = (e - b * d) / denom;
            float t = (b * e - d) / denom;
            dist = glm::length((ro + rd * t) - (selPos + ad * s));
        };

        // Ray-plane intersection: returns hit point. plane passes through
        // selPos with normal `n`. Falls back to selPos if ray is parallel.
        auto rayPlane = [&](const glm::vec3& n) -> glm::vec3 {
            float denom = glm::dot(rd, n);
            if (std::abs(denom) < 1e-6f) return selPos;
            float t = glm::dot(selPos - ro, n) / denom;
            return ro + rd * t;
        };

        // Signed angle of a point on the plane perpendicular to axis `a`.
        auto angleOnRing = [&](int a, const glm::vec3& pt) {
            glm::vec3 v = pt - selPos;
            // Pick the same basis used when drawing the ring.
            glm::vec3 u = (a == 1) ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
            glm::vec3 t = glm::normalize(glm::cross(kAxes[a], u));
            glm::vec3 b = glm::normalize(glm::cross(kAxes[a], t));
            return std::atan2(glm::dot(v, b), glm::dot(v, t));
        };

        // ── Press: hit-test and latch onto an axis ────────────────────────
        if (vpHovered_ && ImGui::IsMouseClicked(0) && gizmoAxis_ < 0 && !mouseLook_) {
            if (xformMode_ == kXFormMove || xformMode_ == kXFormScale) {
                int   best = -1;
                float bestDist = pickRadius;
                float bestS    = 0.f;
                unsigned allow = (xformMode_ == kXFormMove) ? 0b111u : allowScale;
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
                if (xformMode_ == kXFormScale) {
                    float s0, d0; rayAxis(glm::vec3(0, 1, 0), s0, d0);
                    // Cheaper: distance ray ↔ point
                    glm::vec3 w = selPos - ro;
                    float t = glm::dot(w, rd);
                    glm::vec3 closest = ro + rd * t;
                    float d = glm::length(closest - selPos);
                    if (d < pickRadius * 0.65f) { best = 3; bestS = 0.f; }
                }
                if (best >= 0) {
                    gizmoAxis_       = best;
                    gizmoStartPos_   = selPos;
                    gizmoStartS_     = bestS;
                    gizmoPrePos_     = selPos;
                    glm::vec3 rot;   SelectedRot(rot);     gizmoStartRot_   = rot;   gizmoPreRot_ = rot;
                    glm::vec3 scl;   SelectedScale(scl);   gizmoStartScale_ = scl;   gizmoPreScale_ = scl;
                }
            } else if (xformMode_ == kXFormRotate && allowRot) {
                int   best = -1;
                float bestDist = axisLen * 0.08f;
                for (int a = 0; a < 3; ++a) {
                    if ((allowRot & (1u << a)) == 0) continue;
                    glm::vec3 hit = rayPlane(kAxes[a]);
                    float dist = std::abs(glm::length(hit - selPos) - axisLen);
                    if (dist < bestDist) { bestDist = dist; best = a; }
                }
                if (best >= 0) {
                    gizmoAxis_     = best;
                    gizmoStartS_   = angleOnRing(best, rayPlane(kAxes[best]));
                    glm::vec3 rot; SelectedRot(rot); gizmoStartRot_ = rot; gizmoPreRot_ = rot;
                }
            }
        }

        // ── Drag / release ───────────────────────────────────────────────
        if (gizmoAxis_ >= 0) {
            if (ImGui::IsMouseDown(0)) {
                if (xformMode_ == kXFormMove) {
                    float s, d;
                    rayAxis(kAxes[gizmoAxis_], s, d);
                    glm::vec3 np = gizmoStartPos_ + kAxes[gizmoAxis_] * (s - gizmoStartS_);
                    SetSelectedPos(np);
                } else if (xformMode_ == kXFormScale) {
                    if (gizmoAxis_ == 3) {
                        // Uniform: drag vertically on screen. Use mouse ΔY.
                        float dy = -ImGui::GetIO().MouseDelta.y * 0.01f;
                        glm::vec3 s = gizmoStartScale_ * (1.f + dy);
                        s = glm::max(s, glm::vec3(0.01f));
                        SetSelectedScale(s);
                        gizmoStartScale_ = s;   // accumulate on next frame
                    } else {
                        float s, d;
                        rayAxis(kAxes[gizmoAxis_], s, d);
                        float factor = (axisLen > 0.f) ? (s / axisLen) : 1.f;
                        factor = glm::clamp(factor, 0.01f, 100.f);
                        glm::vec3 ns = gizmoStartScale_;
                        ns[gizmoAxis_] = gizmoStartScale_[gizmoAxis_] * factor;
                        SetSelectedScale(ns);
                    }
                } else if (xformMode_ == kXFormRotate) {
                    glm::vec3 hit = rayPlane(kAxes[gizmoAxis_]);
                    float a = angleOnRing(gizmoAxis_, hit);
                    float delta_deg = glm::degrees(a - gizmoStartS_);
                    glm::vec3 rot = gizmoStartRot_;
                    rot[gizmoAxis_] += delta_deg;
                    SetSelectedRot(rot);
                }
            } else {
                // Release: persist + undo entry for the modified transform.
                if (xformMode_ == kXFormMove) {
                    PushUndo(kUndoMove,   selectedType_, selectedID_, gizmoPrePos_);
                    PersistSelectedPos(db);
                    std::snprintf(statusMsg_, sizeof(statusMsg_), "Moved id=%d.", selectedID_);
                } else if (xformMode_ == kXFormRotate) {
                    PushUndo(kUndoRotate, selectedType_, selectedID_, gizmoPreRot_);
                    PersistSelectedRot(db);
                    std::snprintf(statusMsg_, sizeof(statusMsg_), "Rotated id=%d.", selectedID_);
                } else if (xformMode_ == kXFormScale) {
                    PushUndo(kUndoScale,  selectedType_, selectedID_, gizmoPreScale_);
                    PersistSelectedScale(db);
                    std::snprintf(statusMsg_, sizeof(statusMsg_), "Scaled id=%d.", selectedID_);
                }
                gizmoAxis_ = -1;
            }
        }
    } else {
        gizmoAxis_ = -1;
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

    // RMB context menu (non-terrain, non-armed modes).
    if (vpHovered_ && ImGui::IsMouseClicked(1) && !mouseLook_) {
        ImVec2 mp  = ImGui::GetMousePos();
        float  vpX = mp.x - vpOrigin_.x;
        float  vpY = mp.y - vpOrigin_.y;
        if (xformMode_ == kXFormSelect && !scnArmed) {
            pendingPlacePos_ = RaycastScene(vpX, vpY);
            ImGui::OpenPopup("##vp_add_ctx");
        } else if (!scnArmed && zoneMode_ != kModeEnviro
                   && zoneMode_ != kModeOther && zoneMode_ != kModeTerrain) {
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
