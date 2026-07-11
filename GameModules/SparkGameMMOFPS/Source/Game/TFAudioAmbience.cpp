/**
 * @file TFAudioAmbience.cpp
 * @brief Per-zone ambient beds (sanctuary hum / continent wind, crossfaded)
 *        plus activity-biased distant-combat and wind-gust one-shots.
 */
#include "Game/TFAudioAmbience.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h" // W9: kEyeHeightM (muzzle -> pawn-pos rebase)
#include "Game/TFWeaponSystem.h"
#include "Net/TFFireFxProtocol.h" // W9 remote-fire-events: 0x54F4 wire struct
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h" // W9: SpawnMuzzleFx (remote flash quad)
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include "Audio/AudioEngine.h"
#include "Camera/SparkEngineCamera.h"
#include "Spark/IEngineContext.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace Terrafront
{

    namespace
    {
        // Fixed one-shot assets (the wind BED path comes from presentation.json).
        constexpr const char* kSanctuaryHum = "Audio/MMOFPS/ambient/sanctuary_hum.wav";
        constexpr const char* kCombatDistant = "Audio/MMOFPS/ambient/combat_distant.wav";
        constexpr const char* kWindGusts[] = {
            "Audio/MMOFPS/ambient/wind_loop_02.wav",
            "Audio/MMOFPS/ambient/wind_loop_03.wav",
        };

        constexpr float kSanctuaryBedVol = 0.40f; // target vol inside the sanctuary
        constexpr float kCrossfadeSec = 1.5f;     // full-swing bed crossfade time
        constexpr float kActivityHalfLifeSec = 10.0f;

        // combat_distant scheduling: interval/volume lerped by activity 0..1.
        constexpr double kCombatIntervalCalmSec = 45.0;
        constexpr double kCombatIntervalHotSec = 9.0;
        constexpr float kCombatVolCalm = 0.10f;
        constexpr float kCombatVolHot = 0.40f;

        constexpr double kGustIntervalMinSec = 18.0;
        constexpr double kGustIntervalMaxSec = 40.0;

        float Lerp(float a, float b, float t)
        {
            return a + (b - a) * t;
        }
    } // namespace

    TFAudioAmbience::TFAudioAmbience() = default;
    TFAudioAmbience::~TFAudioAmbience()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFAudioAmbience::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Combat-activity heat: bus events are the client-visible signal on the
        // authority roles (Standalone/ListenHost — the killfeed source). Remote
        // fire heard by the distant-gunfire layer adds via RemoteFireHeat().
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled&)
                                         { m_activity = std::min(1.0f, m_activity + 0.25f); });
        events.Subscribe<EvRegionContested>(
            [this](const EvRegionContested& ev)
            {
                if (ev.contested)
                    m_activity = std::min(1.0f, m_activity + 0.20f);
            });
        events.Subscribe<EvRegionCaptured>([this](const EvRegionCaptured&)
                                           { m_activity = std::min(1.0f, m_activity + 0.30f); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFAudioAmbience initialized");
        return true;
    }

    void TFAudioAmbience::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        m_clock += deltaTime;

#ifdef ENABLE_NETWORKING
        // W9 remote-fire-events: 0x54F4 handler lifecycle, polled like
        // TFSocialSystem's mirror handlers (pure clients only — the server
        // never sends the message to itself or the listen host's player).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_netHandlers)
            EnsureNetHandlers();
        else if (!clientUp && m_netHandlers)
            ReleaseNetHandlers();
#endif

        // Exponential decay toward calm (half-life ~10 s).
        m_activity *= std::exp(-deltaTime * 0.6931472f / kActivityHalfLifeSec);
        if (m_ctx->weapons)
            m_activity = std::max(m_activity, std::min(1.0f, m_ctx->weapons->RemoteFireHeat()));

        ::AudioEngine* audio = Audio();
        if (!audio)
            return;

        float x = 0.0f, z = 0.0f;
        if (!ListenerXZ(x, z))
            return; // no camera and no pawn yet (login flow) — stay silent
        const bool inSanctuary = TFTravel_IsInSanctuary(x, z);
        m_lastInSanctuary = inSanctuary;

        UpdateBeds(*audio, inSanctuary, deltaTime);
        UpdateOneShots(*audio, inSanctuary);
    }

    void TFAudioAmbience::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // ambience is presentation-only; nothing authoritative
    }

    void TFAudioAmbience::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (m_netHandlers)
            ReleaseNetHandlers();
#endif
        StopBeds();
        m_loaded.clear();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Beds
    // ---------------------------------------------------------------------------

    void TFAudioAmbience::UpdateBeds(::AudioEngine& audio, bool inSanctuary, float dt)
    {
        // Resolve bed paths lazily: wind bed follows presentation.json (path +
        // volume) so data-table tuning keeps working; defaults match the old
        // TFWorldSetup wind loop exactly.
        AmbientAudioDef windDef; // in-class defaults == legacy behavior
        if (m_ctx->data && m_ctx->data->IsLoaded())
            windDef = m_ctx->data->GetPresentation().ambient;

        // Path swap (tf_reload_data changed presentation.json): stop the old
        // loop BEFORE dropping the pointer, or it plays at its last volume
        // forever with nobody holding it.
        if (m_windBed.path != windDef.path)
        {
            if (m_windBed.src && m_windBed.src->IsPlaying && m_windBed.src->IsLooping &&
                m_windBed.src->Sound == audio.GetSound(m_windBed.path))
                audio.StopSound(m_windBed.src);
            m_windBed.src = nullptr;
            m_windBed.path = windDef.path;
        }
        m_sanctuaryBed.path = kSanctuaryHum;

        const float windTarget = inSanctuary ? 0.0f : windDef.volume;
        const float humTarget = inSanctuary ? kSanctuaryBedVol : 0.0f;

        const float step = dt / kCrossfadeSec; // full swing over kCrossfadeSec
        const auto fade = [&](Bed& bed, float target)
        {
            bed.vol += std::clamp(target - bed.vol, -step, step);
            bed.vol = std::clamp(bed.vol, 0.0f, 1.0f);

            // Revalidate the held pool slot (see header): a stolen/stopped slot
            // must never be written to — it may be someone else's sound now.
            if (bed.src && (!bed.src->IsPlaying || !bed.src->IsLooping || bed.src->Sound != audio.GetSound(bed.path)))
                bed.src = nullptr;

            if (!bed.src)
            {
                if (bed.vol <= 0.001f)
                    return; // fully faded out and not playing — leave the slot free
                EnsureLoaded(audio, bed.path);
                bed.src = audio.PlaySound(bed.path, bed.vol, 1.0f, /*loop*/ true);
                if (!bed.src)
                    return; // pool exhausted — retry next frame
            }

            // Mirror PlaySound's scaling (volume * sfx * master) every frame so
            // console volume changes keep applying to the held loop.
            const float finalVol = bed.vol * audio.GetSFXVolume() * audio.GetMasterVolume();
            bed.src->Volume = finalVol;
            if (bed.src->Voice)
                bed.src->Voice->SetVolume(finalVol);
        };

        fade(m_windBed, windTarget);
        fade(m_sanctuaryBed, humTarget);
    }

    void TFAudioAmbience::StopBeds()
    {
        ::AudioEngine* audio = Audio();
        const auto stop = [&](Bed& bed)
        {
            if (audio && bed.src && bed.src->IsPlaying && bed.src->IsLooping &&
                bed.src->Sound == audio->GetSound(bed.path))
                audio->StopSound(bed.src);
            bed.src = nullptr;
            bed.vol = 0.0f;
        };
        stop(m_windBed);
        stop(m_sanctuaryBed);
    }

    // ---------------------------------------------------------------------------
    // One-shot layers (continent only)
    // ---------------------------------------------------------------------------

    void TFAudioAmbience::UpdateOneShots(::AudioEngine& audio, bool inSanctuary)
    {
        // No war-noise layer in the sanctuary or on the login/char-select screen
        // (the beds DO run pre-world — parity with the old TFWorldSetup wind).
        if (inSanctuary || !m_ctx->InWorld())
        {
            // Re-arm so leaving doesn't fire a stale backlog immediately.
            m_nextCombatShot = std::max(m_nextCombatShot, m_clock + 4.0);
            m_nextGust = std::max(m_nextGust, m_clock + 4.0);
            return;
        }

        if (m_clock >= m_nextCombatShot)
        {
            const float vol = Lerp(kCombatVolCalm, kCombatVolHot, m_activity);
            EnsureLoaded(audio, kCombatDistant);
            audio.PlaySound(kCombatDistant, vol);
            const double interval =
                kCombatIntervalCalmSec + (kCombatIntervalHotSec - kCombatIntervalCalmSec) * m_activity;
            std::uniform_real_distribution<double> jitter(0.75, 1.25);
            m_nextCombatShot = m_clock + interval * jitter(m_rng);
        }

        if (m_clock >= m_nextGust)
        {
            std::uniform_int_distribution<size_t> pick(0, std::size(kWindGusts) - 1);
            std::uniform_real_distribution<float> gustVol(0.12f, 0.22f);
            const char* gust = kWindGusts[pick(m_rng)];
            EnsureLoaded(audio, gust);
            audio.PlaySound(gust, gustVol(m_rng));
            std::uniform_real_distribution<double> next(kGustIntervalMinSec, kGustIntervalMaxSec);
            m_nextGust = m_clock + next(m_rng);
        }
    }

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
            m_ctx->world->SpawnMuzzleFx(muzzlePos, dirUnit);

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

        m_netHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] remote-fire fx handler registered");
    }

    void TFAudioAmbience::ReleaseNetHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFSocialSystem pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFFxMsg_RemoteFire),
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

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    ::AudioEngine* TFAudioAmbience::Audio() const
    {
        return (m_ctx && m_ctx->engine) ? m_ctx->engine->GetAudio() : nullptr;
    }

    bool TFAudioAmbience::ListenerXZ(float& outX, float& outZ) const
    {
        // Camera first — matches the per-zone skybox rule in TFWorldSetup, so
        // what you SEE (sanctuary sky) and what you HEAR always agree.
        if (const SparkEngineCamera* cam = m_ctx->engine ? m_ctx->engine->GetCamera() : nullptr)
        {
            const auto& p = cam->GetPosition();
            outX = p.x;
            outZ = p.z;
            return true;
        }
        PawnInfo pawn;
        if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
        {
            outX = pawn.pos[0];
            outZ = pawn.pos[2];
            return true;
        }
        return false;
    }

    void TFAudioAmbience::EnsureLoaded(::AudioEngine& audio, const std::string& assetPath)
    {
        if (!m_loaded.insert(assetPath).second)
            return;
        const std::string full = "Assets/" + assetPath; // module paths are Assets-relative
        if (FAILED(audio.LoadSound(assetPath, std::wstring(full.begin(), full.end()))))
            Spark::SimpleConsole::GetInstance().LogWarning("[TFAudio] ambience load FAIL " + assetPath);
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFAudioAmbience::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Ambience"))
            return;
        ImGui::Text("zone      : %s", m_lastInSanctuary ? "sanctuary" : "continent");
        ImGui::Text("wind bed  : %.2f (%s)", m_windBed.vol, m_windBed.src ? "live" : "-");
        ImGui::Text("hum bed   : %.2f (%s)", m_sanctuaryBed.vol, m_sanctuaryBed.src ? "live" : "-");
        ImGui::Text("activity  : %.2f", m_activity);
        ImGui::Text("next shot : combat %+.1fs  gust %+.1fs", m_nextCombatShot - m_clock, m_nextGust - m_clock);
#endif
    }

} // namespace Terrafront
