/**
 * @file TFSquadHUDBeacon.cpp
 * @brief TFSquadHUD world-space half: the squad-waypoint beacon (tall pulsing
 *        light pillar + base diamond + distance readout on the foreground
 *        drawlist, TFNameplates ProjectToScreen mirror) and the debug panel.
 *
 * Split from TFSquadHUD.cpp per the repo file-size rule (TFHUDDraw.cpp /
 * TFPingUIDraw.cpp pattern — same class, feature-owned translation units).
 * TFSquadHUD.cpp keeps the lifecycle, gating, and the left-edge squadmate
 * list. Shared internals (the SquadCol accent) live in TFSquadHUDInternal.h.
 */
#include "UI/TFSquadHUD.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFSquadSystem.h"
#include "UI/TFSquadHUDInternal.h"
#include "UI/TFUiCommon.h"
#include "World/TFWorldSetup.h"
#include "Camera/SparkEngineCamera.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs elsewhere

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cmath>
#include <cstdio>

namespace Terrafront
{

    using namespace SquadHudDetail; // TFSquadHUDInternal.h: SquadCol shared with the list part

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        /// World -> screen through the SAME matrices the frame was rendered
        /// with. MUST MATCH TFWorldSetup::ComputeViewProj's first-person branch
        /// (LookAtLH from the module camera pose; PerspectiveFovLH with
        /// XM_PIDIV4 * 1.6, window aspect, 0.3, 6000) — mirrored from
        /// TFNameplates.cpp; if the render FOV or clip planes ever change,
        /// change every mirror or beacons/plates drift off-world.
        bool ProjectToScreen(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj, const ImGuiViewport& vp,
                             const float world[3], ImVec2& out)
        {
            using namespace DirectX;
            const XMVECTOR ws = XMVectorSet(world[0], world[1], world[2], 1.0f);
            const XMVECTOR vs = XMVector3TransformCoord(ws, view);
            if (XMVectorGetZ(vs) < 0.3f)
                return false; // behind / inside the near plane (LH: +z forward)
            const XMVECTOR ndc = XMVector3TransformCoord(vs, proj);
            const float nx = XMVectorGetX(ndc);
            const float ny = XMVectorGetY(ndc);
            if (nx < -1.25f || nx > 1.25f || ny < -1.25f || ny > 1.25f)
                return false; // comfortably off-screen
            out.x = vp.Pos.x + (nx * 0.5f + 0.5f) * vp.Size.x;
            out.y = vp.Pos.y + (0.5f - ny * 0.5f) * vp.Size.y;
            return true;
        }

    } // namespace

    void TFSquadHUD::DrawWaypointBeacon()
    {
        if (!m_ctx->world)
            return;
        float wp[3];
        if (!m_ctx->squads->GetLocalWaypoint(wp))
            return;
        PawnInfo me{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, me) || !me.alive)
            return; // dead: no first-person camera to project through
        SparkEngineCamera* cam = m_ctx->world->GetCamera();
        if (!cam)
            return;

        using namespace DirectX;
        const XMFLOAT3 cp = cam->GetPosition();
        const XMFLOAT3 cf = cam->GetForward();
        float fx = cf.x, fy = cf.y, fz = cf.z;
        const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (flen < 1e-4f)
            return;
        fx /= flen;
        fy /= flen;
        fz /= flen;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;
        const XMVECTOR eye = XMLoadFloat3(&cp);
        const XMVECTOR fwd = XMVectorSet(fx, fy, fz, 0.0f);
        const XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorAdd(eye, fwd), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.6f, vp->Size.x / vp->Size.y, 0.3f, 6000.0f);

        // Pillar base sits on the live terrain (waypoints replicate the height
        // sampled at set time; re-clamp so the beacon never floats or sinks).
        const float groundY = m_ctx->world->TerrainHeightAt(wp[0], wp[2]);
        const float base[3] = {wp[0], groundY, wp[2]};
        const float top[3] = {wp[0], groundY + kTFSquadHudBeaconHeightM, wp[2]};
        const float marker[3] = {wp[0], groundY + kTFSquadHudBeaconBaseM, wp[2]};

        ImVec2 sBase, sTop, sMark;
        if (!ProjectToScreen(view, proj, *vp, base, sBase) || !ProjectToScreen(view, proj, *vp, top, sTop) ||
            !ProjectToScreen(view, proj, *vp, marker, sMark))
            return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float pulse = 0.72f + 0.28f * std::sin(m_clock * 3.2f);

        // Light pillar: nested vertical quads, wide-faint to narrow-bright.
        struct Layer
        {
            float halfW;
            int alpha;
        };
        constexpr Layer layers[3] = {{5.0f, 34}, {2.6f, 72}, {1.1f, 150}};
        for (const Layer& ly : layers)
        {
            const int a8 = static_cast<int>(static_cast<float>(ly.alpha) * pulse);
            dl->AddQuadFilled(ImVec2(sBase.x - ly.halfW, sBase.y), ImVec2(sBase.x + ly.halfW, sBase.y),
                              ImVec2(sTop.x + ly.halfW, sTop.y), ImVec2(sTop.x - ly.halfW, sTop.y), SquadCol(a8));
        }

        // Base diamond + distance readout.
        const float r = 7.0f;
        const ImVec2 dia[4] = {ImVec2(sMark.x, sMark.y - r), ImVec2(sMark.x + r, sMark.y), ImVec2(sMark.x, sMark.y + r),
                               ImVec2(sMark.x - r, sMark.y)};
        dl->AddConvexPolyFilled(dia, 4, SquadCol(static_cast<int>(230.0f * pulse)));
        dl->AddPolyline(dia, 4, IM_COL32(16, 40, 24, 200), ImDrawFlags_Closed, 1.5f);

        const float dxc = wp[0] - cp.x, dzc = wp[2] - cp.z;
        char dtxt[16];
        std::snprintf(dtxt, sizeof(dtxt), "%.0fm", std::sqrt(dxc * dxc + dzc * dzc));
        TFUi::AddTextCentered(dl, ImGui::GetFontSize() * 0.95f, ImVec2(sMark.x + 1.0f, sMark.y + r + 9.0f),
                              IM_COL32(0, 0, 0, 190), dtxt);
        TFUi::AddTextCentered(dl, ImGui::GetFontSize() * 0.95f, ImVec2(sMark.x, sMark.y + r + 8.0f), SquadCol(235),
                              dtxt);

        m_beaconDrawn = true;
    }

    void TFSquadHUD::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Squad HUD"))
            return;
        ImGui::Text("rows drawn  : %u", m_rowsDrawn);
        ImGui::Text("beacon drawn: %s", m_beaconDrawn ? "yes" : "no");
        if (m_ctx && m_ctx->squads)
            ImGui::Text("wp requests : %zu", m_ctx->squads->PendingWaypointRequests().size());
    }

#else // !SPARK_HAS_IMGUI — headless: nothing to draw

    void TFSquadHUD::DrawWaypointBeacon() {}
    void TFSquadHUD::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
