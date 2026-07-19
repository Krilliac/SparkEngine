/**
 * @file TFTutorialUi.cpp
 * @brief TFTutorial ImGui presentation: the toast line, the right-edge step
 *        checklist and the world-anchored hint markers (ping-marker spirit,
 *        own copy of the TFTargetRange::RenderUI projection recipe). Split
 *        from TFTutorial.cpp; lifecycle and step detection stay there.
 */
#include "Game/TFTutorial.h"

#include "Game/TFTutorialInternal.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

    using namespace TutorialDetail;

    // ---------------------------------------------------------------------------
    // Presentation (checklist + world markers + toast)
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        // ---- world anchors (own copies — cosmetic markers only) ----------------
        // MUST match TFTargetRange.cpp kRangeCenterX / kLineZ (firing line).
        constexpr float kRangeMarkX = 404.0f;
        constexpr float kRangeMarkZ = 3878.0f;

        // Projection recipe copied from TFTargetRange::RenderUI — must match
        // TFWorldSetup::ComputeViewProj's first-person branch (XM_PIDIV4 * 1.6,
        // near 0.3); cosmetic drift tolerance only.
        constexpr float kFovY = 0.78539816f * 1.6f;
        constexpr float kNearZ = 0.3f;

    } // namespace

    void TFTutorial::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;

        // Toast (start/finish/skip/reset) — drawn even when done so the
        // completion line survives arriving on the continent.
        if (m_toastTTL > 0.0f && m_toast[0] != '\0' && !FullscreenUiOpen())
        {
            const float a = std::clamp(m_toastTTL / 0.6f, 0.0f, 1.0f);
            TFUi::AddTextCentered(ImGui::GetForegroundDrawList(), 20.0f,
                                  ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.18f),
                                  IM_COL32(150, 230, 255, static_cast<int>(235.0f * a)), m_toast);
        }

        if (!m_active || m_done || FullscreenUiOpen())
            return;

        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive) || !alive)
            return;
        if (!TFTravel_IsInSanctuary(pos[0], pos[2]))
            return; // combat zone: auto-hidden (paused)

        // ---- compact checklist (right edge, below the minimap band) ------------
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 14.0f, vp->Pos.y + vp->Size.y * 0.30f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##tf_tutorial", nullptr, flags))
        {
            ImGui::TextColored(ImVec4(0.55f, 0.86f, 1.0f, 1.0f), "TUTORIAL");
            ImGui::SameLine();
            if (m_escHoldSec > 0.05f)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), " skipping %d%%",
                                   static_cast<int>(100.0f * m_escHoldSec / kTFTutorialSkipHoldSec));
            else
                ImGui::TextDisabled(" hold ESC to skip");
            ImGui::Separator();

            char line[128];
            for (uint8_t i = 0; i < static_cast<uint8_t>(Step::COUNT); ++i)
            {
                const Step s = static_cast<Step>(i);
                const bool doneStep = i < static_cast<uint8_t>(m_step);
                const bool current = s == m_step;
                if (doneStep)
                {
                    std::snprintf(line, sizeof(line), "[x] %s", StepLabel(s));
                    ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.5f, 0.85f), "%s", line);
                }
                else if (current)
                {
                    std::snprintf(line, sizeof(line), " >  %s", StepLabel(s));
                    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s", line);
                    if (s == Step::Move)
                        ImGui::TextDisabled("      %d%%", static_cast<int>(100.0f * m_moveSec / kMoveHoldSec));
                    else if (s == Step::Sprint)
                        ImGui::TextDisabled("      %d%%", static_cast<int>(100.0f * m_sprintSec / kSprintHoldSec));
                    else if (s == Step::RangeHits)
                        ImGui::TextDisabled("      hits %u/%u  (follow the marker)", m_rangeHits, kTFTutorialRangeHits);
                    else if (s == Step::ClassTerm || s == Step::Travel)
                        ImGui::TextDisabled("      follow the marker");
                }
                else
                {
                    std::snprintf(line, sizeof(line), "[ ] %s", StepLabel(s));
                    ImGui::TextDisabled("%s", line);
                }
            }
        }
        ImGui::End();

        // ---- world-anchored hint marker for the current step -------------------
        float anchorX = 0.0f, anchorZ = 0.0f;
        const char* anchorLabel = nullptr;
        if (m_step == Step::RangeHits)
        {
            anchorX = kRangeMarkX;
            anchorZ = kRangeMarkZ;
            anchorLabel = "FIRING RANGE";
        }
        else if (m_step == Step::ClassTerm)
        {
            anchorX = kClassTermX;
            anchorZ = kClassTermZ;
            anchorLabel = "CLASS TERMINAL";
        }
        else if (m_step == Step::Travel)
        {
            anchorX = kTFSanctuaryTerminalX;
            anchorZ = kTFSanctuaryTerminalZ;
            anchorLabel = "TRAVEL TERMINAL";
        }
        if (!anchorLabel || !m_ctx->world)
            return;

        const SparkEngineCamera* cam = m_ctx->world->GetCamera();
        if (!cam)
            return;

        // Camera basis + projection: own copy of the TFTargetRange::RenderUI
        // recipe (must match TFWorldSetup::ComputeViewProj's first-person
        // branch; cosmetic drift tolerance only).
        const auto cp = cam->GetPosition();
        const auto cf = cam->GetForward();
        float fwd[3] = {cf.x, cf.y, cf.z};
        const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl < 1.0e-4f)
            return;
        fwd[0] /= fl;
        fwd[1] /= fl;
        fwd[2] /= fl;
        float right[3] = {fwd[2], 0.0f, -fwd[0]};
        const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
        if (rl < 1.0e-4f)
            return; // looking straight up/down: skip this frame
        right[0] /= rl;
        right[2] /= rl;
        const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2],
                             fwd[0] * right[1] - fwd[1] * right[0]};

        const float w = vp->Size.x;
        const float h = vp->Size.y;
        const float tanHalfY = std::tan(kFovY * 0.5f);
        const float aspect = w / h;
        const auto project = [&](const float p[3], ImVec2& out) -> bool
        {
            const float vx = p[0] - cp.x;
            const float vy = p[1] - cp.y;
            const float vz = p[2] - cp.z;
            const float cxv = vx * right[0] + vy * right[1] + vz * right[2];
            const float cyv = vx * up[0] + vy * up[1] + vz * up[2];
            const float czv = vx * fwd[0] + vy * fwd[1] + vz * fwd[2];
            if (czv <= kNearZ)
                return false;
            const float ndcX = cxv / (czv * tanHalfY * aspect);
            const float ndcY = cyv / (czv * tanHalfY);
            out.x = vp->Pos.x + (ndcX * 0.5f + 0.5f) * w;
            out.y = vp->Pos.y + (0.5f - ndcY * 0.5f) * h;
            return true;
        };

        // Bobbing diamond + label + distance (ping-marker spirit, own copy).
        const float bob = 0.25f * std::sin(static_cast<float>(m_clock) * 2.4f);
        const float markerPos[3] = {anchorX, m_ctx->world->TerrainHeightAt(anchorX, anchorZ) + 3.0f + bob, anchorZ};
        ImVec2 at;
        if (!project(markerPos, at))
            return;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(m_clock) * 3.1f);
        const int alpha = static_cast<int>(210.0f * pulse);
        const float s = 9.0f;
        const ImVec2 pts[4] = {ImVec2(at.x, at.y - s), ImVec2(at.x + s, at.y), ImVec2(at.x, at.y + s),
                               ImVec2(at.x - s, at.y)};
        fg->AddQuadFilled(pts[0], pts[1], pts[2], pts[3], IM_COL32(140, 220, 255, alpha / 2));
        fg->AddQuad(pts[0], pts[1], pts[2], pts[3], IM_COL32(170, 235, 255, alpha), 1.5f);
        TFUi::AddTextCentered(fg, 14.0f, ImVec2(at.x, at.y + s + 12.0f), IM_COL32(170, 235, 255, alpha), anchorLabel);
        const float dx = pos[0] - anchorX;
        const float dz = pos[2] - anchorZ;
        char dist[24];
        std::snprintf(dist, sizeof(dist), "%.0f m", std::sqrt(dx * dx + dz * dz));
        TFUi::AddTextCentered(fg, 12.0f, ImVec2(at.x, at.y + s + 26.0f), IM_COL32(150, 190, 215, alpha), dist);
    }

#else // !SPARK_HAS_IMGUI

    void TFTutorial::RenderUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
