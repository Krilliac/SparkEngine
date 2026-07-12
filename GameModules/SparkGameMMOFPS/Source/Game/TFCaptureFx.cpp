/**
 * @file TFCaptureFx.cpp
 * @brief Capture-point presentation FX (see TFCaptureFx.h for the contract).
 *        All three effects submit through TFTransparentPass::Get() (W8
 *        recipe: unit two-sided quad, alpha-blended, depth read-only) using
 *        the shared Assets/Textures/MMOFPS/fx/shield_plane.png soft-glow
 *        texture — no new texture assets, matching the "reuse the emissive +
 *        TFGroundFx/billboard patterns" brief.
 */
#include "Game/TFCaptureFx.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFTransparentPass.h"
#include "World/TFRegionSystem.h" // ctx.regions public accessors (CaptureProgress/IsCapturable)
#include "World/TFWorldSetup.h"   // TerrainHeightAt for ring placement

#include "Audio/AudioEngine.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    namespace
    {

        constexpr const char* kGlowTexture =
            "Assets/Textures/MMOFPS/fx/shield_plane.png"; // soft radial glow (W8 shield-wall precedent)

        // ---- tower progress beam ----
        constexpr float kBeamMaxHeightM = 9.0f; // just under the 11.5 m banner height
        constexpr float kBeamMinContestedFrac = 0.18f; // contested always shows a visible sliver
        constexpr float kBeamWidthM = 0.85f;
        constexpr float kBeamAlphaBase = 0.30f;
        constexpr float kBeamAlphaProgress = 0.35f;
        constexpr float kBeamEmissive = 0.85f;
        constexpr float kContestedPulseHz = 2.2f; // red alarm pulse rate (shared with the standing ring)

        // ---- owner-flip burst ----
        constexpr int kBurstCount = 22;
        constexpr float kBurstSpeedMinMps = 2.0f, kBurstSpeedMaxMps = 5.0f;
        constexpr float kBurstUpMinMps = 1.5f, kBurstUpMaxMps = 4.0f;
        constexpr float kBurstGravityMps2 = 5.0f;
        constexpr float kBurstLifeMinSec = 0.40f, kBurstLifeMaxSec = 0.75f;
        constexpr float kBurstSizeM = 0.30f;
        constexpr float kBurstAlpha = 0.85f;
        constexpr float kBurstEmissive = 1.0f;
        constexpr float kFlareLifeSec = 1.2f;
        constexpr float kFlareRiseM = 6.0f;
        constexpr float kFlareWidthM = 1.1f;
        constexpr float kFlareAlpha = 0.80f;
        constexpr float kFlareEmissive = 1.0f;
        constexpr const char* kFlipStingPath = "Audio/MMOFPS/ui/capture_alarm.wav";
        constexpr float kFlipStingVol = 0.70f;

        // ---- standing ring ----
        constexpr float kRingRadiusMinM = 0.9f, kRingRadiusMaxM = 1.7f;
        constexpr float kRingLiftM = 0.04f; // avoid z-fighting with terrain
        constexpr float kRingAlphaMin = 0.12f, kRingAlphaMax = 0.42f;
        constexpr float kRingEmissive = 0.60f;
        // Mirrors kCaptureRadiusM in TFRegionSystem.cpp (DESIGN §4; private there) and
        // TFAudioAmbience's kAlarmCaptureRadiusM — client presentation only, so a drift
        // would desync nothing but this ring's edge.
        constexpr float kRingCaptureRadiusM = 10.0f;

        constexpr float kTwoPi = 6.2831853f;

        bool IsPlayableFactionLocal(FactionId f)
        {
            return f == FactionId::MRA || f == FactionId::AUC || f == FactionId::HLX;
        }

        /// Faction-alarm red used for every contested pulse (beam + ring share it).
        float ContestedPulseAlphaMul(float pulseClock)
        {
            const float pulse = 0.5f + 0.5f * std::sin(pulseClock * kContestedPulseHz * kTwoPi);
            return 0.55f + 0.45f * pulse;
        }

        void ContestedColor(float pulseClock, float out[3])
        {
            const float pulse = 0.5f + 0.5f * std::sin(pulseClock * kContestedPulseHz * kTwoPi);
            out[0] = 0.85f;
            out[1] = 0.08f + 0.10f * pulse;
            out[2] = 0.08f;
        }

        /// Lazily resolves the GraphicsEngine/unit-plane/glow-SRV trio shared by
        /// every submission below. False (nothing resolved) when headless or the
        /// device isn't up yet — callers just skip drawing this frame.
        bool ResolveFx(TFGameContext& ctx, GraphicsEngine*& outGfx, Mesh*& outPlane, ID3D11ShaderResourceView*& outSrv)
        {
            outGfx = ctx.engine ? ctx.engine->GetGraphics() : nullptr;
            if (!outGfx || !outGfx->GetDevice() || !outGfx->GetContext())
                return false;
            outPlane = TFTransparentPass::Get().GetUnitPlane(*outGfx);
            if (!outPlane)
                return false;
            outSrv = outGfx->GetOrLoadTextureSRV(kGlowTexture);
            return true;
        }

    } // namespace

    TFCaptureFx& TFCaptureFx::Get()
    {
        static TFCaptureFx s_instance;
        return s_instance;
    }

    float TFCaptureFx::Rand01()
    {
        return static_cast<float>(m_rng() & 0xFFFFu) / 65535.0f;
    }

    // ---------------------------------------------------------------------------

    void TFCaptureFx::Update(TFGameContext& ctx, float dt)
    {
        using namespace DirectX;

        m_pulseClock += dt;
        if (m_particles.empty() && m_flares.empty())
            return;

        GraphicsEngine* gfx = nullptr;
        Mesh* plane = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        const bool canDraw = ResolveFx(ctx, gfx, plane, srv);

        for (BurstParticle& p : m_particles)
        {
            p.age += dt;
            if (p.age >= p.life)
                continue; // culled below; skip drawing a dead particle this frame
            p.vel[1] -= kBurstGravityMps2 * dt;
            p.pos[0] += p.vel[0] * dt;
            p.pos[1] += p.vel[1] * dt;
            p.pos[2] += p.vel[2] * dt;
            if (!canDraw)
                continue;

            const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
            const float size = p.size0 + (p.size1 - p.size0) * t;
            const float alpha = kBurstAlpha * (1.0f - t) * (1.0f - t); // ease-out fade

            const XMMATRIX world =
                XMMatrixScaling(size, size, 1.0f) * XMMatrixTranslation(p.pos[0], p.pos[1], p.pos[2]);
            TFTransparentPass::Get().Submit(plane, world, srv,
                                            XMFLOAT4(p.color[0], p.color[1], p.color[2], alpha), kBurstEmissive);
        }
        std::erase_if(m_particles, [](const BurstParticle& p) { return p.age >= p.life; });

        for (Flare& f : m_flares)
        {
            f.age += dt;
            if (f.age >= f.life)
                continue;
            if (!canDraw)
                continue;

            const float t = std::clamp(f.age / f.life, 0.0f, 1.0f);
            const float y = f.pos[1] + kFlareRiseM * t;
            const float alpha = kFlareAlpha * (1.0f - t * t); // ease-out fade
            const float w = kFlareWidthM * (1.0f - 0.4f * t); // thins slightly as it climbs

            const XMMATRIX trans = XMMatrixTranslation(f.pos[0], y, f.pos[2]);
            const XMMATRIX worldA = XMMatrixScaling(w, w * 1.6f, 1.0f) * trans;
            const XMMATRIX worldB = XMMatrixScaling(w, w * 1.6f, 1.0f) * XMMatrixRotationY(XM_PIDIV2) * trans;
            const XMFLOAT4 tint{f.color[0], f.color[1], f.color[2], alpha};
            TFTransparentPass::Get().Submit(plane, worldA, srv, tint, kFlareEmissive);
            TFTransparentPass::Get().Submit(plane, worldB, srv, tint, kFlareEmissive);
        }
        std::erase_if(m_flares, [](const Flare& f) { return f.age >= f.life; });
    }

    // ---------------------------------------------------------------------------

    void TFCaptureFx::SubmitTowerBeam(TFGameContext& ctx, const float towerBasePos[3], float progress,
                                      FactionId capturing, bool contested)
    {
        using namespace DirectX;

        if (!contested && progress <= 0.0f)
            return; // idle, unclaimed point — the tower/banner already reads as neutral

        GraphicsEngine* gfx = nullptr;
        Mesh* plane = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        if (!ResolveFx(ctx, gfx, plane, srv))
            return;

        float color[4];
        float alphaMul = 1.0f;
        if (contested)
        {
            ContestedColor(m_pulseClock, color);
            alphaMul = ContestedPulseAlphaMul(m_pulseClock);
        }
        else
        {
            // capturing is guaranteed set whenever progress > 0 (design contract);
            // FactionColor(None, ...) still returns a safe neutral gray otherwise.
            FactionColor(capturing, color);
        }

        const float heightFrac = contested ? std::max(progress, kBeamMinContestedFrac) : progress;
        const float h = kBeamMaxHeightM * std::clamp(heightFrac, 0.0f, 1.0f);
        const float alpha = (kBeamAlphaBase + kBeamAlphaProgress * std::clamp(progress, 0.0f, 1.0f)) * alphaMul;
        const XMFLOAT4 tint{color[0], color[1], color[2], alpha};

        const XMMATRIX trans =
            XMMatrixTranslation(towerBasePos[0], towerBasePos[1] + h * 0.5f, towerBasePos[2]);
        const XMMATRIX worldA = XMMatrixScaling(kBeamWidthM, h, 1.0f) * trans;
        const XMMATRIX worldB = XMMatrixScaling(kBeamWidthM, h, 1.0f) * XMMatrixRotationY(XM_PIDIV2) * trans;
        TFTransparentPass::Get().Submit(plane, worldA, srv, tint, kBeamEmissive);
        TFTransparentPass::Get().Submit(plane, worldB, srv, tint, kBeamEmissive);
    }

    // ---------------------------------------------------------------------------

    void TFCaptureFx::SpawnOwnerFlipBurst(TFGameContext& ctx, const float towerBasePos[3], FactionId newOwner)
    {
        if (!ctx.HasLocalPlayer())
            return; // pure dedicated server never builds client presentation

        float color[4];
        FactionColor(newOwner, color);

        const int room = static_cast<int>(kMaxParticles) - static_cast<int>(m_particles.size());
        const int n = std::clamp(room, 0, kBurstCount);
        for (int i = 0; i < n; ++i)
        {
            BurstParticle p;
            const float ang = Rand01() * kTwoPi;
            const float speed = kBurstSpeedMinMps + (kBurstSpeedMaxMps - kBurstSpeedMinMps) * Rand01();
            p.pos[0] = towerBasePos[0];
            p.pos[1] = towerBasePos[1] + 0.6f;
            p.pos[2] = towerBasePos[2];
            p.vel[0] = std::cos(ang) * speed;
            p.vel[2] = std::sin(ang) * speed;
            p.vel[1] = kBurstUpMinMps + (kBurstUpMaxMps - kBurstUpMinMps) * Rand01();
            p.life = kBurstLifeMinSec + (kBurstLifeMaxSec - kBurstLifeMinSec) * Rand01();
            p.size0 = kBurstSizeM * (0.7f + 0.6f * Rand01());
            p.size1 = p.size0 * 0.25f;
            p.color[0] = color[0];
            p.color[1] = color[1];
            p.color[2] = color[2];
            m_particles.push_back(p);
        }

        if (m_flares.size() < kMaxFlares)
        {
            Flare f;
            f.pos[0] = towerBasePos[0];
            f.pos[1] = towerBasePos[1];
            f.pos[2] = towerBasePos[2];
            f.life = kFlareLifeSec;
            f.color[0] = color[0];
            f.color[1] = color[1];
            f.color[2] = color[2];
            m_flares.push_back(f);
        }

        ::AudioEngine* audio = ctx.engine ? ctx.engine->GetAudio() : nullptr;
        if (!audio)
            return;

        // Lazy-load once (TFUiSounds_Play precedent); module-image-local static is
        // safe here because this header is only included by this module's TUs
        // (per-image DLL statics gotcha respected).
        static bool s_stingLoaded = false;
        if (!s_stingLoaded)
        {
            s_stingLoaded = true;
            const std::string full = std::string("Assets/") + kFlipStingPath;
            if (FAILED(audio->LoadSound(kFlipStingPath, std::wstring(full.begin(), full.end()))))
                SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] capture-fx: flip sting load FAIL %s", kFlipStingPath);
        }
        audio->PlaySound3D(kFlipStingPath,
                           DirectX::XMFLOAT3(towerBasePos[0], towerBasePos[1] + 1.5f, towerBasePos[2]),
                           kFlipStingVol, 1.0f, /*loop*/ false);
    }

    // ---------------------------------------------------------------------------

    void TFCaptureFx::UpdateStandingRing(TFGameContext& ctx)
    {
        using namespace DirectX;

        if (!ctx.InWorld() || !ctx.players || !ctx.regions || !ctx.world || !ctx.data || !ctx.data->IsLoaded())
            return;
        if (ctx.localPlayer == kInvalidPlayer)
            return;

        PawnInfo pawn{};
        if (!ctx.players->GetPawnByPlayer(ctx.localPlayer, pawn) || !pawn.alive)
            return;

        // Same eligibility fallback as TFRegionSystem::FeedLocalCaptureHUD: the
        // replicated pawn faction wins once it mirrors in, ctx.localFaction covers
        // the frames before it does.
        const FactionId f = IsPlayableFactionLocal(pawn.faction) ? pawn.faction : ctx.localFaction;
        if (!IsPlayableFactionLocal(f))
            return;

        constexpr float r2 = kRingCaptureRadiusM * kRingCaptureRadiusM;
        for (const RegionDef& def : ctx.data->GetContinent().regions)
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
            const float progress = ctx.regions->CaptureProgress(def.id, capturing, contested);
            // Same "can this point react to me" predicate as the HUD feed: live
            // (progress moving / contested) or lattice-capturable by my faction.
            // Standing on an owned/unlinked/idle point shows nothing, matching the
            // 2026-07-10 "forever-frozen bar" fix this predicate already carries.
            const bool live = progress > 0.0f || capturing != FactionId::None || contested;
            if (!live && !ctx.regions->IsCapturable(def.id, f))
                continue;

            GraphicsEngine* gfx = nullptr;
            Mesh* plane = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            if (!ResolveFx(ctx, gfx, plane, srv))
                return;

            float color[4];
            float alphaMul = 1.0f;
            if (contested)
            {
                ContestedColor(m_pulseClock, color);
                alphaMul = ContestedPulseAlphaMul(m_pulseClock);
            }
            else if (capturing != FactionId::None)
            {
                FactionColor(capturing, color);
            }
            else
            {
                // Idle-but-capturable point: tint toward the player's own faction as
                // a quiet "you can start this" invite rather than showing nothing.
                FactionColor(f, color);
            }

            const float y = ctx.world->TerrainHeightAt(pawn.pos[0], pawn.pos[2]) + kRingLiftM;
            const float progClamp = std::clamp(progress, 0.0f, 1.0f);
            const float radius = kRingRadiusMinM + (kRingRadiusMaxM - kRingRadiusMinM) * progClamp;
            const float alpha = (kRingAlphaMin + (kRingAlphaMax - kRingAlphaMin) * progClamp) * alphaMul;

            const XMMATRIX world = XMMatrixScaling(radius * 2.0f, radius * 2.0f, 1.0f) * XMMatrixRotationX(XM_PIDIV2) *
                                   XMMatrixTranslation(pawn.pos[0], y, pawn.pos[2]);
            TFTransparentPass::Get().Submit(plane, world, srv, XMFLOAT4(color[0], color[1], color[2], alpha),
                                            kRingEmissive);
            return; // one live point at a time, mirrors FeedLocalCaptureHUD
        }
    }

} // namespace Terrafront
