/**
 * @file TFPingSystemFx.cpp
 * @brief TFPingSystem client rendering: the billboarded diamond world markers
 *        (additive, camera-facing, TFGroundFx recipe) drawn once per frame from
 *        TFWorldSetup::RenderWorld. Split from TFPingSystem.cpp; the marker
 *        presentation constants are file-local to this TU.
 */
#include "Game/TFPingSystem.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    namespace
    {
        // Marker presentation (client only).
        constexpr float kMarkerBaseSizeM = 0.55f;  ///< diamond half-diagonal at close range
        constexpr float kMarkerDistScale = 0.012f; ///< extra size per meter (keeps it readable far away)
        constexpr float kMarkerMaxSizeM = 3.2f;    ///< size cap at long range
        constexpr float kMarkerBobAmpM = 0.14f;    ///< gentle vertical bob amplitude
        constexpr float kMarkerBobHz = 0.7f;       ///< bob cycles per second
        constexpr float kMarkerHeadM = 1.9f;       ///< Enemy pings anchor over the pawn head
        constexpr float kMarkerLiftM = 0.55f;      ///< Location/Support pings hover over the hit point

        constexpr float kSqrtHalf = 0.70710678f; // cos/sin 45 deg (diamond rotation)

        float MarkerAnchorY(TFPingType type, float posY)
        {
            return posY + (type == TFPingType::Enemy ? kMarkerHeadM : kMarkerLiftM);
        }
    } // namespace

    // ---------------------------------------------------------------------------
    // Client: world markers (TFWorldSetup::RenderWorld hook)
    // ---------------------------------------------------------------------------

    void TFPingSystem::RenderClientFx(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!m_initialized || !gfx || !gfx->GetDevice() || !gfx->GetContext() || m_clientPings.empty())
            return;

        if (!m_quad)
        {
            m_quad = std::make_unique<Mesh>();
            m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
        }
        if (!m_quad || m_quad->GetIndexCount() == 0)
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();

        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr); // default 1x1 white; the color below is the marker tint
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        // Billboard basis from inv(view) rows (TFGroundFx / TFGrenadeSystem boom
        // recipe), with the plane's local X/Z axes rotated 45 deg inside the
        // (right, up) screen plane so the quad reads as a diamond.
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        const XMVECTOR camPos = invView.r[3];

        for (const auto& [id, p] : m_clientPings)
        {
            (void)id;
            float col[4];
            PingColor(p.type, col);

            const float bob = std::sin(p.age * kMarkerBobHz * 2.0f * 3.14159265f) * kMarkerBobAmpM;
            const XMVECTOR anchor = XMVectorSet(p.pos[0], MarkerAnchorY(p.type, p.pos[1]) + bob, p.pos[2], 1.0f);

            const float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(anchor, camPos)));
            const float size = std::min(kMarkerBaseSizeM + dist * kMarkerDistScale, kMarkerMaxSizeM);
            const float fade = std::clamp(p.lifeLeft / kTFPingMarkerFadeSec, 0.0f, 1.0f);
            if (fade <= 0.01f)
                continue;

            const XMVECTOR axisX =
                XMVectorScale(XMVectorAdd(XMVectorScale(right, kSqrtHalf), XMVectorScale(up, kSqrtHalf)), size);
            const XMVECTOR axisZ =
                XMVectorScale(XMVectorAdd(XMVectorScale(right, -kSqrtHalf), XMVectorScale(up, kSqrtHalf)), size);

            XMMATRIX bb = XMMatrixIdentity();
            bb.r[0] = axisX;
            bb.r[1] = toward;
            bb.r[2] = axisZ;
            bb.r[3] = XMVectorSetW(anchor, 1.0f);

            gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(col[0] * 2.2f, col[1] * 2.2f, col[2] * 2.2f, 1.0f),
                                      XMFLOAT2(1.0f, 1.0f), /*emissive*/ 1.0f, /*alpha*/ 0.85f * fade,
                                      XMFLOAT2(0.0f, 0.0f));
            m_quad->Render(dc);
        }

        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
