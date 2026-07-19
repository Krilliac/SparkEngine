/**
 * @file TFWeaponServerFx.cpp
 * @brief TFWeaponSystem server fx broadcasts: the 0x54F4 remote-fire fx for
 *        validated shots and the 0x54F5 authoritative impact fx, both with
 *        per-shooter rate caps, plus the refined terrain-hit march the impact
 *        path uses. Split from TFWeaponServer.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFImpactFx.h" // W11 impact-broadcast: in-process listen-host route
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFFireFxProtocol.h" // W9 remote-fire-events: 0x54F4 S->C fx broadcast
#include "World/TFWorldSetup.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // W9 remote-fire-events: 0x54F4 fx broadcast (Net/TFFireFxProtocol.h)
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::ServerBroadcastRemoteFireFx(ShooterState& st, PlayerId shooter, EntityId shooterPawn,
                                                     WeaponId weapon, const float muzzle[3], const float dir[3])
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return; // Standalone / client role: no remote clients can exist
        const auto& clients = nm.GetClients();
        if (clients.empty())
            return;

        // Per-shooter rate cap (~10/s). The stamp only advances when a message
        // is actually delivered below, so shooters with nobody in range never
        // burn their budget ("skip if no clients in range").
        const double now = ServerNow();
        if (now - st.lastFireFxSent < kTFRemoteFireFxMinIntervalSec)
            return;

        const TF_RemoteFireFx fx = TF_RemoteFireFx::From(shooterPawn, weapon, muzzle, dir);
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFFxMsg_RemoteFire);
        msg.channel = Spark::Net::ChannelType::Unreliable; // presentational; drops are free
        msg.payload.resize(sizeof(fx));
        std::memcpy(msg.payload.data(), &fx, sizeof(fx));

        constexpr float kRange2 = kTFRemoteFireFxRangeM * kTFRemoteFireFxRangeM;
        bool sentAny = false;
        for (const auto& [clientId, info] : clients)
        {
            if (clientId == shooter || info.state != Spark::Net::ConnectionState::Connected)
                continue; // never echo the shooter's own shot back
            PawnInfo listener;
            if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(clientId, listener))
                continue; // not entered world / no pawn yet — nothing to hear with
            if (WeaponMath::Dist2(listener.pos, muzzle) > kRange2)
                continue;
            nm.SendToClient(clientId, msg);
            sentAny = true;
        }
        if (sentAny)
            st.lastFireFxSent = now;
#else
        (void)st;
        (void)shooter;
        (void)shooterPawn;
        (void)weapon;
        (void)muzzle;
        (void)dir;
#endif
    }

    // ---------------------------------------------------------------------------
    // W11 impact-broadcast: 0x54F5 authoritative impact fx (Net/TFFireFxProtocol.h)
    // ---------------------------------------------------------------------------

    bool TFWeaponSystem::ImpactFxCapOpen(PlayerId shooter) const
    {
        const auto it = m_shooters.find(shooter);
        if (it == m_shooters.end())
            return true;
        return ServerNow() - it->second.lastImpactFxSent >= kTFImpactFxMinIntervalSec;
    }

    float TFWeaponSystem::TerrainHitT(const float origin[3], const float dir[3], float dist) const
    {
        // Same 1 m march as TerrainBlocked, plus the bisection refine the client
        // guess-trace used (TFImpactFx precedent) so the puff sits ON the
        // surface instead of up to 1 m past it.
        if (!m_ctx || !m_ctx->world)
            return -1.0f;
        const float step = 1.0f;
        for (float t = step; t < dist; t += step)
        {
            const float y = origin[1] + dir[1] * t;
            if (y < m_ctx->world->TerrainHeightAt(origin[0] + dir[0] * t, origin[2] + dir[2] * t))
            {
                float lo = t - step;
                float hi = t;
                for (int i = 0; i < 5; ++i)
                {
                    const float mid = 0.5f * (lo + hi);
                    const float my = origin[1] + dir[1] * mid;
                    if (my < m_ctx->world->TerrainHeightAt(origin[0] + dir[0] * mid, origin[2] + dir[2] * mid))
                        hi = mid;
                    else
                        lo = mid;
                }
                return 0.5f * (lo + hi);
            }
        }
        return -1.0f;
    }

    void TFWeaponSystem::ServerBroadcastImpactFx(PlayerId shooter, const float point[3], uint8_t surface)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;

        // Per-shooter rate cap (~8/s; multi-pellet shotguns collapse to one
        // puff per window). The stamp only advances when something was actually
        // delivered below — same semantics as the 0x54F4 fire-fx cap.
        ShooterState& st = m_shooters[shooter];
        const double now = ServerNow();
        if (now - st.lastImpactFxSent < kTFImpactFxMinIntervalSec)
            return;

        bool delivered = false;

        // In-process route: the listen host / standalone player never receives
        // 0x54F5 (a server cannot message itself), so bots' and remote players'
        // impacts near the host feed TFImpactFx directly. The local shooter is
        // excluded — their puff is the immediate OnLocalShot prediction.
        if (m_ctx->HasLocalPlayer() && m_ctx->localPlayer != shooter)
            delivered |= TFImpactFx::Get().OnServerImpact(*m_ctx, point, surface);

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && !nm.GetClients().empty())
        {
            const TF_ImpactFx fx = TF_ImpactFx::From(point, static_cast<TFImpactSurface>(surface));
            Spark::Net::NetworkMessage msg;
            msg.type = static_cast<Spark::Net::MessageType>(kTFFxMsg_ImpactFx);
            msg.channel = Spark::Net::ChannelType::Unreliable; // presentational; drops are free
            msg.payload.resize(sizeof(fx));
            std::memcpy(msg.payload.data(), &fx, sizeof(fx));

            constexpr float kRange2 = kTFImpactFxRangeM * kTFImpactFxRangeM;
            for (const auto& [clientId, info] : nm.GetClients())
            {
                if (clientId == shooter || info.state != Spark::Net::ConnectionState::Connected)
                    continue; // the shooter's own puff is client-predicted
                PawnInfo listener;
                if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(clientId, listener))
                    continue; // not entered world / no pawn yet — nothing to see with
                if (WeaponMath::Dist2(listener.pos, point) > kRange2)
                    continue;
                nm.SendToClient(clientId, msg);
                delivered = true;
            }
        }
#endif
        if (delivered)
            st.lastImpactFxSent = now;
    }

} // namespace Terrafront
