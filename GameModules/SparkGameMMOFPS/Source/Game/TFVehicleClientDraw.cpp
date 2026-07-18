/**
 * @file TFVehicleClientDraw.cpp
 * @brief W3 vehicles — client drawing half of TFVehicleClient.cpp: the E/B
 *        prompt overlays, the terminal purchase menu, the W10 cockpit HUD
 *        hand-off and the debug panel (RenderDebugUI hook). All ImGui bodies
 *        compile only under SPARK_HAS_IMGUI (module rule); headless builds
 *        keep empty stubs so the lifecycle contract holds. Split from
 *        TFVehicleClient.cpp per the repo file-size rule (TFHUDDraw pattern
 *        — same class, feature-owned translation units); the per-frame UX
 *        state machine and request plumbing stay there.
 */
#include "Game/TFVehicleSystem.h"

#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFUiSounds.h" // W10 audio-wave-2: terminal bleeps
#include "Game/TFVehiclePhysics.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFVehicleHUD.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <cstdio>
#include <memory>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Rendering (SPARK_HAS_IMGUI only)
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFVehicleSystem::RenderPromptsAndMenus()
    {
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        const PlayerId pid = LocalPlayerId();
        float pawnPos[3];
        bool alive = false;
        if (pid == kInvalidPlayer || !LocalPlayerPawn(pawnPos, alive) || !alive)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 promptAt(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.62f);
        char buf[128];

        const bool seated = IsSeated(pid);
        if (seated)
        {
            float hp = 0.0f, maxHp = 0.0f;
            const bool haveHp = GetSeatedVehicleHp(pid, hp, maxHp);
            (void)haveHp;
            std::snprintf(buf, sizeof(buf), "[E] Exit vehicle");
            TFUi::AddTextCentered(fg, 17.0f, promptAt, IM_COL32(230, 230, 230, 220), buf);

            // Driver of a deploy-capable vehicle gets the deploy hint.
            if (auto it = m_seatOf.find(pid); it != m_seatOf.end() && it->second.seatIdx == 0)
            {
                TFVehicleInfo info;
                if (GetVehicleInfo(it->second.vehicle, info))
                {
                    const VehicleDef* def = DefOf(info.vehId);
                    if (def && def->hasDeploySpawn)
                    {
                        std::snprintf(buf, sizeof(buf), "[B] %s Aegis spawn", info.deployed ? "Undeploy" : "Deploy");
                        TFUi::AddTextCentered(
                            fg, 15.0f, ImVec2(promptAt.x, promptAt.y + 22.0f),
                            info.deployed ? IM_COL32(140, 235, 160, 220) : IM_COL32(230, 230, 230, 200), buf);
                    }
                }
            }
            return; // shop/enter prompts never draw while seated
        }

        if (m_promptVehicle != 0)
        {
            TFVehicleInfo info;
            if (GetVehicleInfo(m_promptVehicle, info))
            {
                // W10 prompt polish: vehicle name + free-seat count + the seat
                // role you would take (all data already client-side).
                const VehicleDef* def = DefOf(info.vehId);
                int freeSeats = 0;
                for (uint8_t i = 0; i < info.seatCount && i < 8; ++i)
                    if (info.seats[i] == kInvalidPlayer)
                        ++freeSeats;
                const char* role = (def && static_cast<size_t>(m_promptSeat) < def->seats.size())
                                       ? def->seats[m_promptSeat].role.c_str()
                                       : (m_promptSeat == 0 ? "driver" : "seat");
                std::snprintf(buf, sizeof(buf), "[E] Enter %s as %s  -  %d/%u seats free",
                              def ? def->name.c_str() : "vehicle", role, freeSeats,
                              static_cast<unsigned>(info.seatCount));
                TFUi::AddTextCentered(fg, 17.0f, promptAt, IM_COL32(230, 230, 230, 220), buf);
            }
            return;
        }

        // Terminal prompt / purchase menu.
        float pad[2];
        if (m_ctx->localFaction == FactionId::None ||
            !FindTerminal(pawnPos, m_ctx->localFaction, kTFVehTerminalPromptM, pad))
        {
            if (!m_shopOpen)
                return;
        }
        if (!m_shopOpen)
        {
            std::snprintf(buf, sizeof(buf), "[E] Vehicle terminal");
            TFUi::AddTextCentered(fg, 17.0f, promptAt, IM_COL32(230, 230, 230, 220), buf);
            return;
        }

        // ---- purchase list -------------------------------------------------------
        const float w = 380.0f, h = 250.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + (vp->Size.y - h) * 0.5f),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::Begin("Vehicle Terminal##tf_vshop", &open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
        {
            // Wallet is authoritative-side only; pure clients see "-" (server
            // still validates every purchase).
            const PlayerId pid2 = LocalPlayerId();
            if (m_ctx->IsAuthority() && m_ctx->progression)
                ImGui::Text("Flux: %u", m_ctx->progression->FluxOf(pid2));
            else
                ImGui::TextDisabled("Flux: (server validated)");
            ImGui::Separator();

            for (const VehicleDef& def : m_ctx->data->AllVehicles())
            {
                if (!def.enabled)
                    continue;
                const uint32_t wallet =
                    (m_ctx->IsAuthority() && m_ctx->progression) ? m_ctx->progression->FluxOf(pid2) : 0xFFFFFFFFu;
                const bool affordable = wallet >= static_cast<uint32_t>(def.fluxCost);
                char label[128];
                std::snprintf(label, sizeof(label), "%s  -  %d flux##buy%u", def.name.c_str(), def.fluxCost,
                              static_cast<unsigned>(def.id));
                ImGui::BeginDisabled(!affordable);
                if (ImGui::Button(label, ImVec2(-1.0f, 0.0f)))
                {
                    ClientRequestPurchase(def.id);
                    TFUiSounds_Play(m_ctx, TFUiBleep::Confirm); // W10 audio-wave-2
                    open = false;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s%s", def.role.c_str(),
                                      def.hasDeploySpawn ? "  (deployable mobile spawn)" : "");
            }
            ImGui::Separator();
            ImGui::TextDisabled("[E] close");
        }
        ImGui::End();
        if (!open)
            SetShopOpen(false);
    }

    void TFVehicleSystem::RenderDebugUI()
    {
        if (m_ctx && m_ctx->HasLocalPlayer() && m_initialized)
        {
            // Player-facing overlays ride the module's only per-frame ImGui hook.
            RenderPromptsAndMenus();
            // W10: the seated cockpit widget (superseded the W3 hp-only bar).
            if (!m_vehicleHud)
                m_vehicleHud = std::make_unique<TFVehicleHUD>();
            m_vehicleHud->Render(*m_ctx, *this);
        }

        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Vehicles", &m_showDebug))
        {
            ImGui::Text("server vehicles : %zu   mirror: %zu", m_vehicles.size(), m_mirror.size());
            ImGui::Text("driving model   : %s (%zu hull bodies)", m_joltDrive ? "jolt" : "math",
                        m_joltDrive ? m_joltDrive->BodyCount() : static_cast<size_t>(0));
            ImGui::Text("seated players  : %zu", m_seatOf.size());
            ImGui::Text("purchases       : %u (rejected %u)", m_purchases, m_purchasesRejected);
            ImGui::Text("seat ops        : %u   destroyed: %u   bad packets: %u", m_seatOps, m_destroyed, m_badPackets);
            ImGui::Separator();
            ForEachVehicle(
                [](const TFVehicleInfo& v)
                {
                    int occupied = 0;
                    for (uint8_t i = 0; i < v.seatCount && i < 8; ++i)
                        if (v.seats[i] != kInvalidPlayer)
                            ++occupied;
                    ImGui::Text("veh %u kind=%u %s hp=%.0f/%.0f pos=(%.0f %.0f %.0f) seats %d/%u%s", v.entity,
                                static_cast<unsigned>(v.vehId), FactionTag(v.faction), v.hp, v.maxHp, v.pos[0],
                                v.pos[1], v.pos[2], occupied, static_cast<unsigned>(v.seatCount),
                                v.deployed ? " [DEPLOYED]" : "");
                });
        }
        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI — headless builds keep the state machine, drop the pixels

    void TFVehicleSystem::RenderPromptsAndMenus() {}
    void TFVehicleSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
