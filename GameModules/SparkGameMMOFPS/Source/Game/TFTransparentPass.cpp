/**
 * @file TFTransparentPass.cpp
 * @brief Sorted alpha-blended draw pass (see TFTransparentPass.h for the
 *        contract and the depth-write limitation note).
 */
#include "Game/TFTransparentPass.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace Terrafront
{

    TFTransparentPass& TFTransparentPass::Get()
    {
        static TFTransparentPass s_instance;
        return s_instance;
    }

    void TFTransparentPass::Submit(Mesh* mesh, const DirectX::XMMATRIX& world, ID3D11ShaderResourceView* srv,
                                   const DirectX::XMFLOAT4& tint, float emissive)
    {
        if (!mesh)
            return;
        if (m_items.size() >= kMaxItems)
        {
            ++m_dropped;
            if (!m_warnedDrop)
            {
                m_warnedDrop = true;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] TFTransparentPass: > %zu items queued without a Flush — dropping "
                               "(is the flush call site wired into RenderWorld?)",
                               kMaxItems);
            }
            return;
        }
        Item it{};
        it.mesh = mesh;
        DirectX::XMStoreFloat4x4(&it.world, world); // unaligned store (see header: C2338)
        it.srv = srv;
        it.tint = tint;
        it.emissive = emissive;
        it.dist2 = 0.0f;
        m_items.push_back(it);
    }

    void TFTransparentPass::Flush(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (m_items.empty())
            return;
        if (!gfx || !gfx->GetContext())
        {
            // No device this frame (headless / teardown): never let the queue grow.
            m_items.clear();
            return;
        }

        // Camera position = translation row of the inverse view matrix (same
        // extraction TFWorldSetup::RenderWorld uses for the frame constants).
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        XMFLOAT3 cam;
        XMStoreFloat3(&cam, invView.r[3]);

        for (Item& it : m_items)
        {
            // World translation row of the stored (unaligned) matrix.
            const float dx = it.world._41 - cam.x, dy = it.world._42 - cam.y, dz = it.world._43 - cam.z;
            it.dist2 = dx * dx + dy * dy + dz * dz;
        }
        // Back-to-front: farthest first so nearer alpha surfaces composite over
        // farther ones.
        std::stable_sort(m_items.begin(), m_items.end(),
                         [](const Item& a, const Item& b) { return a.dist2 > b.dist2; });

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
        // Depth READ-ONLY (W9): test against opaque geometry but never write,
        // so transparent surfaces stop occluding anything drawn after Flush
        // (the W8 artifact class the header's depth policy note describes).
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        m_drawnLastFlush = 0;
        for (const Item& it : m_items)
        {
            if (!it.mesh || it.mesh->GetIndexCount() == 0)
                continue;
            // tint.rgb -> ObjectColor.rgb (ObjectColor.a pinned to 1);
            // tint.w   -> MaterialProperties.w so alpha applies exactly once
            // (the basic PS multiplies texAlpha * ObjectColor.a * MatProps.w).
            const XMFLOAT4 color{it.tint.x, it.tint.y, it.tint.z, 1.0f};
            const XMMATRIX worldM = XMLoadFloat4x4(&it.world);
            gfx->UpdateBasicConstants(worldM, view, proj, color, XMFLOAT2(1.0f, 1.0f), it.emissive, it.tint.w);
            gfx->SetBasicTexture(it.srv);
            it.mesh->Render(dc);
            ++m_drawnLastFlush;
        }

        // Restore the opaque defaults for whatever draws after us (HUD, next frame).
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
        m_items.clear();
    }

    Mesh* TFTransparentPass::GetUnitPlane(GraphicsEngine& gfx)
    {
        using namespace DirectX;

        if (m_unitPlane)
            return (m_unitPlane->GetIndexCount() > 0) ? m_unitPlane.get() : nullptr;
        if (m_planeBuildFailed || !gfx.GetDevice() || !gfx.GetContext())
            return nullptr;

        // Two-sided unit quad in the XY plane at z = 0 (see header). Both
        // windings are emitted so back-face culling shows the quad from either
        // side without touching the rasterizer state; normals face the viewer
        // side so the basic PS's ambient + N.L stays sane (emissive carries
        // most of the brightness for energy surfaces anyway).
        const XMFLOAT3 p0{-0.5f, 0.5f, 0.0f}, p1{0.5f, 0.5f, 0.0f};
        const XMFLOAT3 p2{0.5f, -0.5f, 0.0f}, p3{-0.5f, -0.5f, 0.0f};
        const XMFLOAT2 t0{0.0f, 0.0f}, t1{1.0f, 0.0f}, t2{1.0f, 1.0f}, t3{0.0f, 1.0f};

        std::vector<Vertex> verts;
        verts.reserve(8);
        // Face A: front-facing to a camera on the -Z side (clockwise from -Z).
        const XMFLOAT3 nA{0.0f, 0.0f, -1.0f};
        verts.emplace_back(p0, nA, t0);
        verts.emplace_back(p1, nA, t1);
        verts.emplace_back(p2, nA, t2);
        verts.emplace_back(p3, nA, t3);
        // Face B: same positions, opposite normal, reversed winding (+Z side).
        const XMFLOAT3 nB{0.0f, 0.0f, 1.0f};
        verts.emplace_back(p0, nB, t0);
        verts.emplace_back(p1, nB, t1);
        verts.emplace_back(p2, nB, t2);
        verts.emplace_back(p3, nB, t3);

        const std::vector<unsigned int> inds = {
            0, 1, 2, 0, 2, 3, // face A (CW viewed from -Z)
            4, 6, 5, 4, 7, 6, // face B (CW viewed from +Z)
        };

        auto mesh = std::make_unique<Mesh>();
        if (FAILED(mesh->Initialize(gfx.GetDevice(), gfx.GetContext())) ||
            FAILED(mesh->CreateFromVertices(verts, inds)))
        {
            m_planeBuildFailed = true;
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] TFTransparentPass: unit plane creation failed");
            return nullptr;
        }
        m_unitPlane = std::move(mesh);
        return m_unitPlane.get();
    }

} // namespace Terrafront
