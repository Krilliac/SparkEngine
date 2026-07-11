/**
 * @file TFFootsteps.cpp
 * @brief Client-side footstep audio — see TFFootsteps.h for the trigger /
 *        grounded / surface model and its documented limitations.
 */
#include "Game/TFFootsteps.h"

#include "Game/TFMovementModel.h" // kTFEyeHeightM (listener fallback height)
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h" // IsSeated — riding pawns never step
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include "Audio/AudioEngine.h"
#include "Camera/SparkEngineCamera.h"
#include "Spark/IEngineContext.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <iterator>

namespace Terrafront
{

    namespace
    {
        constexpr const char* kSandSteps[] = {
            "Audio/MMOFPS/foley/footstep_sand_01.wav",
            "Audio/MMOFPS/foley/footstep_sand_02.wav",
            "Audio/MMOFPS/foley/footstep_sand_03.wav",
            "Audio/MMOFPS/foley/footstep_sand_04.wav",
        };
        constexpr const char* kMetalSteps[] = {
            "Audio/MMOFPS/foley/footstep_metal_01.wav",
            "Audio/MMOFPS/foley/footstep_metal_02.wav",
            "Audio/MMOFPS/foley/footstep_metal_03.wav",
            "Audio/MMOFPS/foley/footstep_metal_04.wav",
        };

        constexpr float kHearRangeM = 25.0f;    // remote pawns beyond this are silent
        constexpr float kStrideWalkM = 2.2f;    // meters between steps, walking
        constexpr float kStrideSprintM = 2.8f;  // meters between steps, sprinting
        constexpr float kSprintSpeedMps = 6.0f; // above this horizontal speed use the sprint stride
                                                // (class runSpeed tops out at 5.6, sprint at 7.8)
        constexpr float kTeleportGuardM = 5.0f; // larger per-frame delta == teleport/respawn, not a stride
        constexpr float kGroundGapM = 0.10f;    // feet within this of the terrain == grounded
        constexpr float kRestVelEps = 0.15f;    // |vel.y| below this == vertically at rest (see header)
        constexpr float kBodyTopGapM = 0.35f;   // grounded this far ABOVE terrain == on a static body (metal)
        constexpr float kLocalWalkVol = 0.32f;
        constexpr float kLocalSprintVol = 0.40f;
        constexpr float kRemoteMaxVol = 0.50f; // at 0 m, falls off linearly to 0 at kHearRangeM
        constexpr float kPruneEverySec = 5.0f;
        constexpr double kTrackStaleSec = 3.0;

        float HorizontalDist(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0];
            const float dz = a[2] - b[2];
            return std::sqrt(dx * dx + dz * dz);
        }
    } // namespace

    TFFootsteps::TFFootsteps() = default;
    TFFootsteps::~TFFootsteps()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFFootsteps::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFFootsteps initialized");
        return true;
    }

    void TFFootsteps::Shutdown()
    {
        m_tracks.clear();
        m_loaded.clear();
        m_initialized = false;
    }

    void TFFootsteps::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // presentation-only; nothing authoritative
    }

    void TFFootsteps::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;
        m_clock += deltaTime;
        m_pruneAccum += deltaTime;

        ::AudioEngine* audio = Audio();
        if (!audio || !m_ctx->players)
            return;

        float listener[3];
        if (!ListenerPos(listener))
            return;

        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                // Riding pawns follow the vehicle transform — no footsteps.
                if (m_ctx->vehicles && m_ctx->vehicles->IsSeated(p.owner))
                {
                    m_tracks.erase(p.entity);
                    return;
                }
                const bool isLocal = p.owner == m_ctx->localPlayer;
                float dist = 0.0f;
                if (!isLocal)
                {
                    const float dx = p.pos[0] - listener[0];
                    const float dy = p.pos[1] - listener[1];
                    const float dz = p.pos[2] - listener[2];
                    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (dist > kHearRangeM)
                    {
                        m_tracks.erase(p.entity); // re-seed prevPos on re-entry, no phantom stride
                        return;
                    }
                }
                UpdatePawn(*audio, p, isLocal, dist);
            });

        if (m_pruneAccum >= kPruneEverySec)
        {
            m_pruneAccum = 0.0f;
            PruneStale();
        }
    }

    void TFFootsteps::UpdatePawn(::AudioEngine& audio, const PawnInfo& pawn, bool isLocal, float distToListener)
    {
        Track& t = m_tracks[pawn.entity];
        t.lastSeen = m_clock;

        if (!t.havePrev)
        {
            std::copy(pawn.pos, pawn.pos + 3, t.prevPos);
            t.havePrev = true;
            return;
        }

        const float moved = HorizontalDist(pawn.pos, t.prevPos);
        std::copy(pawn.pos, pawn.pos + 3, t.prevPos);
        if (moved > kTeleportGuardM)
        {
            t.accum = 0.0f; // teleport / travel / respawn — not a stride
            return;
        }

        // Grounded heuristic (see header): near the analytic terrain, or
        // vertically at rest (standing on a static-body top zeroes vel.y on
        // both the server integrator and the client prediction sim).
        const float terrain = m_ctx->world ? m_ctx->world->TerrainHeightAt(pawn.pos[0], pawn.pos[2]) : pawn.pos[1];
        const float gap = pawn.pos[1] - terrain;
        const bool grounded = gap < kGroundGapM || std::fabs(pawn.vel[1]) < kRestVelEps;
        if (!grounded)
            return; // airborne frames neither add distance nor reset the stride

        t.accum += moved;
        const float hSpeed = std::sqrt(pawn.vel[0] * pawn.vel[0] + pawn.vel[2] * pawn.vel[2]);
        const bool sprinting = hSpeed > kSprintSpeedMps;
        const float stride = sprinting ? kStrideSprintM : kStrideWalkM;
        if (t.accum < stride)
            return;
        t.accum -= stride;
        if (t.accum > stride)
            t.accum = 0.0f; // never bank more than one pending step

        PlayStep(audio, pawn, isLocal, sprinting, distToListener);
    }

    void TFFootsteps::PlayStep(::AudioEngine& audio, const PawnInfo& pawn, bool isLocal, bool sprinting,
                               float distToListener)
    {
        // Surface pick (see header): sanctuary floors are metal; a grounded
        // pawn well above the analytic terrain stands on a static body top
        // (collision-v2 landing) == metal; sand everywhere else.
        const float terrain = m_ctx->world ? m_ctx->world->TerrainHeightAt(pawn.pos[0], pawn.pos[2]) : pawn.pos[1];
        const bool onBody = (pawn.pos[1] - terrain) > kBodyTopGapM;
        const bool metal = onBody || TFTravel_IsInSanctuary(pawn.pos[0], pawn.pos[2]);

        const char* path;
        if (metal)
            path = kMetalSteps[m_metalIdx++ % std::size(kMetalSteps)];
        else
            path = kSandSteps[m_sandIdx++ % std::size(kSandSteps)];

        float vol;
        if (isLocal)
            vol = sprinting ? kLocalSprintVol : kLocalWalkVol;
        else
            vol = kRemoteMaxVol * std::clamp(1.0f - distToListener / kHearRangeM, 0.0f, 1.0f);
        if (vol < 0.02f)
            return;

        std::uniform_real_distribution<float> jitter(0.94f, 1.06f);
        EnsureLoaded(audio, path);
        audio.PlaySound(path, vol, jitter(m_rng));

        ++m_stepsPlayed;
        m_lastMetal = metal;
    }

    void TFFootsteps::PruneStale()
    {
        for (auto it = m_tracks.begin(); it != m_tracks.end();)
        {
            if (m_clock - it->second.lastSeen > kTrackStaleSec)
                it = m_tracks.erase(it);
            else
                ++it;
        }
    }

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    ::AudioEngine* TFFootsteps::Audio() const
    {
        return (m_ctx && m_ctx->engine) ? m_ctx->engine->GetAudio() : nullptr;
    }

    bool TFFootsteps::ListenerPos(float out[3]) const
    {
        if (const SparkEngineCamera* cam = m_ctx->engine ? m_ctx->engine->GetCamera() : nullptr)
        {
            const auto& p = cam->GetPosition();
            out[0] = p.x;
            out[1] = p.y;
            out[2] = p.z;
            return true;
        }
        PawnInfo pawn;
        if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
        {
            out[0] = pawn.pos[0];
            out[1] = pawn.pos[1] + kTFEyeHeightM;
            out[2] = pawn.pos[2];
            return true;
        }
        return false;
    }

    void TFFootsteps::EnsureLoaded(::AudioEngine& audio, const std::string& assetPath)
    {
        if (!m_loaded.insert(assetPath).second)
            return;
        const std::string full = "Assets/" + assetPath; // module paths are Assets-relative
        if (FAILED(audio.LoadSound(assetPath, std::wstring(full.begin(), full.end()))))
            Spark::SimpleConsole::GetInstance().LogWarning("[TFAudio] footstep load FAIL " + assetPath);
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFFootsteps::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Footsteps"))
            return;
        ImGui::Text("tracked pawns : %zu", m_tracks.size());
        ImGui::Text("steps played  : %u (last: %s)", m_stepsPlayed, m_lastMetal ? "metal" : "sand");
#endif
    }

} // namespace Terrafront
