/**
 * @file TFVehicleClient.cpp
 * @brief W3 vehicles — client UX half of TFVehicleSystem: terminal purchase
 *        menu (E), vehicle enter/exit prompts (E), Aegis deploy toggle (B),
 *        and the debug panel. All ImGui bodies compile only under
 *        SPARK_HAS_IMGUI (module rule); requests route directly into the
 *        server methods on authority roles and over the wire otherwise.
 *
 *        W10 (vehicle-hud lane): the seated hp bar grew into the full cockpit
 *        widget in UI/TFVehicleHUD.h/.cpp (name/HP/speed/seat diagram/deploy/
 *        altitude + per-seat crosshair), lazily constructed here and rendered
 *        from RenderDebugUI. Seat swap while seated: keys 1-8 request that
 *        seat, F cycles to the next free one — both send TFMsg::VehicleEnter
 *        for the CURRENT vehicle (see TFVehicleSystem.h W10 notes; the server
 *        validates strictly). The enter prompt now lists the vehicle name,
 *        free-seat count and the seat role you would take.
 *
 *        Known W3 caveat (documented in the header): while the purchase menu
 *        is open the mouse is released (TFMapScreen pattern), but TFClientNet
 *        still samples fire — a stray click can discharge the weapon at the
 *        terminal. Cosmetic only; TF-W4 adds a shared "UI owns input" flag.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFUiSounds.h" // W10 audio-wave-2: terminal bleeps
#include "Game/TFVehiclePhysics.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFVehicleHUD.h"
#include "World/TFRegionSystem.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

    namespace
    {

        constexpr float kInteractDebounceSec = 0.25f;
        constexpr float kShopAutoCloseM = 30.0f; // walk away -> menu closes

    } // namespace

    // ---------------------------------------------------------------------------
    // Identity / pawn helpers
    // ---------------------------------------------------------------------------

    PlayerId TFVehicleSystem::LocalPlayerId() const
    {
        if (!m_ctx)
            return kInvalidPlayer;
        if (m_ctx->localPlayer != kInvalidPlayer)
            return m_ctx->localPlayer;
        return m_ctx->clientNet ? m_ctx->clientNet->LocalPlayerId() : kInvalidPlayer;
    }

    bool TFVehicleSystem::GetSeatOf(PlayerId player, EntityId& outVehicle, uint8_t& outSeatIdx) const
    {
        // Authoritative seat map first (server roles), replicated tables second
        // (pure clients) — the same two-source pattern as IsSeated.
        if (auto it = m_seatOf.find(player); it != m_seatOf.end())
        {
            outVehicle = it->second.vehicle;
            outSeatIdx = it->second.seatIdx;
            return true;
        }
        for (const auto& [entity, seats] : m_mirrorSeats)
            for (uint8_t i = 0; i < seats.seatCount && i < 8; ++i)
                if (seats.seats[i] == player)
                {
                    outVehicle = entity;
                    outSeatIdx = i;
                    return true;
                }
        return false;
    }

    bool TFVehicleSystem::LocalPlayerPawn(float outPos[3], bool& outAlive) const
    {
        outAlive = false;
        const PlayerId pid = LocalPlayerId();
        if (pid == kInvalidPlayer || !m_ctx->players)
            return false;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(pid, pawn))
            return false;
        outPos[0] = pawn.pos[0];
        outPos[1] = pawn.pos[1];
        outPos[2] = pawn.pos[2];
        outAlive = pawn.alive;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Requests (direct on authority, wire on pure clients)
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ClientRequestSeatOp(EntityId vehicle, uint8_t seatIdx, bool enter)
    {
        TF_VehicleSeatOp op{};
        op.vehicleEntity = vehicle;
        op.seatIndex = seatIdx;
        if (m_ctx->IsAuthority())
        {
            // final-review #3 follow-up (gate defense-in-depth): this authority-
            // path call goes straight into the server handler, bypassing the
            // RouteClientMessage enter-world gate networked VehicleEnter/
            // VehicleExit traffic is held to. Mirror that gate here so an
            // un-entered-world local host can't seat/exit either. The gate only
            // exists under ENABLE_NETWORKING (RouteClientMessage/onboarding are
            // both ifdef'd out otherwise, and m_enteredWorld would never be
            // populated), so don't apply it to builds without networking.
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(LocalPlayerId()))
                return;
#endif
            ServerHandleSeatOp(LocalPlayerId(), op, enter);
            return;
        }
        if (m_ctx->clientNet && m_ctx->clientNet->IsConnected())
            m_ctx->clientNet->SendMsg(enter ? TFMsg::VehicleEnter : TFMsg::VehicleExit, &op, sizeof(op));
    }

    void TFVehicleSystem::ClientRequestDeploy(EntityId vehicle, bool deploy)
    {
        TF_AegisDeploy msg{};
        msg.vehicleEntity = vehicle;
        msg.deploy = deploy ? 1 : 0;
        if (m_ctx->IsAuthority())
        {
            // final-review #3 follow-up (gate defense-in-depth): same authority-
            // path gate as ClientRequestSeatOp above -- mirror the
            // RouteClientMessage enter-world gate networked AegisDeploy traffic
            // is held to (ENABLE_NETWORKING-only; see the comment above).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(LocalPlayerId()))
                return;
#endif
            ServerHandleAegisDeploy(LocalPlayerId(), msg);
            return;
        }
        if (m_ctx->clientNet && m_ctx->clientNet->IsConnected())
            m_ctx->clientNet->SendMsg(TFMsg::AegisDeploy, &msg, sizeof(msg));
    }

    void TFVehicleSystem::ClientRequestPurchase(VehicleId vehId)
    {
        if (m_ctx->IsAuthority())
        {
            ServerPurchaseVehicle(LocalPlayerId(), vehId);
            return;
        }
        if (m_ctx->clientNet && m_ctx->clientNet->IsConnected())
        {
            // Vehicle-channel id (0x54FF) rides the same TFClientNet send path;
            // the cast is safe — SendMsg only forwards the raw uint16.
            TF_VehPurchase req{};
            req.vehId = static_cast<uint8_t>(vehId);
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFVehMsg_Purchase), &req, sizeof(req));
        }
    }

    void TFVehicleSystem::SetShopOpen(bool open)
    {
        if (m_shopOpen == open)
            return;
        m_shopOpen = open;
        // TFMapScreen pattern: the menu owns the mouse while open.
        InputManager* input = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;
        if (open)
        {
            if (input->IsMouseCaptured())
                input->CaptureMouse(false);
        }
        else
        {
            bool alive = false;
            float pos[3];
            if (LocalPlayerPawn(pos, alive) && alive)
                input->CaptureMouse(true);
        }
    }

    // ---------------------------------------------------------------------------
    // Per-frame UX state machine (all roles with a local player)
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ClientUpdateUX(float dt)
    {
        m_interactDebounce = std::max(0.0f, m_interactDebounce - dt);
        m_promptVehicle = 0;

        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        // Fullscreen UIs own the keys.
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
        {
            SetShopOpen(false);
            return;
        }

        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        const PlayerId pid = LocalPlayerId();
        float pawnPos[3];
        bool alive = false;
        if (!input || pid == kInvalidPlayer || !LocalPlayerPawn(pawnPos, alive) || !alive)
        {
            SetShopOpen(false);
            return;
        }

        const bool seated = IsSeated(pid);

        // ---- seated: E exits, B toggles Aegis deploy, 1-8/F swap seats ---------
        if (seated)
        {
            SetShopOpen(false);
            EntityId myVeh = 0;
            uint8_t mySeat = 0;
            GetSeatOf(pid, myVeh, mySeat);

            if (input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                ClientRequestSeatOp(myVeh, mySeat, false);
                return;
            }
            if (mySeat == 0 && input->WasKeyPressed('B') && m_interactDebounce <= 0.0f)
            {
                TFVehicleInfo info;
                const VehicleDef* def = nullptr;
                if (GetVehicleInfo(myVeh, info))
                    def = DefOf(info.vehId);
                if (def && def->hasDeploySpawn)
                {
                    m_interactDebounce = kInteractDebounceSec;
                    ClientRequestDeploy(myVeh, !info.deployed);
                }
            }

            // W10 seat swap: 1-8 request that seat directly, F cycles forward to
            // the next free one. Client-side pre-check against the local seat
            // view keeps the wire quiet for obviously-invalid requests; the
            // server re-validates strictly (see ServerHandleSeatOp). Known key
            // overlap (accepted): 1-3 also drive TFWeaponSystem slot switching
            // and F drives class abilities — both harmless while seated (seat
            // weapons override the infantry loadout; TF-W11 candidate: a shared
            // seated input-suppression gate).
            if (m_interactDebounce <= 0.0f && myVeh != 0)
            {
                TFVehicleInfo info;
                if (GetVehicleInfo(myVeh, info))
                {
                    int want = -1;
                    for (uint8_t i = 0; i < info.seatCount && i < 8; ++i)
                        if (i != mySeat && info.seats[i] == kInvalidPlayer &&
                            input->WasKeyPressed('1' + static_cast<int>(i)))
                            want = i;
                    if (want < 0 && input->WasKeyPressed('F'))
                    {
                        for (uint8_t step = 1; step < info.seatCount; ++step)
                        {
                            const uint8_t i = static_cast<uint8_t>((mySeat + step) % info.seatCount);
                            if (i < 8 && info.seats[i] == kInvalidPlayer)
                            {
                                want = i;
                                break;
                            }
                        }
                    }
                    if (want >= 0)
                    {
                        m_interactDebounce = kInteractDebounceSec;
                        ClientRequestSeatOp(myVeh, static_cast<uint8_t>(want), true); // enter-while-seated == swap
                    }
                }
            }
            return;
        }

        // ---- on foot: nearest enterable vehicle wins the E prompt --------------
        const FactionId myFaction = m_ctx->localFaction;
        float bestD2 = 1.0e30f;
        ForEachVehicle(
            [&](const TFVehicleInfo& info)
            {
                if (info.hp <= 0.0f || info.faction != myFaction)
                    return;
                int freeSeat = -1;
                for (uint8_t i = 0; i < info.seatCount && i < 8; ++i)
                    if (info.seats[i] == kInvalidPlayer)
                    {
                        freeSeat = i;
                        break;
                    }
                if (freeSeat < 0)
                    return;
                const float reach = VehicleRadius(info.vehId) + kTFVehEnterRangeM;
                const float dx = info.pos[0] - pawnPos[0];
                const float dz = info.pos[2] - pawnPos[2];
                const float d2 = dx * dx + dz * dz;
                if (d2 <= reach * reach && d2 < bestD2)
                {
                    bestD2 = d2;
                    m_promptVehicle = info.entity;
                    m_promptSeat = static_cast<uint8_t>(freeSeat);
                }
            });

        if (m_promptVehicle != 0)
        {
            SetShopOpen(false);
            if (input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                ClientRequestSeatOp(m_promptVehicle, m_promptSeat, true);
            }
            return;
        }

        // ---- terminal shop ------------------------------------------------------
        float pad[2];
        const bool nearTerminal =
            myFaction != FactionId::None &&
            FindTerminal(pawnPos, myFaction, m_shopOpen ? kShopAutoCloseM : kTFVehTerminalPromptM, pad);
        if (nearTerminal)
        {
            m_shopTerminal[0] = pad[0];
            m_shopTerminal[1] = pad[1];
            if (input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                SetShopOpen(!m_shopOpen);
                TFUiSounds_Play(m_ctx, m_shopOpen ? TFUiBleep::Open : TFUiBleep::Close); // W10 audio-wave-2
            }
        }
        else
        {
            SetShopOpen(false);
        }
    }

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
