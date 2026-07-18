/**
 * @file TFPingUIDraw.cpp
 * @brief TFPingUI rendering half: distance labels drawn under the live 3D
 *        ping markers (ImGui foreground drawlist, TFNameplates
 *        ProjectToScreen mirror) and the Q-hold ping wheel + debug panel.
 *
 * Split from TFPingUI.cpp per the repo file-size rule (TFHUDDraw.cpp /
 * TFMapScreenDraw.cpp pattern — same class, feature-owned translation units).
 * TFPingUI.cpp keeps the lifecycle, Q tap/hold input, ray resolution, and the
 * place/receive bleeps.
 */
#include "UI/TFPingUI.h"

#include "Camera/SparkEngineCamera.h"
#include "Game/TFPingSystem.h"
#include "Game/TFWeaponMath.h"
#include "UI/TFScoreboard.h"
#include "World/TFWorldSetup.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    namespace
    {
        /// World -> screen through the SAME matrices the frame was rendered
        /// with. MUST MATCH TFWorldSetup::ComputeViewProj's first-person branch
        /// (LookAtLH from the module camera pose; PerspectiveFovLH with
        /// XM_PIDIV4 * 1.6, window aspect, 0.3, 6000) — the TFNameplates
        /// ProjectToScreen mirror. If the render FOV or clip planes ever
        /// change, change this too or labels drift off the markers.
        bool PingProjectToScreen(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj, const ImGuiViewport& vp,
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

        ImU32 PingCol32(TFPingType t, float alpha)
        {
            float c[4];
            PingColor(t, c);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
        }
    } // namespace

    void TFPingUI::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_pings || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;
        if (FullscreenUiOpen())
            return;
        if (m_ctx->scoreboard && m_ctx->scoreboard->IsOpen())
            return;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;

        // ---- distance labels under the 3D markers ------------------------------
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (cam && LocalPawnAlive())
        {
            using namespace DirectX;
            const XMFLOAT3 cp = cam->GetPosition();
            const XMFLOAT3 cf = cam->GetForward();
            float fwd[3] = {cf.x, cf.y, cf.z};
            if (WeaponMath::Normalize3(fwd))
            {
                const XMVECTOR eye = XMLoadFloat3(&cp);
                const XMVECTOR fv = XMVectorSet(fwd[0], fwd[1], fwd[2], 0.0f);
                const XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorAdd(eye, fv), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
                const XMMATRIX proj =
                    XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.6f, vp->Size.x / vp->Size.y, 0.3f, 6000.0f);

                ImDrawList* dl = ImGui::GetForegroundDrawList();
                m_pings->ForEachActivePing(
                    [&](const TFPingView& p)
                    {
                        // Label sits just under the marker's bob midline.
                        const float anchor[3] = {p.pos[0], p.pos[1] + (p.type == TFPingType::Enemy ? 1.55f : 0.20f),
                                                 p.pos[2]};
                        ImVec2 sp;
                        if (!PingProjectToScreen(view, proj, *vp, anchor, sp))
                            return;
                        const float dx = p.pos[0] - cp.x, dy = p.pos[1] - cp.y, dz = p.pos[2] - cp.z;
                        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const float alpha = std::clamp(p.lifeLeft / kTFPingMarkerFadeSec, 0.0f, 1.0f);
                        if (alpha <= 0.02f)
                            return;
                        char label[48];
                        std::snprintf(label, sizeof(label), "%s %.0fm", PingTypeName(p.type), dist);
                        const float fontSize = ImGui::GetFontSize() * 0.92f;
                        const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fontSize, 1.0e9f, 0.0f, label);
                        const ImVec2 pos(sp.x - sz.x * 0.5f, sp.y + 4.0f);
                        dl->AddText(ImGui::GetFont(), fontSize, ImVec2(pos.x + 1.0f, pos.y + 1.0f),
                                    IM_COL32(0, 0, 0, static_cast<int>(alpha * 190.0f)), label);
                        dl->AddText(ImGui::GetFont(), fontSize, pos, PingCol32(p.type, alpha), label);
                    });
            }
        }

        // ---- hold-wheel ---------------------------------------------------------
        if (m_wheelOpen)
        {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.62f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.82f);
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("##TFPingWheel", nullptr, flags))
            {
                ImGui::TextDisabled("PING");
                ImGui::Separator();
                float c[4];
                PingColor(TFPingType::Location, c);
                ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "[1] Location");
                PingColor(TFPingType::Enemy, c);
                ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "[2] Enemy");
                PingColor(TFPingType::NeedSupport, c);
                ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "[3] Need Support");
                ImGui::Separator();
                ImGui::TextDisabled("release to cancel");
            }
            ImGui::End();
        }
    }

    void TFPingUI::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Ping UI"))
            return;
        ImGui::Text("tap pings   : %u", m_tapPings);
        ImGui::Text("wheel pings : %u", m_wheelPings);
        ImGui::Text("no target   : %u", m_noTarget);
        ImGui::Text("wheel open  : %s (held %.2fs)", m_wheelOpen ? "yes" : "no", m_holdSec);
    }

#else // !SPARK_HAS_IMGUI — headless: input/audio state only

    void TFPingUI::RenderUI() {}
    void TFPingUI::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
