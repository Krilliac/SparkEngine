/**
 * @file TFWeatherFxRender.cpp
 * @brief TFWeatherFx client presentation: wind-blown dust-flake billboards
 *        (TFGroundFx flipbook recipe, additive, depth read-only) + the
 *        camera-locked fullscreen dust tint quad. The server-authoritative
 *        cycle, console command and 0x547C sync live in TFWeatherFx.cpp
 *        (same class, split per the repo file-size rules — mirrors the
 *        TFWorldSetup/-Render split).
 */
#include "World/TFWeatherFx.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

namespace Terrafront
{

    namespace
    {

        // ------------------------------------------------------------ visuals
        constexpr float kMaxFrameDtSec = 0.05f; ///< clamp render dt (alt-tab / hitches)

        // Wind-blown dust flakes (TFGroundFx recipe, storm-scaled count).
        constexpr size_t kStormMaxFlakes = 120;  ///< hard cap at full intensity
        constexpr int kMaxSpawnPerFrame = 6;     ///< population ramps in, never bursts
        constexpr float kFlakeMinRadiusM = 6.0f; ///< spawn ring around the camera
        constexpr float kFlakeMaxRadiusM = 28.0f;
        constexpr float kFlakeYBelowM = 3.0f; ///< spawn band relative to camera height
        constexpr float kFlakeYAboveM = 7.0f;
        constexpr float kFlakeKillDistM = 60.0f;               ///< blown past this from the camera -> recycle
        constexpr float kWindBaseMps = 10.0f;                  ///< horizontal wind at intensity 0
        constexpr float kWindStormMps = 8.0f;                  ///< ... plus this at intensity 1
        constexpr float kFlakeAlpha = 0.28f;                   ///< peak additive weight at full storm
        constexpr float kFlakeColor[3] = {1.0f, 0.86f, 0.62f}; ///< sandy, matches hover dust

        // Flipbook (same 4-frame strip as the hover dust — no new assets).
        constexpr int kDustFrames = 4;
        constexpr char kDustSheet[] = "Assets/Textures/MMOFPS/fx/hover_dust.png";

        // Fullscreen dust tint (alpha quad locked to the camera).
        constexpr float kTintDistM = 0.35f;    ///< quad distance in front of the camera
        constexpr float kTintMargin = 1.10f;   ///< overscan so FOV edges never peek
        constexpr float kTintAlphaMax = 0.20f; ///< subtle wash at full storm
        constexpr float kTintColor[3] = {0.76f, 0.58f, 0.36f};

        double NowRealSec()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Render (client presentation)
    // ---------------------------------------------------------------------------

    void TFWeatherFx::Render(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj)
    {
        if (!gfx || !gfx->GetDevice() || !gfx->GetContext() || !ctx.HasLocalPlayer())
            return;

        const double nowReal = NowRealSec();
        float dt = (m_lastRealTime < 0.0) ? 0.0f : static_cast<float>(nowReal - m_lastRealTime);
        m_lastRealTime = nowReal;
        dt = std::clamp(dt, 0.0f, kMaxFrameDtSec);

        if (m_intensity <= 0.003f && m_flakes.empty())
            return; // clear sky and nothing left to fade out

        if (!m_quad)
        {
            m_quad = std::make_unique<Mesh>();
            m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
        }
        if (!m_quad || m_quad->GetIndexCount() == 0)
            return;

        UpdateAndDrawFlakes(ctx, gfx, view, proj, dt);
        if (m_intensity > 0.003f)
            DrawScreenTint(gfx, view, proj);

        // Restore the frame-baseline states for whoever draws next.
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

    void TFWeatherFx::UpdateAndDrawFlakes(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                          const DirectX::XMMATRIX& proj, float dt)
    {
        using namespace DirectX;
        (void)ctx;

        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR camPos = invView.r[3];
        const float camX = XMVectorGetX(camPos);
        const float camY = XMVectorGetY(camPos);
        const float camZ = XMVectorGetZ(camPos);

        // Slowly wandering world-space wind heading — reads as gusty without
        // any per-flake noise field.
        m_windAngle += dt * (0.02f + 0.06f * std::sin(static_cast<float>(m_netClock) * 0.13f));
        const float windX = std::sin(m_windAngle);
        const float windZ = std::cos(m_windAngle);
        const float windMps = kWindBaseMps + kWindStormMps * m_intensity;

        // ------------------------------------------------------------ spawn
        const size_t target = static_cast<size_t>(std::lround(static_cast<double>(kStormMaxFlakes) * m_intensity));
        int spawned = 0;
        while (m_flakes.size() < target && spawned++ < kMaxSpawnPerFrame)
        {
            DustFlake f;
            const float ang = Rand01() * 6.2831853f;
            const float rad = kFlakeMinRadiusM + (kFlakeMaxRadiusM - kFlakeMinRadiusM) * Rand01();
            f.pos[0] = camX + std::cos(ang) * rad;
            f.pos[2] = camZ + std::sin(ang) * rad;
            f.pos[1] = camY - kFlakeYBelowM + (kFlakeYBelowM + kFlakeYAboveM) * Rand01();
            const float speed = windMps * (0.7f + 0.6f * Rand01());
            f.vel[0] = windX * speed + (Rand01() - 0.5f) * 3.0f;
            f.vel[2] = windZ * speed + (Rand01() - 0.5f) * 3.0f;
            f.vel[1] = (Rand01() - 0.5f) * 1.2f;
            f.life = 1.4f + 1.4f * Rand01();
            f.size = 1.1f + 1.6f * Rand01();
            m_flakes.push_back(f);
        }

        // ---------------------------------------------------------- advance
        for (DustFlake& f : m_flakes)
        {
            f.age += dt;
            f.pos[0] += f.vel[0] * dt;
            f.pos[1] += f.vel[1] * dt;
            f.pos[2] += f.vel[2] * dt;
        }
        std::erase_if(m_flakes,
                      [&](const DustFlake& f)
                      {
                          if (f.age >= f.life)
                              return true;
                          const float dx = f.pos[0] - camX;
                          const float dz = f.pos[2] - camZ;
                          return dx * dx + dz * dz > kFlakeKillDistM * kFlakeKillDistM;
                      });
        // Over-target population (intensity fading down) is not force-culled —
        // flakes simply age out, so the storm dissolves instead of popping.
        if (m_flakes.empty())
            return;

        // ------------------------------------------------------------ render
        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(kDustSheet));
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly); // test, never write

        // Billboard basis (TFGroundFx recipe): local X -> camera right, local
        // Z -> camera up, local Y (front face) -> toward the camera.
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        for (const DustFlake& f : m_flakes)
        {
            const float t = std::clamp(f.age / f.life, 0.0f, 1.0f);
            const int frame = std::min(kDustFrames - 1, static_cast<int>(t * kDustFrames));
            // Mid-life peak (4t(1-t)): flakes fade in, stream, fade out.
            const float fade = kFlakeAlpha * m_intensity * (4.0f * t * (1.0f - t));

            XMMATRIX bb = XMMatrixIdentity();
            bb.r[0] = XMVectorScale(right, f.size);
            bb.r[1] = toward;
            bb.r[2] = XMVectorScale(up, f.size);
            bb.r[3] = XMVectorSet(f.pos[0], f.pos[1], f.pos[2], 1.0f);

            gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(kFlakeColor[0], kFlakeColor[1], kFlakeColor[2], 1.0f),
                                      {1.0f / kDustFrames, 1.0f}, /*emissive*/ 1.0f, /*alpha*/ fade,
                                      XMFLOAT2(static_cast<float>(frame) / kDustFrames, 0.0f));
            m_quad->Render(dc);
        }
    }

    void TFWeatherFx::DrawScreenTint(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR forward = XMVector3Normalize(invView.r[2]); // camera look direction
        const XMVECTOR center = XMVectorAdd(invView.r[3], XMVectorScale(forward, kTintDistM));

        // Exact frustum extents at kTintDistM from the projection diagonal
        // (proj._11 = 1/(aspect*tan(fov/2)), proj._22 = 1/tan(fov/2)), plus a
        // margin so edges never peek at extreme aspect ratios.
        const float p00 = XMVectorGetX(proj.r[0]);
        const float p11 = XMVectorGetY(proj.r[1]);
        if (p00 <= 0.0f || p11 <= 0.0f)
            return; // not a perspective projection we understand — skip the tint
        const float fullW = 2.0f * (kTintDistM / p00) * kTintMargin;
        const float fullH = 2.0f * (kTintDistM / p11) * kTintMargin;

        XMMATRIX world = XMMatrixIdentity();
        world.r[0] = XMVectorScale(right, fullW);
        world.r[1] = XMVectorNegate(forward); // plane +Y normal faces the camera
        world.r[2] = XMVectorScale(up, fullH);
        world.r[3] = XMVectorSetW(center, 1.0f);

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr); // 1x1 white — pure color quad
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        const float alpha = kTintAlphaMax * m_intensity;
        gfx->UpdateBasicConstants(world, view, proj, XMFLOAT4(kTintColor[0], kTintColor[1], kTintColor[2], 1.0f),
                                  XMFLOAT2(1.0f, 1.0f), /*emissive*/ 1.0f, /*alpha*/ alpha, XMFLOAT2(0.0f, 0.0f));
        m_quad->Render(dc);
    }

} // namespace Terrafront
