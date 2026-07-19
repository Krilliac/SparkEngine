/**
 * @file TFSpectatorUi.cpp
 * @brief TFSpectator ImGui overlay: the SPECTATING / KILLED BY / FREE CAMERA
 *        label, LMB/RMB cycle-click latching, the killcam signal-lost
 *        vignette, the debug panel, and the roster name lookup. Split from
 *        TFSpectator.cpp; lifecycle and target selection stay there, and the
 *        camera drive lives in TFSpectatorDrive.cpp.
 */
#include "Game/TFSpectator.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Net/TFClientNet.h" // kTFLocalHostPlayer (HOST label fallback)
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------------

    void TFSpectator::ResolveTargetName(PlayerId player, char* out, size_t outSize) const
    {
        std::string roster;
        if (m_ctx && m_ctx->social && m_ctx->social->NameOfPlayer(player, roster) && !roster.empty())
        {
            std::snprintf(out, outSize, "%s", roster.c_str());
            return;
        }
        // Scoreboard-parity fallback (bots, pre-roster joiners).
        if (player == kTFLocalHostPlayer)
            std::snprintf(out, outSize, "HOST");
        else
            std::snprintf(out, outSize, "P%u", player);
    }

#ifdef SPARK_HAS_IMGUI

    void TFSpectator::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;

        // Latch for Update's free-cam RMB-look gate (Update runs outside the
        // ImGui frame, so io state must be sampled here).
        ImGuiIO& io = ImGui::GetIO();
        m_uiWantsMouse = io.WantCaptureMouse;

        if (!m_active)
            return;

        // Cycle clicks — read here (not in Update) so a click on the DEPLOY
        // SCREEN / MAP buttons or the death-recap panel never also cycles.
        // (KillcamFollow has no cycling — LMB/RMB stay inert during it.)
        if (m_mode == Mode::Follow && !io.WantCaptureMouse && !FullscreenUiOpen())
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false))
                m_pendingCycle = 1;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right, false))
                m_pendingCycle = -1;
        }

        // Killcam lane: dim vignette once the killer's pose has gone
        // unresolvable (dead/disconnected) — a background-layer fade so it
        // never blocks the DEPLOY/MAP buttons or the recap panel underneath.
        if (m_mode == Mode::KillcamFollow && m_killerGoneFade > 0.0f)
        {
            ImGuiViewport* fvp = ImGui::GetMainViewport();
            ImGui::GetBackgroundDrawList()->AddRectFilled(
                fvp->Pos, ImVec2(fvp->Pos.x + fvp->Size.x, fvp->Pos.y + fvp->Size.y),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, m_killerGoneFade * 0.35f)));
        }

        // Hide the label under the fullscreen screens (DrawDeathActions /
        // TFDeathRecap precedent) — the map and deploy UIs own the display.
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.84f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.42f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("##tf_spectator", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        if (m_mode == Mode::Follow)
        {
            char name[48];
            ResolveTargetName(m_target, name, sizeof(name));
            ImGui::TextColored(ImVec4(0.43f, 0.92f, 0.55f, 0.95f), "SPECTATING  %s", name);
            PawnInfo pi{};
            if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_target, pi) && pi.alive)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.88f, 0.90f), "  HP %.0f  SH %.0f", pi.health, pi.shield);
            }
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 0.75f), "LMB next squadmate   RMB previous");
        }
        else if (m_mode == Mode::KillcamFollow)
        {
            char name[48];
            ResolveTargetName(m_target, name, sizeof(name));
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 0.95f), "KILLED BY  %s", name);
            if (!m_lastPoseValid)
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.68f, 0.85f), "(signal lost)");
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 0.75f), "ESC to skip   %.0fs",
                               std::max(m_killcamTimer, 0.0f));
        }
        else
        {
            ImGui::TextColored(ImVec4(0.65f, 0.80f, 0.95f, 0.95f), "FREE CAMERA  (no squadmate alive)");
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.78f, 0.75f),
                               "W/S fly, A/D strafe, hold RMB to look   (%.0f m tether)",
                               static_cast<double>(kTFSpectFreeRangeM));
        }

        ImGui::End();
    }

    void TFSpectator::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Spectator"))
            return;
        const char* modeName = m_mode == Mode::Follow ? "follow" : (m_mode == Mode::KillcamFollow ? "killcam" : "free");
        ImGui::Text("active:%d  mode:%s  target:%u  cycles:%u", m_active ? 1 : 0, modeName, m_target, m_cycles);
        ImGui::Text("death:(%.1f %.1f %.1f)  cam:(%.1f %.1f %.1f)  free:(%.1f %.1f %.1f)", m_deathPos[0], m_deathPos[1],
                    m_deathPos[2], m_camPos[0], m_camPos[1], m_camPos[2], m_freePos[0], m_freePos[1], m_freePos[2]);
        ImGui::Text("killcam: shown:%d grace:%.2f timer:%.2f fade:%.2f pendingKiller:%u pendingFresh:%d",
                    m_killcamShownThisLife ? 1 : 0, m_killcamGraceTimer, m_killcamTimer, m_killerGoneFade,
                    m_pendingKiller, m_pendingKillerFresh ? 1 : 0);
    }

#else // !SPARK_HAS_IMGUI — headless: no label, no cycle clicks (no ImGui io)

    void TFSpectator::RenderUI() {}
    void TFSpectator::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
