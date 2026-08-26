/**
 * @file TFAudioAmbience.cpp
 * @brief Per-zone ambient beds (sanctuary hum / continent wind, crossfaded)
 *        plus activity-biased distant-combat and wind-gust one-shots. The W9
 *        remote-fire wire half (ClientOnRemoteFire + the 0x54F4/0x54F5 handler
 *        lifecycle) lives in TFAudioAmbienceWire.cpp.
 */
#include "Game/TFAudioAmbience.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "World/TFRegionSystem.h" // W10 audio-wave-2: capture-alarm predicate (public accessors)
#include "World/TFSanctuaryZone.h"
#include "World/TFWeatherFx.h" // W12 weather-visuals: storm intensity for the wind bed + gust bias
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

        // W12 weather-visuals: dust-storm wind bed (wind_loop_02 as a loop) +
        // gust one-shot bias. The bed target is kStormBedVol * storm intensity,
        // so it crossfades up through Building and back down through Clearing.
        constexpr const char* kStormBedPath = "Audio/MMOFPS/ambient/wind_loop_02.wav";
        constexpr float kStormBedVol = 0.55f;
        constexpr float kStormGustVolBoost = 1.6f;       // gust volume multiplier at full storm
        constexpr double kStormGustIntervalScale = 0.45; // gust interval multiplier at full storm

        // W10 audio-wave-2: capture-alarm loop.
        constexpr const char* kCaptureAlarm = "Audio/MMOFPS/ui/capture_alarm.wav";
        constexpr float kAlarmVol = 0.45f;     // target loop volume while on an active point
        constexpr float kAlarmFadeSec = 0.35f; // full-swing fade (much snappier than the beds)
        // Stand-on radius around a capture point — mirrors kCaptureRadiusM in
        // TFRegionSystem.cpp (DESIGN §4; private there). Client presentation
        // only, so a drift would desync nothing but this alarm's edge.
        constexpr float kAlarmCaptureRadiusM = 10.0f;

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

        // W10 audio-wave-2: capture-alarm loop (local pawn on an active point).
        m_alarmOn = LocalOnActiveCapturePoint();
        UpdateCaptureAlarm(*audio, deltaTime);
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

        // W12 weather-visuals: storm wind loop rides the SAME fade helper —
        // held-source revalidation and volume mirroring come for free. Silent
        // in the sanctuary (storms are a continent phenomenon).
        m_stormBed.path = kStormBedPath;
        const float storm = TFWeatherFx::Get().StormIntensity01();
        fade(m_stormBed, inSanctuary ? 0.0f : kStormBedVol * storm);
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
        stop(m_alarmBed); // W10 audio-wave-2
        stop(m_stormBed); // W12 weather-visuals
        m_alarmOn = false;
    }

    // ---------------------------------------------------------------------------
    // W10 audio-wave-2: capture alarm (local pawn on an actively-capturing or
    // contested point)
    // ---------------------------------------------------------------------------

    bool TFAudioAmbience::LocalOnActiveCapturePoint() const
    {
        if (!m_ctx->InWorld() || !m_ctx->players || !m_ctx->regions || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        if (m_ctx->localPlayer == kInvalidPlayer)
            return false;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) || !pawn.alive)
            return false;

        // Same "live point" notion as the HUD capture feed (TFRegionSystem::
        // FeedLocalCaptureHUD): the alarm sounds only where progress is moving
        // or the point is contested — standing on an idle point stays silent.
        constexpr float r2 = kAlarmCaptureRadiusM * kAlarmCaptureRadiusM;
        for (const RegionDef& def : m_ctx->data->GetContinent().regions)
        {
            if (def.tier == "skyanchor" || def.capturePoints.empty())
                continue;
            bool onPoint = false;
            for (const auto& pt : def.capturePoints)
            {
                const float dx = pawn.pos[0] - pt[0];
                const float dz = pawn.pos[2] - pt[1];
                if (dx * dx + dz * dz <= r2)
                {
                    onPoint = true;
                    break;
                }
            }
            if (!onPoint)
                continue;
            FactionId capturing = FactionId::None;
            bool contested = false;
            const float progress = m_ctx->regions->CaptureProgress(def.id, capturing, contested);
            if (contested || capturing != FactionId::None || progress > 0.0f)
                return true;
        }
        return false;
    }

    void TFAudioAmbience::UpdateCaptureAlarm(::AudioEngine& audio, float dt)
    {
        m_alarmBed.path = kCaptureAlarm;
        const float target = m_alarmOn ? kAlarmVol : 0.0f;

        // Same held-source discipline as the beds (see file header), with a
        // much faster fade so the alarm reads as an on/off state change.
        Bed& bed = m_alarmBed;
        const float step = dt / kAlarmFadeSec;
        bed.vol += std::clamp(target - bed.vol, -step, step);
        bed.vol = std::clamp(bed.vol, 0.0f, 1.0f);

        if (bed.src && (!bed.src->IsPlaying || !bed.src->IsLooping || bed.src->Sound != audio.GetSound(bed.path)))
            bed.src = nullptr;

        if (!bed.src)
        {
            if (bed.vol <= 0.001f)
                return; // silent and not playing — keep the pool slot free
            EnsureLoaded(audio, bed.path);
            bed.src = audio.PlaySound(bed.path, bed.vol, 1.0f, /*loop*/ true);
            if (!bed.src)
                return; // pool exhausted — retry next frame
        }

        const float finalVol = bed.vol * audio.GetSFXVolume() * audio.GetMasterVolume();
        bed.src->Volume = finalVol;
        if (bed.src->Voice)
            bed.src->Voice->SetVolume(finalVol);
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
            // W12 weather-visuals: storms make gusts louder and more frequent
            // (intensity 0 leaves both factors at exactly 1 — legacy behavior).
            const float storm = TFWeatherFx::Get().StormIntensity01();
            std::uniform_int_distribution<size_t> pick(0, std::size(kWindGusts) - 1);
            std::uniform_real_distribution<float> gustVol(0.12f, 0.22f);
            const char* gust = kWindGusts[pick(m_rng)];
            EnsureLoaded(audio, gust);
            audio.PlaySound(gust, std::min(0.6f, gustVol(m_rng) * (1.0f + (kStormGustVolBoost - 1.0f) * storm)));
            std::uniform_real_distribution<double> next(kGustIntervalMinSec, kGustIntervalMaxSec);
            m_nextGust = m_clock + next(m_rng) * (1.0 + (kStormGustIntervalScale - 1.0) * static_cast<double>(storm));
        }
    }

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
            const auto p = cam->GetPosition();
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
        ImGui::Text("storm bed : %.2f (%s, weather %s)", m_stormBed.vol, m_stormBed.src ? "live" : "-",
                    TFWeatherFx::PhaseName(TFWeatherFx::Get().CurrentPhase())); // W12 weather-visuals
        ImGui::Text("activity  : %.2f", m_activity);
        ImGui::Text("cap alarm : %s (vol %.2f, %s)", m_alarmOn ? "ON" : "off", m_alarmBed.vol,
                    m_alarmBed.src ? "live" : "-"); // W10 audio-wave-2
        ImGui::Text("next shot : combat %+.1fs  gust %+.1fs", m_nextCombatShot - m_clock, m_nextGust - m_clock);
#endif
    }

} // namespace Terrafront
