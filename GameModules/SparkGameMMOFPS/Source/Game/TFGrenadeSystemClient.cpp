/**
 * @file TFGrenadeSystemClient.cpp
 * @brief TFGrenadeSystem client half: G-key throw entry + local count mirror,
 *        the replicated grenade/boom/smoke/flash mirror store with arc
 *        extrapolation, the self-registering net handlers and the debug UI.
 *        Split from TFGrenadeSystem.cpp; the shared view-direction helper and
 *        boom lifetime live in TFGrenadeSystemInternal.h.
 */
#include "Game/TFGrenadeSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBallistics.h"
#include "Game/TFGrenadeSystemInternal.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h" // TFMsg (SendMsg id cast)
#include "Net/TFServerSim.h"
#include "UI/TFChatWindow.h"
#include "UI/TFHUD.h"
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#include "Audio/AudioEngine.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    using namespace GrenadeSysDetail;

    namespace
    {
        // Boom presentation (client only).
        constexpr const char* kBoomAudio = "Audio/MMOFPS/vehicles/explosion.wav";
        constexpr float kBoomAudioRangeM = 90.0f;

        // Client-side mirror pruning: fuse + net slack. A grenade whose boom
        // packet was lost (unreliable updates can't revive it) dies here.
        constexpr float kClientStaleSec = kTFGrenadeFuseSec + 1.5f;
    } // namespace

    // ---------------------------------------------------------------------------
    // Client: G key + count mirror
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ClientReseedLocalCount()
    {
        PawnInfo pi{};
        const bool alive = m_ctx->players && m_ctx->localPlayer != kInvalidPlayer &&
                           m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) && pi.alive;
        if (alive && !m_wasAliveLocal && m_ctx->data && m_ctx->data->IsLoaded())
        {
            const ClassDef* cls = m_ctx->data->GetClass(pi.cls);
            m_localRemaining = cls ? cls->grenades : 0;
        }
        m_wasAliveLocal = alive;
    }

    void TFGrenadeSystem::ClientPollThrowKey()
    {
        if (!m_ctx || !m_ctx->InWorld() || m_ctx->localPlayer == kInvalidPlayer)
            return;
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;
        // Same input-suppression gate set as TFAbilitySystem::ClientPollAbilityKey.
        const bool uiOpen =
            (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
            (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) || (m_ctx->hud && m_ctx->hud->IsChatOpen()) ||
            (m_ctx->chatWindow && m_ctx->chatWindow->IsOpen()) || (m_ctx->socialPanel && m_ctx->socialPanel->IsOpen());
        if (uiOpen)
            return;
        PawnInfo pi{};
        if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) || !pi.alive)
            return;
        if (!input->WasKeyPressed(m_throwVk))
            return;
        if (m_localRemaining == 0)
            return; // known-empty: don't spam the server (unknown (-1) still sends)

        SendThrow(pi.yaw, pi.pitch);
    }

    void TFGrenadeSystem::SendThrow(float viewYaw, float viewPitch)
    {
        if (!m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;
        if (m_ctx->IsAuthority())
        {
            // Mirror the RouteClientMessage enter-world gate for the direct
            // authority path (TFAbilitySystem::SendRequest defense-in-depth).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            float dir[3];
            ViewDir(viewYaw, viewPitch, dir);
            ServerTryThrow(m_ctx->localPlayer, dir);
        }
        else if (m_ctx->clientNet)
        {
            TF_GrenadeThrow req{};
            req.viewYaw = viewYaw;
            req.viewPitch = viewPitch;
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgGrenadeThrow), &req, sizeof(req));
        }
    }

    // ---------------------------------------------------------------------------
    // Client: mirror store + presentation state
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ClientHandleSpawn(const TF_GrenadeSpawn& msg)
    {
        ClientGrenade& g = m_clientGrenades[msg.grenadeId];
        g.pos[0] = msg.posQX / kTFGrenadePosScale;
        g.pos[1] = msg.posQY / kTFGrenadePosScale;
        g.pos[2] = msg.posQZ / kTFGrenadePosScale;
        g.vel[0] = msg.velQX / kTFGrenadeVelScale;
        g.vel[1] = msg.velQY / kTFGrenadeVelScale;
        g.vel[2] = msg.velQZ / kTFGrenadeVelScale;
        g.resting = false;
        g.staleSec = 0.0f;

        if (m_ctx && msg.player == m_ctx->localPlayer)
            m_localRemaining = msg.remaining; // authoritative HUD count echo
    }

    void TFGrenadeSystem::ClientHandleUpdate(const TF_GrenadeUpdate& msg)
    {
        ClientGrenade& g = m_clientGrenades[msg.grenadeId]; // creates if the spawn raced
        g.pos[0] = msg.posQX / kTFGrenadePosScale;
        g.pos[1] = msg.posQY / kTFGrenadePosScale;
        g.pos[2] = msg.posQZ / kTFGrenadePosScale;
        g.vel[0] = msg.velQX / kTFGrenadeVelScale;
        g.vel[1] = msg.velQY / kTFGrenadeVelScale;
        g.vel[2] = msg.velQZ / kTFGrenadeVelScale;
        g.resting = (msg.flags & 1) != 0;
        g.staleSec = 0.0f;
    }

    void TFGrenadeSystem::ClientHandleBoom(const TF_GrenadeBoom& msg)
    {
        m_clientGrenades.erase(msg.grenadeId);

        BoomFx fx;
        fx.pos[0] = msg.posQX / kTFGrenadePosScale;
        fx.pos[1] = msg.posQY / kTFGrenadePosScale;
        fx.pos[2] = msg.posQZ / kTFGrenadePosScale;
        m_booms.push_back(fx);

        if (::AudioEngine* audio = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetAudio() : nullptr)
        {
            if (!m_boomAudioLoaded)
            {
                m_boomAudioLoaded = true;
                const std::string full = std::string("Assets/") + kBoomAudio;
                if (FAILED(audio->LoadSound(kBoomAudio, std::wstring(full.begin(), full.end()))))
                    SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] grenade boom audio load FAIL %s", kBoomAudio);
            }
            // Manual range gate on top of the 3D pan/attenuation so far-away
            // booms cost nothing.
            float lx = fx.pos[0], ly = fx.pos[1], lz = fx.pos[2];
            PawnInfo pi{};
            if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi))
            {
                lx = pi.pos[0];
                ly = pi.pos[1];
                lz = pi.pos[2];
            }
            const float dx = fx.pos[0] - lx, dy = fx.pos[1] - ly, dz = fx.pos[2] - lz;
            if (dx * dx + dy * dy + dz * dz <= kBoomAudioRangeM * kBoomAudioRangeM)
                audio->PlaySound3D(kBoomAudio, DirectX::XMFLOAT3(fx.pos[0], fx.pos[1], fx.pos[2]), 0.9f);
        }
    }

    // ---------------------------------------------------------------------------
    // loadout-depth wave: client mirror for flash / smoke
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ClientHandleFlash(const TF_FlashState& msg)
    {
        // Unicast to us specifically (SendToOwner), so no player-id check needed.
        m_localFlashDurationSec = msg.durationMs / 1000.0f;
        m_localFlashUntil = NowSec() + m_localFlashDurationSec;
    }

    void TFGrenadeSystem::ClientHandleSmoke(const TF_SmokeSpawn& msg)
    {
        SmokePuff puff;
        puff.pos[0] = msg.posQX / kTFGrenadePosScale;
        puff.pos[1] = msg.posQY / kTFGrenadePosScale;
        puff.pos[2] = msg.posQZ / kTFGrenadePosScale;
        puff.radiusM = msg.radiusQ / kTFGrenadePosScale;
        puff.life = std::max(0.1f, msg.durationMs / 1000.0f);
        puff.age = 0.0f;
        m_smokePuffs.push_back(puff);
    }

    void TFGrenadeSystem::ClientAdvanceSmoke(float dt)
    {
        if (dt <= 0.0f || m_smokePuffs.empty())
            return;
        for (SmokePuff& p : m_smokePuffs)
            p.age += dt;
        std::erase_if(m_smokePuffs, [](const SmokePuff& p) { return p.age >= p.life; });
    }

    void TFGrenadeSystem::ClientAdvance(float dt)
    {
        if (dt <= 0.0f)
            return;

        for (auto& [id, g] : m_clientGrenades)
        {
            (void)id;
            g.staleSec += dt;
            if (g.resting)
                continue;
            // Extrapolate the arc between 10 Hz corrections with the same
            // gravity the server integrates; terrain-clamp so the body never
            // sinks while waiting for the next correction.
            g.vel[1] -= Ballistics::kProjectileGravityMps2 * dt;
            g.pos[0] += g.vel[0] * dt;
            g.pos[1] += g.vel[1] * dt;
            g.pos[2] += g.vel[2] * dt;
            if (m_ctx && m_ctx->world)
            {
                const float h = m_ctx->world->TerrainHeightAt(g.pos[0], g.pos[2]);
                if (g.pos[1] < h + kTFGrenadeRadiusM)
                    g.pos[1] = h + kTFGrenadeRadiusM;
            }
        }
        std::erase_if(m_clientGrenades, [](const auto& kv) { return kv.second.staleSec > kClientStaleSec; });

        for (BoomFx& b : m_booms)
            b.age += dt;
        std::erase_if(m_booms, [](const BoomFx& b) { return b.age >= kBoomLifeSec; });
    }

    // ---------------------------------------------------------------------------
    // Net handlers (pure clients; TFAbilitySystem lifecycle pattern)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFGrenadeSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFGrenadeSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgGrenadeSpawn),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_GrenadeSpawn))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_GrenadeSpawn msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleSpawn(msg);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgGrenadeUpdate),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_GrenadeUpdate))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_GrenadeUpdate msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleUpdate(msg);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgGrenadeBoom),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_GrenadeBoom))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_GrenadeBoom msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleBoom(msg);
                           });
        // loadout-depth wave
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgFlashState),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_FlashState))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_FlashState msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleFlash(msg);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgSmokeSpawn),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_SmokeSpawn))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_SmokeSpawn msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleSmoke(msg);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] grenade mirror handlers registered");
    }

    void TFGrenadeSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with no-ops so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id :
             {kTFMsgGrenadeSpawn, kTFMsgGrenadeUpdate, kTFMsgGrenadeBoom, kTFMsgFlashState, kTFMsgSmokeSpawn})
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        }
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Grenades"))
            return;
        ImGui::Text("server live : %zu / %u", m_live.size(), kTFMaxLiveGrenades);
        ImGui::Text("throws      : %u ok, %u rejected", m_throws, m_rejected);
        ImGui::Text("detonations : %u", m_detonations);
        ImGui::Text("client view : %zu bodies, %zu booms, %zu smoke", m_clientGrenades.size(), m_booms.size(),
                    m_smokePuffs.size());
        ImGui::Text("local count : %d", m_localRemaining);
        ImGui::Text("bad packets : %u", m_badPackets);
        ImGui::Text("flashed now : %zu players, local %s", m_flashedUntil.size(), IsLocalFlashed() ? "YES" : "no");
#endif
    }

} // namespace Terrafront
