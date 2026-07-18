/**
 * @file TFGrenadeSystemFx.cpp
 * @brief TFGrenadeSystem client rendering: replicated grenade bodies (sphere),
 *        the additive boom flipbook, smoke puff billboards and the flash
 *        whiteout ImGui overlay. Split from TFGrenadeSystem.cpp; the shared
 *        boom lifetime lives in TFGrenadeSystemInternal.h.
 */
#include "Game/TFGrenadeSystem.h"

#include "Game/TFGrenadeSystemInternal.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>

namespace Terrafront
{

    using namespace GrenadeSysDetail;

    namespace
    {
        // Boom presentation (client only).
        constexpr float kBoomSize0M = 1.0f;
        constexpr float kBoomSize1M = 3.8f;
        constexpr int kBoomFrames = 4;
        constexpr char kBoomSheet[] = "Assets/Textures/MMOFPS/fx/muzzle_flash_common.png";
    } // namespace

    // ---------------------------------------------------------------------------
    // Client: rendering (TFWorldSetup::RenderWorld hook)
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::RenderClientFx(GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                         const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!m_initialized || !gfx || !gfx->GetDevice() || !gfx->GetContext())
            return;
        if (m_clientGrenades.empty() && m_booms.empty() && m_smokePuffs.empty())
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();

        // ------------------------------------------------------------ bodies
        if (!m_clientGrenades.empty())
        {
            if (!m_sphere)
            {
                m_sphere = std::make_unique<Mesh>();
                m_sphere->Initialize(gfx->GetDevice(), gfx->GetContext());
                m_sphere->CreateSphere(kTFGrenadeRadiusM, 10, 8);
            }
            if (m_sphere && m_sphere->GetIndexCount() > 0)
            {
                gfx->SetBasicShaders();
                gfx->SetBasicTexture(nullptr); // default 1x1 white; color below is the body tint
                for (const auto& [id, g] : m_clientGrenades)
                {
                    (void)id;
                    const XMMATRIX w = XMMatrixTranslation(g.pos[0], g.pos[1], g.pos[2]);
                    gfx->UpdateBasicConstants(w, view, proj, XMFLOAT4(0.10f, 0.10f, 0.12f, 1.0f), XMFLOAT2(1.0f, 1.0f));
                    m_sphere->Render(dc);
                }
            }
        }

        // ------------------------------------------------------------ booms
        if (!m_booms.empty())
        {
            if (!m_quad)
            {
                m_quad = std::make_unique<Mesh>();
                m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
                m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
            }
            if (m_quad && m_quad->GetIndexCount() > 0)
            {
                gfx->SetBasicShaders();
                gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(kBoomSheet));
                gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
                gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

                // Billboard basis from inv(view) rows (TFGroundFx recipe): local
                // X -> camera right, local Z -> camera up, local Y -> toward the
                // camera; determinant stays +1 for CreatePlane's winding.
                const XMMATRIX invView = XMMatrixInverse(nullptr, view);
                const XMVECTOR right = XMVector3Normalize(invView.r[0]);
                const XMVECTOR up = XMVector3Normalize(invView.r[1]);
                const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

                for (const BoomFx& b : m_booms)
                {
                    const float t = std::clamp(b.age / kBoomLifeSec, 0.0f, 1.0f);
                    const float size = kBoomSize0M + (kBoomSize1M - kBoomSize0M) * t;
                    const int frame = std::min(kBoomFrames - 1, static_cast<int>(t * kBoomFrames));
                    const float fade = (1.0f - t) * (1.0f - t); // ease-out

                    XMMATRIX bb = XMMatrixIdentity();
                    bb.r[0] = XMVectorScale(right, size);
                    bb.r[1] = toward;
                    bb.r[2] = XMVectorScale(up, size);
                    bb.r[3] = XMVectorSet(b.pos[0], b.pos[1], b.pos[2], 1.0f);

                    gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(6.0f, 4.5f, 2.0f, 1.0f),
                                              XMFLOAT2(1.0f / kBoomFrames, 1.0f), /*emissive*/ 1.0f, /*alpha*/ fade,
                                              XMFLOAT2(static_cast<float>(frame) / kBoomFrames, 0.0f));
                    m_quad->Render(dc);
                }

                gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
                gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
                gfx->SetBasicTexture(nullptr);
            }
        }

        // ------------------------------------------------- loadout-depth wave: smoke puffs
        // Reuses the procedural soft-circle disc (GetOrCreateSoftCircleShadowSRV,
        // TFBlobShadows precedent) — no new texture asset needed.
        if (!m_smokePuffs.empty())
        {
            if (ID3D11ShaderResourceView* soft = gfx->GetOrCreateSoftCircleShadowSRV())
            {
                if (!m_quad)
                {
                    m_quad = std::make_unique<Mesh>();
                    m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
                    m_quad->CreatePlane(1.0f, 1.0f);
                }
                if (m_quad && m_quad->GetIndexCount() > 0)
                {
                    gfx->SetBasicShaders();
                    gfx->SetBasicTexture(soft);
                    gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
                    gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

                    const XMMATRIX invView = XMMatrixInverse(nullptr, view);
                    const XMVECTOR right = XMVector3Normalize(invView.r[0]);
                    const XMVECTOR up = XMVector3Normalize(invView.r[1]);
                    const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

                    for (const SmokePuff& p : m_smokePuffs)
                    {
                        const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
                        // Quick bloom-in over the first third of the life, then a
                        // slow linear fade to nothing by the end.
                        const float size = p.radiusM * (0.35f + 0.65f * std::min(1.0f, t * 3.0f));
                        const float fade = (t < 0.15f) ? (t / 0.15f) : (1.0f - (t - 0.15f) / 0.85f);
                        if (fade <= 0.0f)
                            continue;

                        XMMATRIX bb = XMMatrixIdentity();
                        bb.r[0] = XMVectorScale(right, size);
                        bb.r[1] = toward;
                        bb.r[2] = XMVectorScale(up, size);
                        bb.r[3] = XMVectorSet(p.pos[0], p.pos[1], p.pos[2], 1.0f);

                        gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(0.72f, 0.72f, 0.70f, 1.0f),
                                                  XMFLOAT2(1.0f, 1.0f), /*emissive*/ 0.0f, /*alpha*/ fade * 0.6f);
                        m_quad->Render(dc);
                    }

                    gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
                    gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
                    gfx->SetBasicTexture(nullptr);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // loadout-depth wave: flash whiteout overlay (TFOpticsSystem::RenderOverlay
    // precedent — plain foreground-drawlist writes, no window, no input capture)
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::RenderFlashOverlay()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::GetCurrentContext() || !IsLocalFlashed())
            return;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;

        const float remainingSec = static_cast<float>(m_localFlashUntil - NowSec());
        const float alpha =
            m_localFlashDurationSec > 0.0f ? std::clamp(remainingSec / m_localFlashDurationSec, 0.0f, 1.0f) : 0.0f;
        if (alpha <= 0.0f)
            return;

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                          IM_COL32(255, 255, 255, static_cast<int>(235.0f * alpha)));
#endif // SPARK_HAS_IMGUI
    }

} // namespace Terrafront
