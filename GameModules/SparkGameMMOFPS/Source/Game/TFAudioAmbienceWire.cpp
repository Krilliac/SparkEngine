/**
 * @file TFAudioAmbienceWire.cpp
 * @brief TFAudioAmbience wire half: the W9 remote-fire client fx seam
 *        (ClientOnRemoteFire) plus the pure-client 0x54F4/0x54F5
 *        NetworkManager handler lifecycle. Split from TFAudioAmbience.cpp
 *        (TFOutfitSystemWire pattern); the beds/one-shot layers stay there.
 */
#include "Game/TFAudioAmbience.h"

#include "Data/TFDataTables.h"
#include "Game/TFImpactFx.h" // impact-fx lane (W10): puff at the remote tracer endpoint
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h" // W9: kEyeHeightM (muzzle -> pawn-pos rebase)
#include "Game/TFWeaponSystem.h"
#include "Net/TFFireFxProtocol.h" // W9 remote-fire-events: 0x54F4 wire struct
#include "World/TFWorldSetup.h"   // W9: SpawnMuzzleFx (remote flash quad)
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // W9 remote-fire-events: client fx seam + 0x54F4 handler lifecycle
    // ---------------------------------------------------------------------------

    void TFAudioAmbience::ClientOnRemoteFire(const float muzzlePos[3], const float dirUnit[3], WeaponId weaponId,
                                             EntityId shooterEntity)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        // Brief world-space flash quad at the muzzle — same short-lived fx pool
        // (and reap/cap) as local shots; safe client-side (no authority path).
        if (m_ctx->world)
        {
            m_ctx->world->SpawnMuzzleFx(muzzlePos, dirUnit);
            // W11 impact-broadcast: the impact puff no longer rides this seam —
            // the W10 client guess-trace was replaced by the authoritative
            // 0x54F5 TF_ImpactFx point (handler below), so tracer flash and
            // puff can arrive independently (both unreliable, both cosmetic).
        }

        // Audio + combat heat: converge on the W8 listen-host bucket logic in
        // TFWeaponSystem::ClientOnRemoteFire (near = weapon's own clip, far =
        // faction distant tail with the concurrent-tail cap). It also bumps
        // RemoteFireHeat(), which Update() already max-merges into m_activity —
        // that IS the combat-heat hook; no second heat path.
        if (!m_ctx->weapons || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        if (!m_ctx->data->GetWeapon(weaponId))
            return; // unknown weapon id (stale tables / bad packet) — flash only

        // Resolve faction/owner from the replicated pawn mirror; a not-yet-
        // mirrored shooter degrades to the common distant tail.
        PawnInfo shooter{};
        shooter.entity = shooterEntity;
        shooter.owner = kInvalidPlayer;
        shooter.faction = FactionId::None;
        if (m_ctx->players)
            m_ctx->players->GetPawnByEntity(shooterEntity, shooter);

        // The wire muzzle pos is the authoritative fire origin; the bucket
        // logic measures pawn.pos + eye height, so re-base onto the muzzle.
        shooter.pos[0] = muzzlePos[0];
        shooter.pos[1] = muzzlePos[1] - WeaponMath::kEyeHeightM;
        shooter.pos[2] = muzzlePos[2];

        const WeaponDef def = m_ctx->data->ResolveWeapon(weaponId, shooter.faction);
        m_ctx->weapons->ClientOnRemoteFire(shooter.owner, shooter, def);
    }

#ifdef ENABLE_NETWORKING

    bool TFAudioAmbience::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx && m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFAudioAmbience::EnsureNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(kTFFxMsg_RemoteFire),
                           [this](const NetworkMessage& m) { OnNetRemoteFireFx(m.payload.data(), m.payload.size()); });

        // W11 impact-broadcast: 0x54F5 authoritative impact point -> the
        // surface-flavored puff (Game/TFImpactFx). Registered here because this
        // system already owns the pure-client fx handler lifecycle (and its
        // Shutdown no-op replacement keeps module unload dangle-free).
        nm.RegisterHandler(static_cast<MessageType>(kTFFxMsg_ImpactFx),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_ImpactFx) || !m_ctx)
                                   return; // malformed — drop
                               TF_ImpactFx fx;
                               std::memcpy(&fx, m.payload.data(), sizeof(fx));
                               float pos[3];
                               fx.DecodePos(pos);
                               TFImpactFx::Get().OnServerImpact(*m_ctx, pos, fx.surface);
                           });

        m_netHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] remote-fire/impact fx handlers registered");
    }

    void TFAudioAmbience::ReleaseNetHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFSocialSystem pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFFxMsg_RemoteFire),
                           [](const Spark::Net::NetworkMessage&) {});
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFFxMsg_ImpactFx),
                           [](const Spark::Net::NetworkMessage&) {});
        m_netHandlers = false;
    }

    void TFAudioAmbience::OnNetRemoteFireFx(const void* data, size_t size)
    {
        if (size != sizeof(TF_RemoteFireFx))
            return; // malformed — drop
        TF_RemoteFireFx fx;
        std::memcpy(&fx, data, sizeof(fx));

        float pos[3];
        float dir[3];
        fx.DecodePos(pos);
        fx.DecodeDir(dir);
        ClientOnRemoteFire(pos, dir, static_cast<WeaponId>(fx.weaponId), static_cast<EntityId>(fx.shooterEntity));
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
