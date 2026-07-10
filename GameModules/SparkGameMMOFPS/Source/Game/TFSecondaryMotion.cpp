/**
 * @file TFSecondaryMotion.cpp
 * @brief Damped pendulum-chain simulation (see TFSecondaryMotion.h for the
 *        contract). Verlet point masses with an ordered root-to-tip distance
 *        projection pass: exact link lengths every frame, one pass, no matrix
 *        solves — cheap enough to run per chain per frame without thought.
 */
#include "Game/TFSecondaryMotion.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    namespace
    {

        constexpr float kMaxChainDtSec = 0.05f; ///< clamp sim dt (alt-tab / hitches)
        constexpr float kMinChainDtSec = 1.0e-5f;
        constexpr float kDegenerateLenM = 1.0e-6f;

        bool Finite3(const DirectX::XMFLOAT3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        float Dist3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        /// World matrix for a unit cube stretched between two world points (same
        /// basis construction as the TFViewModel arm boxes / shot-FX tracer).
        DirectX::XMMATRIX BoxBetweenWorld(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float thickM)
        {
            using namespace DirectX;
            XMVECTOR va = XMVectorSet(a.x, a.y, a.z, 0.0f);
            XMVECTOR vb = XMVectorSet(b.x, b.y, b.z, 0.0f);
            XMVECTOR d = XMVectorSubtract(vb, va);
            float len = XMVectorGetX(XMVector3Length(d));
            if (len < 1.0e-4f)
                len = 1.0e-4f;
            XMVECTOR f = XMVectorScale(d, 1.0f / len);
            XMVECTOR upRef = (std::fabs(XMVectorGetY(f)) > 0.99f) ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
            XMVECTOR r = XMVector3Normalize(XMVector3Cross(upRef, f));
            XMVECTOR u = XMVector3Cross(f, r);
            XMMATRIX orient = XMMatrixIdentity();
            orient.r[0] = r;
            orient.r[1] = u;
            orient.r[2] = f;
            XMVECTOR mid = XMVectorScale(XMVectorAdd(va, vb), 0.5f);
            return XMMatrixScaling(thickM, thickM, len) * orient * XMMatrixTranslationFromVector(mid);
        }

    } // namespace

    // ------------------------------------------------------------------ chain

    void TFPendulumChain::Configure(const TFPendulumParams& params)
    {
        m_params = params;
        m_params.linkCount = std::clamp(m_params.linkCount, 1, 16);
        m_params.linkLengthM = std::max(m_params.linkLengthM, 1.0e-3f);
        // Normalize restDir; a zero vector falls back to straight down.
        const float rl =
            std::sqrt(m_params.restDir[0] * m_params.restDir[0] + m_params.restDir[1] * m_params.restDir[1] +
                      m_params.restDir[2] * m_params.restDir[2]);
        if (rl < kDegenerateLenM)
        {
            m_params.restDir[0] = 0.0f;
            m_params.restDir[1] = -1.0f;
            m_params.restDir[2] = 0.0f;
        }
        else
        {
            m_params.restDir[0] /= rl;
            m_params.restDir[1] /= rl;
            m_params.restDir[2] /= rl;
        }

        m_pos.assign(static_cast<size_t>(m_params.linkCount) + 1, DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});
        m_prev = m_pos;
        m_impulse = DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
        m_hasAnchor = false; // snap to the next Update's anchor
    }

    void TFPendulumChain::AddImpulse(const DirectX::XMFLOAT3& velWorldMps)
    {
        m_impulse.x += velWorldMps.x;
        m_impulse.y += velWorldMps.y;
        m_impulse.z += velWorldMps.z;
    }

    void TFPendulumChain::SnapToRest(const DirectX::XMFLOAT3& anchorWorld)
    {
        for (size_t i = 0; i < m_pos.size(); ++i)
        {
            const float d = m_params.linkLengthM * static_cast<float>(i);
            m_pos[i] =
                DirectX::XMFLOAT3{anchorWorld.x + m_params.restDir[0] * d, anchorWorld.y + m_params.restDir[1] * d,
                                  anchorWorld.z + m_params.restDir[2] * d};
        }
        m_prev = m_pos;
        m_impulse = DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
        m_hasAnchor = true;
    }

    void TFPendulumChain::Update(const DirectX::XMFLOAT3& anchorWorld, float dtSec)
    {
        if (m_pos.empty() || !Finite3(anchorWorld))
            return;

        // First frame, respawn teleport, or a sim blowup: snap instead of whip.
        const size_t tip = m_pos.size() - 1;
        if (!m_hasAnchor || Dist3(anchorWorld, m_pos[0]) > m_params.teleportSnapM || !Finite3(m_pos[tip]))
        {
            SnapToRest(anchorWorld);
            return;
        }

        m_pos[0] = anchorWorld;
        m_prev[0] = anchorWorld;

        const float dt = std::clamp(dtSec, 0.0f, kMaxChainDtSec);
        if (dt <= kMinChainDtSec)
            return;

        const float damp = std::max(0.0f, 1.0f - m_params.dampingPerSec * dt);
        const float dt2 = dt * dt;

        // Verlet integrate the free points: damped inertia + gravity + queued
        // impulse (a velocity change => impulse * dt of displacement) + optional
        // stiffness pull toward the rest pose off each point's parent.
        for (size_t i = 1; i < m_pos.size(); ++i)
        {
            const DirectX::XMFLOAT3 cur = m_pos[i];
            float ax = 0.0f, ay = -m_params.gravityMps2, az = 0.0f;
            if (m_params.stiffness > 0.0f)
            {
                const DirectX::XMFLOAT3& parent = m_pos[i - 1];
                ax += ((parent.x + m_params.restDir[0] * m_params.linkLengthM) - cur.x) * m_params.stiffness;
                ay += ((parent.y + m_params.restDir[1] * m_params.linkLengthM) - cur.y) * m_params.stiffness;
                az += ((parent.z + m_params.restDir[2] * m_params.linkLengthM) - cur.z) * m_params.stiffness;
            }
            m_pos[i].x = cur.x + (cur.x - m_prev[i].x) * damp + m_impulse.x * dt + ax * dt2;
            m_pos[i].y = cur.y + (cur.y - m_prev[i].y) * damp + m_impulse.y * dt + ay * dt2;
            m_pos[i].z = cur.z + (cur.z - m_prev[i].z) * damp + m_impulse.z * dt + az * dt2;
            m_prev[i] = cur;
        }
        m_impulse = DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};

        // Ordered constraint pass, root to tip: each point is projected to exact
        // link length from its (already-final) parent. One pass == rigid links.
        for (size_t i = 1; i < m_pos.size(); ++i)
        {
            const DirectX::XMFLOAT3& parent = m_pos[i - 1];
            float dx = m_pos[i].x - parent.x;
            float dy = m_pos[i].y - parent.y;
            float dz = m_pos[i].z - parent.z;
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < kDegenerateLenM)
            {
                dx = m_params.restDir[0];
                dy = m_params.restDir[1];
                dz = m_params.restDir[2];
            }
            else
            {
                const float inv = 1.0f / len;
                dx *= inv;
                dy *= inv;
                dz *= inv;
            }
            m_pos[i].x = parent.x + dx * m_params.linkLengthM;
            m_pos[i].y = parent.y + dy * m_params.linkLengthM;
            m_pos[i].z = parent.z + dz * m_params.linkLengthM;
        }
    }

    void TFPendulumChain::GetLinkEndpoints(int link, DirectX::XMFLOAT3& outA, DirectX::XMFLOAT3& outB) const
    {
        if (link < 0 || link >= LinkCount())
        {
            outA = outB = DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
            return;
        }
        outA = m_pos[static_cast<size_t>(link)];
        outB = m_pos[static_cast<size_t>(link) + 1];
    }

    DirectX::XMMATRIX TFPendulumChain::LinkWorld(int link, float thickM) const
    {
        if (link < 0 || link >= LinkCount())
            return DirectX::XMMatrixIdentity();
        return BoxBetweenWorld(m_pos[static_cast<size_t>(link)], m_pos[static_cast<size_t>(link) + 1], thickM);
    }

    DirectX::XMMATRIX TFPendulumChain::TipWorld(const DirectX::XMFLOAT3& scaleM) const
    {
        using namespace DirectX;
        if (m_pos.size() < 2)
            return XMMatrixIdentity();
        const XMFLOAT3& a = m_pos[m_pos.size() - 2];
        const XMFLOAT3& b = m_pos[m_pos.size() - 1];
        XMVECTOR va = XMVectorSet(a.x, a.y, a.z, 0.0f);
        XMVECTOR vb = XMVectorSet(b.x, b.y, b.z, 0.0f);
        XMVECTOR d = XMVectorSubtract(vb, va);
        float len = XMVectorGetX(XMVector3Length(d));
        if (len < 1.0e-4f)
            len = 1.0e-4f;
        XMVECTOR f = XMVectorScale(d, 1.0f / len);
        XMVECTOR upRef = (std::fabs(XMVectorGetY(f)) > 0.99f) ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
        XMVECTOR r = XMVector3Normalize(XMVector3Cross(upRef, f));
        XMVECTOR u = XMVector3Cross(f, r);
        XMMATRIX orient = XMMatrixIdentity();
        orient.r[0] = r;
        orient.r[1] = u;
        orient.r[2] = f;
        // Center the fob half its depth past the tip so it hangs off the chain.
        XMVECTOR center = XMVectorAdd(vb, XMVectorScale(f, scaleM.z * 0.5f));
        return XMMatrixScaling(scaleM.x, scaleM.y, scaleM.z) * orient * XMMatrixTranslationFromVector(center);
    }

    // --------------------------------------------------------------- registry

    TFSecondaryMotion& TFSecondaryMotion::Get()
    {
        static TFSecondaryMotion s_instance;
        return s_instance;
    }

    TFPendulumHandle TFSecondaryMotion::AttachPendulum(const TFPendulumParams& params)
    {
        const TFPendulumHandle handle = m_nextHandle++;
        if (m_nextHandle == 0)
            m_nextHandle = 1; // wrap past the invalid handle (theoretical)
        m_chains[handle].Configure(params);
        return handle;
    }

    void TFSecondaryMotion::Detach(TFPendulumHandle handle)
    {
        if (handle != 0)
            m_chains.erase(handle);
    }

    TFPendulumChain* TFSecondaryMotion::Find(TFPendulumHandle handle)
    {
        if (handle == 0)
            return nullptr;
        auto it = m_chains.find(handle);
        return (it != m_chains.end()) ? &it->second : nullptr;
    }

} // namespace Terrafront
