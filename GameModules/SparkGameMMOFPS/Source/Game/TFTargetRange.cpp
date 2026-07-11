/**
 * @file TFTargetRange.cpp
 * @brief Sanctuary firing range (see the header for the cosmetic-only
 *        contract). Layout: two lanes (x = 388 / 420) x three ranges
 *        (20/40/60 m) north of a crate firing line at z = 3880, on the NE
 *        sanctuary plateau (farthest dummy is 192 m from the pad center —
 *        still on the flat 24 m deck, radius 200).
 *
 * COSMETIC ONLY: every number shown here is a local estimate for training
 * feedback. The server never sees these dummies, no TFMsg is involved, and
 * nothing here may ever feed back into damage, movement or the wire.
 */
#include "Game/TFTargetRange.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Engine/ECS/Components.h" // Transform, MeshRenderer
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator> // std::size

namespace Terrafront
{

    namespace
    {

        // ---- layout (all on the flat sanctuary plateau, Y = 24) ---------------
        constexpr float kLineZ = 3880.0f; ///< firing line (crates + info sign)
        constexpr float kLaneAX = 388.0f; ///< west lane
        constexpr float kLaneBX = 420.0f; ///< east lane
        constexpr float kRanges[3] = {20.0f, 40.0f, 60.0f};
        constexpr float kRangeCenterX = 404.0f; ///< draw/perf gate center
        constexpr float kDrawWithinM = 150.0f;  ///< skip all readout work beyond this

        // ---- hit model (generous training-dummy capsule) -----------------------
        constexpr float kDummyRadiusM = 0.75f;
        constexpr float kDummyFootY = 0.10f;
        constexpr float kDummyHeadY = 1.95f;
        constexpr float kMaxHitDistM = 250.0f;

        // ---- presentation -------------------------------------------------------
        constexpr float kFlashDecayPerSec = 6.0f; ///< ~0.17 s full flash
        constexpr float kFlashEmissive = 2.5f;    ///< peak emissive on hit
        constexpr float kFloaterLifeSec = 1.2f;
        constexpr float kFloaterRiseMps = 1.4f;
        constexpr double kDpsIdleResetSec = 4.0;  ///< gap that opens a new window
        constexpr double kDpsShowForSec = 6.0;    ///< sign readout linger
        constexpr double kDpsMinWindowSec = 0.75; ///< first-hit divisor floor
        // Must match TFWorldSetup::ComputeViewProj's first-person branch
        // (XM_PIDIV4 * 1.6, near 0.3) — cosmetic drift tolerance only.
        constexpr float kFovY = 0.78539816f * 1.6f;
        constexpr float kNearZ = 0.3f;

        constexpr const char* kMatGray = "Assets/Materials/MMOFPS/Structure_Concrete.json";
        constexpr const char* kMatSanct = "Assets/Materials/MMOFPS/Structure_Sanctuary.json";

        /// Neutral-gray class bodies, one silhouette per range so lanes read
        /// at a glance (index = dummy order below).
        constexpr const char* kDummyMeshes[3] = {
            "Assets/Models/MMOFPS/characters/char_striker.obj",
            "Assets/Models/MMOFPS/characters/char_bulwark.obj",
            "Assets/Models/MMOFPS/characters/char_ghost.obj",
        };

        /// Closest approach between ray (o + t*d, t in [0, tMax]) and the
        /// vertical segment x = (px, py0..py1, pz). Returns true when within
        /// radius; outT is the ray parameter at closest approach.
        bool RayHitsVerticalCapsule(const float o[3], const float d[3], float px, float py0, float py1, float pz,
                                    float radius, float tMax, float& outT)
        {
            // Segment direction is +Y; solve the standard two-segment closest
            // point problem specialised for b = (0,1,0).
            const float rx = o[0] - px;
            const float ry = o[1] - py0;
            const float rz = o[2] - pz;
            const float segLen = py1 - py0;

            const float a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]; // ~1 (normalized)
            const float e = 1.0f;                                    // |b|^2 per unit
            const float b = d[1];                                    // dot(d, +Y)
            const float c = d[0] * rx + d[1] * ry + d[2] * rz;       // dot(d, r)
            const float f = ry;                                      // dot(+Y, r)

            const float denom = a * e - b * b;
            float t = 0.0f; // along the ray
            float s = 0.0f; // along the segment (meters up from py0)
            if (denom > 1.0e-6f)
                t = (b * f - c * e) / denom;
            t = std::clamp(t, 0.0f, tMax);
            s = std::clamp(f + b * t, 0.0f, segLen);
            // One re-clamp of t against the clamped s (Ericson RTCD 5.1.9).
            t = std::clamp(b * s - c, 0.0f, tMax);

            const float cx = rx + d[0] * t;
            const float cy = ry + d[1] * t - s;
            const float cz = rz + d[2] * t;
            outT = t;
            return cx * cx + cy * cy + cz * cz <= radius * radius;
        }

    } // namespace

    TFTargetRange::TFTargetRange() = default;
    TFTargetRange::~TFTargetRange()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFTargetRange::Initialize(TFGameContext& ctx)
    {
        m_ctx = &ctx;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_range_debug"))
        {
            console.RegisterCommand(
                "tf_range_debug",
                [this](const std::vector<std::string>&) -> std::string
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                                  "[TF] target range: spawned=%d entities=%u hooked=%d shots=%u hits=%u floaters=%zu",
                                  m_spawned ? 1 : 0, SpawnedCount(), m_hooked ? 1 : 0, m_shots, m_hits,
                                  m_floaters.size());
                    return std::string(buf);
                },
                "Sanctuary firing-range status: spawn/hook state, local shot/hit counters", "TERRAFRONT",
                "tf_range_debug");
            m_debugCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFTargetRange initialized (cosmetic-only sanctuary range)");
        return true;
    }

    void TFTargetRange::Shutdown()
    {
        if (!m_initialized)
            return;

        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_range_debug");
            m_debugCmd = false;
        }

        // Release the weapon-system seam BEFORE dropping our state: the hook
        // lambda captures `this`. The TFWeaponSystem object outlives its own
        // Shutdown (module members are destroyed after all Shutdowns), so
        // clearing through the context pointer is safe in every teardown order.
        if (m_hooked && m_ctx && m_ctx->weapons)
            m_ctx->weapons->SetLocalShotHook({});
        m_hooked = false;

        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (world)
        {
            for (const uint32_t ent : m_entities)
            {
                const auto e = static_cast<EntityID>(ent);
                if (ent != 0u && world->GetRegistry().valid(e))
                    world->DestroyEntity(e);
            }
        }
        m_entities.clear();
        m_dummies.clear();
        m_floaters.clear();
        m_spawned = false;
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Spawn + hook
    // ---------------------------------------------------------------------------

    void TFTargetRange::SpawnAll()
    {
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->world)
            return;

        const auto spawn = [&](const char* model, const char* material, float x, float z, float yawDeg, float emissive,
                               float yOff = 0.0f) -> uint32_t
        {
            const float y = m_ctx->world->TerrainHeightAt(x, z) + yOff;
            const auto ent = world->CreateEntity("TF_RangeDecor");
            Transform& tr = world->AddComponent<Transform>(ent);
            tr.position = {x, y, z};
            tr.rotation.y = yawDeg; // DEGREES
            MeshRenderer& mr = world->AddComponent<MeshRenderer>(ent);
            mr.meshPath = model;
            mr.materialPath = material;
            mr.castShadows = true;
            mr.emissive = emissive;
            m_entities.push_back(static_cast<uint32_t>(ent));
            return static_cast<uint32_t>(ent);
        };

        // Dummies: two lanes x 20/40/60 m, facing the firing line (yaw 180).
        m_dummies.clear();
        m_dummies.reserve(6);
        for (int lane = 0; lane < 2; ++lane)
        {
            const float x = (lane == 0) ? kLaneAX : kLaneBX;
            for (int r = 0; r < 3; ++r)
            {
                Dummy d;
                d.x = x;
                d.z = kLineZ + kRanges[r];
                d.rangeM = kRanges[r];
                d.y = m_ctx->world->TerrainHeightAt(d.x, d.z);
                d.body = spawn(kDummyMeshes[r], kMatGray, d.x, d.z, 180.0f, 0.0f);
                // Per-dummy holo sign (readout anchor), 3 m east of the dummy.
                spawn("Assets/Models/MMOFPS/buildings/holo_sign_post.obj", kMatSanct, d.x + 3.0f, d.z, 180.0f, 0.8f);
                m_dummies.push_back(d);
            }
        }

        // Firing line: crates marking the lanes + one info sign mid-line.
        spawn("Assets/Models/MMOFPS/props/prop_crate_m.obj", kMatGray, kLaneAX, kLineZ - 2.0f, 10.0f, 0.0f);
        spawn("Assets/Models/MMOFPS/props/prop_crate_m.obj", kMatGray, kLaneBX, kLineZ - 2.0f, -15.0f, 0.0f);
        spawn("Assets/Models/MMOFPS/buildings/holo_sign_post.obj", kMatSanct, kRangeCenterX, kLineZ - 4.0f, 0.0f, 0.8f);

        m_spawned = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] target range: stamped %zu entities (6 dummies, cosmetic-only)",
                       m_entities.size());
    }

    void TFTargetRange::InstallHook()
    {
        if (m_hooked || !m_ctx->weapons)
            return;
        m_ctx->weapons->SetLocalShotHook([this](const float origin[3], const float dir[3], const WeaponDef& def)
                                         { OnLocalShot(origin, dir, def); });
        m_hooked = true;
    }

    // ---------------------------------------------------------------------------
    // Local shot -> cosmetic hit
    // ---------------------------------------------------------------------------

    float TFTargetRange::EstimateDamage(const WeaponDef& def, float distM)
    {
        // Client-side ESTIMATE of one trigger pull: weapons.json damage times
        // pellet count with linear falloff, mirroring the server's per-pellet
        // shape without lag comp, hit zones or occlusion. Cosmetic only.
        const float pellets = static_cast<float>(std::max(1, def.pellets));
        const float base = def.damage * pellets;
        const float floor = def.minDamage * pellets;
        if (distM <= def.falloffStartM || def.falloffEndM <= def.falloffStartM)
            return base;
        if (distM >= def.falloffEndM)
            return floor;
        const float k = (distM - def.falloffStartM) / (def.falloffEndM - def.falloffStartM);
        return base + (floor - base) * k;
    }

    void TFTargetRange::OnLocalShot(const float origin[3], const float dir[3], const WeaponDef& def)
    {
        // Only meaningful when firing FROM the sanctuary (the range is a safe-
        // zone training feature; continent shots never scan the dummy list).
        if (!m_spawned || !TFTravel_IsInSanctuary(origin[0], origin[2]))
            return;
        ++m_shots;

        // Nearest dummy along the ray (dummies never overlap on the ray in
        // this layout, but keep it correct anyway).
        Dummy* best = nullptr;
        float bestT = kMaxHitDistM;
        for (Dummy& d : m_dummies)
        {
            float t = 0.0f;
            if (RayHitsVerticalCapsule(origin, dir, d.x, d.y + kDummyFootY, d.y + kDummyHeadY, d.z, kDummyRadiusM,
                                       kMaxHitDistM, t) &&
                t < bestT)
            {
                best = &d;
                bestT = t;
            }
        }
        if (!best)
            return;
        ++m_hits;

        const float dmg = EstimateDamage(def, bestT);
        best->flash = 1.0f;
        best->lastDmg = dmg;
        if (m_clock - best->lastHitAt > kDpsIdleResetSec)
        {
            best->windowDmg = 0.0f;
            best->windowStart = m_clock;
        }
        best->windowDmg += dmg;
        best->lastHitAt = m_clock;

        Floater fl;
        fl.pos[0] = origin[0] + dir[0] * bestT;
        fl.pos[1] = origin[1] + dir[1] * bestT + 0.35f;
        fl.pos[2] = origin[2] + dir[2] * bestT;
        fl.amount = dmg;
        m_floaters.push_back(fl);
    }

    // ---------------------------------------------------------------------------
    // Frame driving
    // ---------------------------------------------------------------------------

    void TFTargetRange::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        m_clock += deltaTime;

        if (!m_spawned)
            SpawnAll();
        InstallHook(); // late-poll: ctx.weapons publishes after our Initialize

        if (!m_spawned)
            return;

        // Hit-flash decay -> dummy emissive (presentation only).
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        for (Dummy& d : m_dummies)
        {
            if (d.flash <= 0.0f)
                continue;
            d.flash = std::max(0.0f, d.flash - kFlashDecayPerSec * deltaTime);
            if (world && d.body != 0u && world->GetRegistry().valid(static_cast<EntityID>(d.body)))
            {
                if (MeshRenderer* mr = world->GetComponent<MeshRenderer>(static_cast<EntityID>(d.body)))
                    mr->emissive = d.flash * kFlashEmissive;
            }
        }

        // Floating damage numbers age out.
        for (Floater& f : m_floaters)
            f.age += deltaTime;
        std::erase_if(m_floaters, [](const Floater& f) { return f.age >= kFloaterLifeSec; });
    }

    // ---------------------------------------------------------------------------
    // Readouts (world-anchored ImGui foreground text)
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFTargetRange::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_spawned)
            return;
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        // First-person camera pose (module-owned camera, TFWorldSetup).
        const SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return;
        PawnInfo pawn{};
        if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) || !pawn.alive)
            return;

        // Perf/relevance gate: no readout work unless standing near the range.
        {
            const float dx = pawn.pos[0] - kRangeCenterX;
            const float dz = pawn.pos[2] - (kLineZ + 30.0f);
            if (dx * dx + dz * dz > kDrawWithinM * kDrawWithinM)
                return;
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float w = vp->Size.x;
        const float h = vp->Size.y;
        if (w <= 0.0f || h <= 0.0f)
            return;

        // Camera basis (LH, world-up) matching TFWorldSetup::ComputeViewProj's
        // LookAtLH(eye, eye+fwd, +Y); plain float math so this file needs no
        // DirectXMath. Cosmetic-only: small FOV drift just offsets labels.
        const auto cp = cam->GetPosition();
        const auto cf = cam->GetForward();
        float fwd[3] = {cf.x, cf.y, cf.z};
        const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl < 1.0e-4f)
            return;
        fwd[0] /= fl;
        fwd[1] /= fl;
        fwd[2] /= fl;
        // right = normalize(cross(up, fwd)), up' = cross(fwd, right)  (LH)
        float right[3] = {fwd[2], 0.0f, -fwd[0]};
        const float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
        if (rl < 1.0e-4f)
            return; // looking straight up/down: skip this frame
        right[0] /= rl;
        right[2] /= rl;
        const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2],
                             fwd[0] * right[1] - fwd[1] * right[0]};

        const float tanHalfY = std::tan(kFovY * 0.5f);
        const float aspect = w / h;

        const auto project = [&](const float p[3], ImVec2& out) -> bool
        {
            const float vx = p[0] - cp.x;
            const float vy = p[1] - cp.y;
            const float vz = p[2] - cp.z;
            const float cxv = vx * right[0] + vy * right[1] + vz * right[2];
            const float cyv = vx * up[0] + vy * up[1] + vz * up[2];
            const float czv = vx * fwd[0] + vy * fwd[1] + vz * fwd[2];
            if (czv <= kNearZ)
                return false;
            const float ndcX = cxv / (czv * tanHalfY * aspect);
            const float ndcY = cyv / (czv * tanHalfY);
            out.x = vp->Pos.x + (ndcX * 0.5f + 0.5f) * w;
            out.y = vp->Pos.y + (0.5f - ndcY * 0.5f) * h;
            return true;
        };

        // Range banner over the firing-line info sign.
        {
            const float bannerPos[3] = {
                kRangeCenterX, m_ctx->world->TerrainHeightAt(kRangeCenterX, kLineZ - 4.0f) + 3.6f, kLineZ - 4.0f};
            ImVec2 at;
            if (project(bannerPos, at))
            {
                TFUi::AddTextCentered(fg, 16.0f, at, IM_COL32(140, 220, 255, 210), "TARGET RANGE");
                TFUi::AddTextCentered(fg, 12.0f, ImVec2(at.x, at.y + 16.0f), IM_COL32(150, 170, 190, 170),
                                      "training dummies - local practice only");
            }
        }

        // Per-dummy sign readout: last hit + rolling DPS, lingers a few
        // seconds after the last hit.
        char buf[64];
        for (const Dummy& d : m_dummies)
        {
            if (m_clock - d.lastHitAt > kDpsShowForSec)
                continue;
            const double window = std::max(kDpsMinWindowSec, d.lastHitAt - d.windowStart);
            const float dps = static_cast<float>(d.windowDmg / window);
            const float signPos[3] = {d.x + 3.0f, d.y + 3.0f, d.z};
            ImVec2 at;
            if (!project(signPos, at))
                continue;
            std::snprintf(buf, sizeof(buf), "%.0fm  HIT %.0f", d.rangeM, d.lastDmg);
            TFUi::AddTextCentered(fg, 14.0f, at, IM_COL32(160, 230, 255, 225), buf);
            std::snprintf(buf, sizeof(buf), "DPS %.0f", dps);
            TFUi::AddTextCentered(fg, 15.0f, ImVec2(at.x, at.y + 16.0f), IM_COL32(255, 210, 120, 230), buf);
        }

        // Floating damage numbers: rise + fade at the impact point.
        for (const Floater& f : m_floaters)
        {
            const float k = f.age / kFloaterLifeSec;
            const float p[3] = {f.pos[0], f.pos[1] + f.age * kFloaterRiseMps, f.pos[2]};
            ImVec2 at;
            if (!project(p, at))
                continue;
            const int a = static_cast<int>(230.0f * (1.0f - k));
            std::snprintf(buf, sizeof(buf), "%.0f", f.amount);
            TFUi::AddTextCentered(fg, 19.0f, at, IM_COL32(255, 235, 160, a), buf);
        }
    }

#else // !SPARK_HAS_IMGUI

    void TFTargetRange::RenderUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
