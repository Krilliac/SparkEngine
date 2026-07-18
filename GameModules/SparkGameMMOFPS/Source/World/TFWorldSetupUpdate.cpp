/**
 * @file TFWorldSetupUpdate.cpp
 * @brief TFWorldSetup frame driving: per-frame Update (network pump, weather,
 *        shot-FX aging), muzzle-FX spawning, client origin-rebase driving,
 *        FixedUpdate, Shutdown and the debug UI. Scene/terrain load lives in
 *        TFWorldSetup.cpp (same class, split per the repo file-size rules —
 *        mirrors the TFRegionSystem/-Net split).
 */
#include "World/TFWorldSetup.h"

#include "World/TFSanctuaryZone.h"
#include "World/TFWorldCollision.h"

#include "Game/TFPlayerSystem.h"

#include "Spark/IEngineContext.h"
#include "SceneManager/SceneManager.h"
#include "Engine/ECS/Components.h"
#include "Engine/World/WorldOriginSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "World/TFWeatherFx.h" // W12 weather-visuals: storm cycle + client visuals

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/AreaServer.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Frame driving
    // ---------------------------------------------------------------------------

    void TFWorldSetup::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

#ifdef ENABLE_NETWORKING
        if (m_netBooted)
        {
            // Single message pump for the module: TFWorldSetup booted the
            // NetworkManager, so it owns the Update (mirrors MMOWorldSetup).
            Spark::Net::NetworkManager::GetInstance().Update(deltaTime);
            if (m_ctx->IsAuthority())
                BridgeWorldServerSessions();
        }
#else
        (void)deltaTime;
#endif

        if (m_ctx->role == NetRole::Client)
            DriveOriginRebase();

        // W12 weather-visuals: server-owned dust-storm cycle + 0x547C sync +
        // pure-client handler poll (all roles; presentation reads the singleton).
        TFWeatherFx::Get().Update(*m_ctx, deltaTime);

        // audio-polish lane (W8): TFAudioAmbience owns the ambient beds
        // (sanctuary hum <-> wind crossfade); the old single-wind-loop starter
        // (MaybeStartAmbientAudio) was removed as dead code in W9.

        m_fxClock += deltaTime;
        // Reap expired shot effects (longest-lived component is the tracer).
        constexpr double kFxMaxLife = 0.10;
        m_shotFx.erase(std::remove_if(m_shotFx.begin(), m_shotFx.end(),
                                      [&](const ShotFx& fx) { return m_fxClock - fx.t0 > kFxMaxLife; }),
                       m_shotFx.end());
    }

    void TFWorldSetup::SpawnMuzzleFx(const float origin[3], const float dir[3])
    {
        if (!m_ctx->HasLocalPlayer())
            return;
        if (m_shotFx.size() > 64) // safety cap
            return;
        ShotFx fx;
        fx.t0 = m_fxClock;
        fx.origin = {origin[0], origin[1], origin[2]};
        fx.dir = {dir[0], dir[1], dir[2]};
        m_shotFx.push_back(fx);
    }

    void TFWorldSetup::DriveOriginRebase()
    {
        if (!m_origin || !m_ctx->engine || !m_ctx->players)
            return;
        if (m_ctx->localPlayer == kInvalidPlayer)
            return;

        World* world = m_ctx->engine->GetWorld();
        if (!world)
            return;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            return;

        m_origin->Update(world->GetRegistry(), DirectX::XMFLOAT3{pawn.pos[0], pawn.pos[1], pawn.pos[2]});
    }

    void TFWorldSetup::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // authoritative sim runs in TFServerSim (AreaServer tick)
    }

    void TFWorldSetup::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        StopNetworking();
#endif
        m_sanctuaryCollision.reset(); // removes its static bodies from the engine PhysicsSystem
        m_collision.reset();          // removes its static bodies from the engine PhysicsSystem
        m_camera.reset();
        m_scene = nullptr;
        m_ownScene.reset();
        m_sanctuaryScene.reset();
        m_sanctuaryLoaded = false;
        m_origin.reset();
        m_sceneLoaded = false;
        m_initialized = false;
    }

    void TFWorldSetup::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF World"))
            return;

        static const char* roleNames[] = {"Standalone", "ListenHost", "DedicatedServer", "Client"};
        ImGui::Text("Role: %s", roleNames[static_cast<int>(m_ctx->role)]);
        ImGui::Text("Scene: %s (%s)", m_scenePath.c_str(), m_sceneLoaded ? "loaded" : "not loaded");
        ImGui::Text("Height @ center (2048,2048): %.1f m", TerrainHeightAt(2048.0f, 2048.0f));
        ImGui::Text("World collision: %s (%zu static bodies)",
                    (m_collision && m_collision->IsActive()) ? "Jolt live" : "inactive",
                    m_collision ? m_collision->BodyCount() : size_t{0});
        ImGui::Text("Sanctuary: scene %s | collision %s (%zu bodies) | pad height %.1f m",
                    m_sanctuaryLoaded ? "loaded" : "not loaded",
                    (m_sanctuaryCollision && m_sanctuaryCollision->IsActive()) ? "Jolt live" : "inactive",
                    m_sanctuaryCollision ? m_sanctuaryCollision->BodyCount() : size_t{0},
                    TerrainHeightAt(kTFSanctuaryCenterX, kTFSanctuaryCenterZ));

#ifdef ENABLE_NETWORKING
        ImGui::Text("Net booted: %s | WorldServer: %s | AreaServer: %s", m_netBooted ? "yes" : "no",
                    (m_worldServer && m_worldServer->IsRunning()) ? "up" : "down",
                    (m_areaServer && m_areaServer->IsRunning()) ? "up" : "down");
        ImGui::Text("Sessions bridged: %zu", m_knownClients.size());
#endif

        if (m_origin)
        {
            const auto& stats = m_origin->GetStats();
            ImGui::Text("Origin rebases: %u (max dist %.0f m)", stats.totalRebases, stats.maxDistanceFromOrigin);
        }
#endif
    }

} // namespace Terrafront
