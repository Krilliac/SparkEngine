/**
 * @file TFDeployableSystem.cpp
 * @brief Fabricator turret / ammo pack + Medtech beacon (W3) + resupply
 *        station / AV turret / shield wall (W6): lifecycle, ECS entities +
 *        prop visuals, the W8 shield-wall energy plane, snapshot queries and
 *        debug UI. Placement validation + the ShieldWall Jolt body live in
 *        TFDeployablePlacement.cpp; server placement/damage in
 *        TFDeployableSystemServer.cpp, the server-tick mechanics in
 *        TFDeployableSystemTick.cpp and 0x54FC-0x54FE replication in
 *        TFDeployableSystemNet.cpp (shared bits: TFDeployableSystemInternal.h).
 *
 * Tick driving: Main.cpp (frozen) routes only Update() to this system, so all
 * mechanics run on the render tick with dt accumulation — none of them need
 * fixed-step determinism (timers, radius checks, hitscan through TFDamageSystem).
 * FixedUpdate stays a contract no-op so a future Main wiring cannot double-tick.
 */
#include "Game/TFDeployableSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFDeployableTypes.h"
#include "Game/TFTransparentPass.h"
#include "Game/TFVisualUtils.h"

#include "Engine/ECS/Components.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <string>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;

        // --- W8 render-transparency lane: shield-wall energy plane --------------
        // Frame-local rectangle of the energy plane inside dep_shieldwall.obj
        // (meters, unscaled): posts at x = +-1.45 (inner clearance ~1.32), emitter
        // bar top at y ~0.34, post caps at y ~2.28. Multiplied by the SAME visual
        // scale the frame renders with so the plane always fills the frame.
        constexpr float kShieldPlaneHalfW = 1.30f;
        constexpr float kShieldPlaneY0 = 0.36f;
        constexpr float kShieldPlaneY1 = 2.18f;
        constexpr float kShieldPlaneEmissive = 0.6f;    // unlit glow (MaterialProperties.z)
        constexpr float kShieldPlaneAlphaMin = 0.28f;   // near-destroyed wall
        constexpr float kShieldPlaneAlphaMax = 0.55f;   // full-health wall
        constexpr float kShieldPlaneFactionMix = 0.65f; // white -> faction color blend
        constexpr const char* kShieldPlaneTexture = "Assets/Textures/MMOFPS/fx/shield_plane.png";

        /// Queue one shield wall's energy plane into the transparent pass (pure
        /// presentation; drawn by TFTransparentPass::Flush after all opaque
        /// passes — flush call site lives in TFWorldSetup::RenderWorld).
        void SubmitShieldWallPlane(TFGameContext& ctx, const TFDeployableView& view, Mesh* plane,
                                   ID3D11ShaderResourceView* srv)
        {
            using namespace DirectX;

            // Match the rendered frame's scale exactly: bespoke data row first,
            // else the aliased row * visualScaleMult — the same resolve order as
            // AttachDeployableVisual (never stack both).
            const DeployableVisualDef* dv = nullptr;
            bool aliased = false;
            if (ctx.data && ctx.data->IsLoaded())
            {
                dv = ctx.data->GetDeployableVisual(view.kind);
                if (!dv)
                {
                    dv = ctx.data->GetDeployableVisual(TFDeployVisualAlias(view.kind));
                    aliased = true;
                }
            }
            if (!dv)
                return; // no data tables -> no frame rendered -> no plane either
            const TFDeployableSpec* spec = TFDeploySpecOf(view.kind);
            const float sx = dv->scale[0] * ((aliased && spec) ? spec->visualScaleMult[0] : 1.0f);
            const float sy = dv->scale[1] * ((aliased && spec) ? spec->visualScaleMult[1] : 1.0f);

            const float w = 2.0f * kShieldPlaneHalfW * sx;
            const float h = (kShieldPlaneY1 - kShieldPlaneY0) * sy;
            const float cy = 0.5f * (kShieldPlaneY0 + kShieldPlaneY1) * sy;
            // Same composition as Transform::GetLocalMatrix (S * R * T); yaw stays
            // in RADIANS here (the ECS/PhysicsBody degree conversions are their
            // consumers' concern, not this draw's).
            const XMMATRIX world = XMMatrixScaling(w, h, 1.0f) * XMMatrixRotationY(view.yaw) *
                                   XMMatrixTranslation(view.pos[0], view.pos[1] + cy, view.pos[2]);

            // Faction-colored energy; alpha tracks health so a battered wall
            // visibly thins out before it pops.
            float fcol[4];
            FactionColor(view.faction, fcol);
            const float healthFrac =
                (view.maxHealth > 0.0f) ? std::clamp(view.health / view.maxHealth, 0.0f, 1.0f) : 1.0f;
            const float alpha = kShieldPlaneAlphaMin + (kShieldPlaneAlphaMax - kShieldPlaneAlphaMin) * healthFrac;
            const XMFLOAT4 tint{1.0f + kShieldPlaneFactionMix * (fcol[0] - 1.0f),
                                1.0f + kShieldPlaneFactionMix * (fcol[1] - 1.0f),
                                1.0f + kShieldPlaneFactionMix * (fcol[2] - 1.0f), alpha};

            TFTransparentPass::Get().Submit(plane, world, srv, tint, kShieldPlaneEmissive);
        }

    } // namespace

    TFDeployableSystem::TFDeployableSystem() = default;
    TFDeployableSystem::~TFDeployableSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDeployableSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDeployableSystem initialized");
        return true;
    }

    void TFDeployableSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;

        if (m_ctx->IsAuthority())
        {
            ServerTick(deltaTime);
        }
        else
        {
            // client mirror: decay lifetimes locally between server keepalives
            for (auto& [entity, rec] : m_deployables)
                rec.view.life = std::max(0.0f, rec.view.life - deltaTime);
        }

        // --- W8 render-transparency: queue shield-wall energy planes ------------
        // Pure presentation, both roles (listen host draws its own authority set,
        // pure clients their replicated mirror); headless (no device) skips, so
        // nothing ever queues without a Flush. Lazy plane/SRV lookup only when a
        // wall actually exists this frame.
        if (GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr;
            gfx && gfx->GetDevice() && gfx->GetContext())
        {
            Mesh* plane = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            bool resolved = false;
            for (const auto& [entity, rec] : m_deployables)
            {
                if (rec.view.kind != kDeployShieldWall)
                    continue;
                if (!resolved)
                {
                    resolved = true;
                    plane = TFTransparentPass::Get().GetUnitPlane(*gfx);
                    srv = gfx->GetOrLoadTextureSRV(kShieldPlaneTexture);
                }
                if (!plane)
                    break; // quad creation failed — visual-only, skip quietly
                SubmitShieldWallPlane(*m_ctx, rec.view, plane, srv);
            }
        }

#ifdef ENABLE_NETWORKING
        if (ClientNetActive())
        {
            if (!m_clientHandlers)
                ClientEnsureHandlers();
        }
        else if (m_clientHandlers)
        {
            ClientReleaseHandlers();
            for (auto& [entity, rec] : m_deployables)
                DestroyLocalEntity(rec.local);
            m_deployables.clear();
        }
#endif
    }

    // Main.cpp does not drive FixedUpdate for this system; see file header.
    void TFDeployableSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFDeployableSystem::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ClientReleaseHandlers();
        m_knownClients.clear();
#endif
        for (auto& [entity, rec] : m_deployables)
        {
            ReleaseWallBody(rec);
            DestroyLocalEntity(rec.local);
        }
        m_deployables.clear();
        m_ownerIndex.clear();
        m_pendingAvShots.clear();
        m_initialized = false;
    }

    double TFDeployableSystem::NowSec() const
    {
        return m_clock;
    }

    // ---------------------------------------------------------------------------
    // Entities & visuals (both sides)
    // ---------------------------------------------------------------------------

    uint32_t TFDeployableSystem::CreateDeployableEntity(const TFDeployableView& view)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return 0;

        EntityID e = world->CreateEntity("TF_Deploy_K" + std::to_string(static_cast<unsigned>(view.kind)));
        if (static_cast<uint32_t>(e) == 0) // id 0 is reserved (same dodge as pawns)
            e = world->CreateEntity("TF_Deploy_K" + std::to_string(static_cast<unsigned>(view.kind)));

        Transform& t = world->AddComponent<Transform>(e);
        t.position = {view.pos[0], view.pos[1], view.pos[2]};
        t.rotation.y = view.yaw * kRadToDeg;

        NetworkIdentity& ni = world->AddComponent<NetworkIdentity>(e);
        ni.networkID = (view.entity != 0) ? view.entity : static_cast<uint32_t>(e);
        ni.ownerClientID = view.owner;
        ni.isLocalAuthority = m_ctx->IsAuthority();

        TFDeployableComp& dc = world->AddComponent<TFDeployableComp>(e);
        dc.kind = view.kind;
        dc.owner = view.owner;
        dc.life = view.life;

        world->AddComponent<TFFactionComp>(e).faction = view.faction;

        AttachDeployableVisual(static_cast<uint32_t>(e), view.kind, view.faction);
        return static_cast<uint32_t>(e);
    }

    void TFDeployableSystem::DestroyLocalEntity(uint32_t local)
    {
        if (local == 0)
            return;
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(local);
        if (world && world->GetRegistry().valid(e))
            world->DestroyEntity(e);
    }

    void TFDeployableSystem::AttachDeployableVisual(uint32_t local, DeployableKind kind, FactionId faction)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(local);
        if (!world || !world->GetRegistry().valid(e) || world->HasComponent<MeshRenderer>(e))
            return;

        // Prop stand-ins (Assets/Models/MMOFPS/props, W1 asset staging): turret =
        // antenna mast, ammo pack = crate, med beacon = slim antenna (data-driven
        // via Assets/MMOFPS/Data/deployables.json). Faction is telegraphed by
        // material like pawn visuals; bespoke meshes are TF-W4. W6 kinds try their
        // own data row first, then fall back to the aliased W3 row scaled by the
        // spec's visualScaleMult (TFDeployableTypes.h).
        const DeployableVisualDef* dv = nullptr;
        bool aliased = false;
        if (m_ctx->data && m_ctx->data->IsLoaded())
        {
            dv = m_ctx->data->GetDeployableVisual(kind);
            if (!dv)
            {
                dv = m_ctx->data->GetDeployableVisual(TFDeployVisualAlias(kind));
                aliased = true;
            }
        }
        if (!dv)
            return; // no data table loaded (e.g. headless harness) - nothing to draw

        MeshRenderer& mr = world->AddComponent<MeshRenderer>(e);
        mr.meshPath = "Assets/" + dv->model;
        mr.materialPath = FactionStructureMaterial(*m_ctx, faction);
        mr.castShadows = true;

        // visualScaleMult only compensates the ALIASED base row; a bespoke data
        // row already carries its final scale (integration note: never stack both).
        const TFDeployableSpec* spec = TFDeploySpecOf(kind);
        const float* mult = (aliased && spec) ? spec->visualScaleMult : nullptr;
        if (Transform* t = world->GetComponent<Transform>(e))
            t->scale = {dv->scale[0] * (mult ? mult[0] : 1.0f), dv->scale[1] * (mult ? mult[1] : 1.0f),
                        dv->scale[2] * (mult ? mult[2] : 1.0f)};
    }

    // ---------------------------------------------------------------------------
    // Queries (both sides)
    // ---------------------------------------------------------------------------

    bool TFDeployableSystem::GetDeployable(EntityId entity, TFDeployableView& out) const
    {
        auto it = m_deployables.find(entity);
        if (it == m_deployables.end())
            return false;
        out = it->second.view;
        return true;
    }

    void TFDeployableSystem::ForEachDeployable(const std::function<void(const TFDeployableView&)>& fn) const
    {
        for (const auto& [entity, rec] : m_deployables)
            fn(rec.view);
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Deployables"))
            return;
        ImGui::Text("live: %zu  placed: %u  refused: %u  expired: %u  killed: %u", m_deployables.size(), m_placed,
                    m_refusedPlacements, m_expired, m_destroyedByDamage);
        ImGui::Text("turret shots: %u  AV shots: %u  healed: %.0f hp  repaired: %.0f hp", m_turretShots, m_avShots,
                    m_healGiven, m_repairGiven);
        for (const auto& [entity, rec] : m_deployables)
        {
            const TFDeployableSpec* sp = TFDeploySpecOf(rec.view.kind);
            ImGui::Text("e%u %s p%u %s hp=%.0f/%.0f life=%.0fs pos=(%.0f %.0f %.0f)", entity, sp ? sp->name : "?",
                        rec.view.owner, FactionTag(rec.view.faction), rec.view.health, rec.view.maxHealth,
                        rec.view.life, rec.view.pos[0], rec.view.pos[1], rec.view.pos[2]);
        }
#endif
    }

} // namespace Terrafront
