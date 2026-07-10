/**
 * @file TFViewModel.cpp
 * @brief First-person arms + weapon viewmodel (see TFViewModel.h for the
 *        wiring contract). All geometry is procedural boxes except the weapon
 *        itself, which reuses the real weapons.json OBJ recentered/scaled by
 *        the data-driven ViewmodelDef — byte-compatible with the constants the
 *        old static pass-3 draw in TFWorldSetup::RenderWorld used.
 */
#include "Game/TFViewModel.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponSystem.h"

#include "Game/PlaceholderMesh.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Terrafront
{

    namespace
    {

        // ---------------------------------------------------------------- feel tuning
        // All amplitudes deliberately small — the viewmodel should breathe, not swim.

        constexpr float kMaxFrameDtSec = 0.05f; ///< clamp render dt (alt-tab / hitches)

        // Recoil kick per weapons.json recoil unit (Cyclone-9 recoilVert 0.55 ->
        // ~0.9 deg pitch + ~9 mm slide per shot; Longtooth 2.4 -> capped heavy thump).
        constexpr float kRecoilPitchPerVert = 0.028f; ///< rad muzzle-up per recoilVert
        constexpr float kRecoilYawPerHoriz = 0.012f;  ///< rad, random sign per shot
        constexpr float kRecoilBackPerVert = 0.016f;  ///< m slide toward the eye
        constexpr float kRecoilMinVert = 0.20f;       ///< floor so 0-recoil defs still tick
        constexpr float kRecoilPitchCapRad = 0.20f;
        constexpr float kRecoilYawCapRad = 0.10f;
        constexpr float kRecoilBackCapM = 0.085f;
        constexpr float kRecoilPitchSpringK = 260.0f; ///< 1/s^2 (critically damped)
        constexpr float kRecoilBackSpringK = 340.0f;
        constexpr float kDryFireDipRad = 0.014f; ///< empty-mag click-twitch (muzzle dip)

        // View-turn lag (weapon trails the camera slightly).
        constexpr float kSwayPerYawRate = 0.010f; ///< m of lateral lag per rad/s of turn
        constexpr float kSwayPerPitchRate = 0.008f;
        constexpr float kSwayMaxM = 0.045f;
        constexpr float kSwaySpringK = 90.0f;

        // Idle breathing.
        constexpr float kIdleAmpM = 0.0035f;
        constexpr float kIdleHz = 0.45f;

        // Walk bob, driven by predicted horizontal speed.
        constexpr float kBobPhasePerMeter = 4.4f; ///< rad of stride phase per meter walked
        constexpr float kBobVertAmpM = 0.012f;
        constexpr float kBobLatAmpM = 0.009f;
        constexpr float kBobRefSpeed = 5.2f; ///< amplitude reference (base run speed)

        // Sprint lower (blend 0..1 spring; applied as drop + tilt-down).
        constexpr float kSprintLowerM = 0.10f;
        constexpr float kSprintLowerPitchRad = 0.30f;
        constexpr float kLowerSpringK = 55.0f;

        // Secondary motion: dangling charm + sling-strap hint (TFSecondaryMotion
        // world-space chains anchored through the animated grip frame) plus a
        // transient barrel flex. Amplitudes deliberately small — jewelry, not rope.
        constexpr float kBarrelFlexPerVert = 0.010f; ///< rad extra muzzle flex per recoilVert
        constexpr float kBarrelFlexCapRad = 0.035f;
        constexpr float kBarrelFlexSpringK = 700.0f; ///< snappier than the recoil spring
        constexpr float kCharmGravityMps2 = 6.5f;    ///< sub-g: lazier, more readable swing
        constexpr float kCharmDampingPerSec = 2.8f;
        constexpr float kStrapLinkLenM = 0.05f;
        constexpr int kStrapLinks = 3;
        constexpr float kStrapDampingPerSec = 4.5f;                 ///< webbing swings heavier than chain
        constexpr float kStrapAnchor[3] = {-0.01f, -0.06f, -0.03f}; ///< grip space, under stock
        constexpr float kCharmFireBackMps = 0.55f;                  ///< charm impulse per recoilVert (camera-back)
        constexpr float kCharmFireUpMps = 0.30f;                    ///< ... and camera-up
        constexpr float kCharmFireKickCap = 3.0f;                   ///< accumulated recoilVert cap per frame
        constexpr float kStrapImpulseScale = 0.5f;                  ///< strap reacts softer than the charm
        constexpr float kJumpVelDeltaMps = 3.0f;                    ///< +vel step that reads as a jump
        constexpr float kJumpMinUpMps = 1.5f;
        constexpr float kLandMinFallMps = 3.0f;   ///< must fall faster than this to "land"
        constexpr float kLandRefFallMps = 9.0f;   ///< fall speed mapping to full land kick
        constexpr float kLandSwayDipMps = 0.055f; ///< m/s kick into the vertical sway spring
        constexpr float kJumpSwayRiseMps = 0.030f;
        constexpr float kCharmJumpKickMps = 0.35f; ///< world-down charm impulse on jump
        constexpr float kCharmLandKickMps = 0.80f; ///< world-down charm impulse, full impact
        constexpr float kCharmLinkThickM = 0.006f;
        constexpr float kStrapLinkThickM = 0.013f;
        constexpr float kCharmLinkColor[4] = {0.36f, 0.37f, 0.41f, 1.0f}; ///< chain metal
        constexpr float kStrapColor[4] = {0.11f, 0.11f, 0.13f, 1.0f};     ///< dark webbing
        constexpr float kCharmFobBase[3] = {0.45f, 0.45f, 0.48f};
        constexpr float kCharmFobTintK = 0.75f; ///< faction color strength on the fob

        // Muzzle flash quad (first-person; the world-space flash/tracer from
        // TFWorldSetup::SpawnMuzzleFx stays the source of truth for other players).
        constexpr float kFlashLifeSec = 0.045f;
        constexpr float kFlashScaleM[3] = {0.13f, 0.13f, 0.20f};

        // Arm palette. Sleeves take the faction hue (same moderated-lerp scheme as the
        // ECS pawn tint) so your own arms match your team color; gloves stay dark.
        constexpr float kSleeveGrey[3] = {0.34f, 0.35f, 0.38f};
        constexpr float kSleeveTintK = 0.55f;
        constexpr float kGloveColor[4] = {0.15f, 0.15f, 0.17f, 1.0f};

        // Arm joints, view-space meters RELATIVE to the weapon anchor (ViewmodelDef
        // place, i.e. the right-hand grip). Shoulders sit off the bottom corners of
        // the screen so the arms read as coming from the player's body.
        constexpr float kRightShoulder[3] = {0.26f, -0.42f, -0.48f};
        constexpr float kRightElbow[3] = {0.17f, -0.24f, -0.30f};
        constexpr float kRightHand[3] = {0.015f, -0.02f, -0.05f};
        constexpr float kLeftShoulder[3] = {-0.28f, -0.44f, -0.44f};
        constexpr float kLeftElbow[3] = {-0.20f, -0.26f, -0.22f};
        constexpr float kUpperArmThickM = 0.075f;
        constexpr float kForearmThickM = 0.062f;
        constexpr float kHandScaleM[3] = {0.075f, 0.055f, 0.10f};

        constexpr float kTwoPi = 6.28318531f;

        /// Advance a critically-damped spring toward `target`.
        void SpringTo(float& pos, float& vel, float target, float k, float dt)
        {
            const float damping = 2.0f * std::sqrt(k);
            vel += (-k * (pos - target) - damping * vel) * dt;
            pos += vel * dt;
        }

        float WrapPi(float a)
        {
            while (a > 3.14159265f)
                a -= kTwoPi;
            while (a < -3.14159265f)
                a += kTwoPi;
            return a;
        }

        /// World matrix for a unit cube stretched joint-to-joint (same right/up/
        /// forward basis construction the TFWorldSetup shot-FX tracer uses).
        DirectX::XMMATRIX BoxBetween(const float a[3], const float b[3], float thickM)
        {
            using namespace DirectX;
            XMVECTOR va = XMVectorSet(a[0], a[1], a[2], 0.0f);
            XMVECTOR vb = XMVectorSet(b[0], b[1], b[2], 0.0f);
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

        double NowRealSec()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

        /// Per-slot charm dressing: where the trinket hangs off the weapon (grip
        /// space, right side so it stays clear of the support hand), chain length,
        /// and fob proportions. Weapon slot picks the silhouette, faction picks
        /// the fob color — both from existing defs, no new data files.
        struct CharmStyle
        {
            float anchor[3];
            int links;
            float linkLenM;
            float fobScale[3];
        };

        CharmStyle CharmStyleForSlot(const std::string& slot)
        {
            // default (rifle/carbine/lmg/shotgun): dogtag on a 3-link chain
            CharmStyle s{{0.035f, -0.045f, 0.16f}, 3, 0.026f, {0.020f, 0.030f, 0.006f}};
            if (slot == "pistol")
            {
                s = CharmStyle{{0.022f, -0.035f, 0.05f}, 2, 0.022f, {0.016f, 0.016f, 0.016f}}; // compact cube fob
            }
            else if (slot == "sniper")
            {
                s = CharmStyle{{0.030f, -0.045f, 0.24f}, 3, 0.032f, {0.012f, 0.038f, 0.012f}}; // long scope tassel
            }
            else if (slot == "launcher")
            {
                s = CharmStyle{{0.035f, -0.060f, 0.12f}, 2, 0.030f, {0.026f, 0.020f, 0.014f}}; // stubby tag
            }
            else if (slot == "melee" || slot == "tool")
            {
                s = CharmStyle{{0.0f, -0.040f, 0.06f}, 2, 0.020f, {0.014f, 0.020f, 0.005f}}; // small lanyard
            }
            return s;
        }

        TFPendulumParams CharmParams(const CharmStyle& style)
        {
            TFPendulumParams p;
            p.linkCount = style.links;
            p.linkLengthM = style.linkLenM;
            p.gravityMps2 = kCharmGravityMps2;
            p.dampingPerSec = kCharmDampingPerSec;
            return p;
        }

        TFPendulumParams StrapParams()
        {
            TFPendulumParams p;
            p.linkCount = kStrapLinks;
            p.linkLengthM = kStrapLinkLenM;
            p.dampingPerSec = kStrapDampingPerSec;
            return p;
        }

    } // namespace

    // ---------------------------------------------------------------------------

    TFViewModel& TFViewModel::Get()
    {
        static TFViewModel s_instance;
        return s_instance;
    }

    void TFViewModel::NotifyLocalFire(float recoilVert, float recoilHoriz)
    {
        const float vert = std::max(recoilVert, kRecoilMinVert);
        m_recoilPitch.pos = std::min(m_recoilPitch.pos + vert * kRecoilPitchPerVert, kRecoilPitchCapRad);
        m_recoilBack.pos = std::min(m_recoilBack.pos + vert * kRecoilBackPerVert, kRecoilBackCapM);
        const float sign = (m_rng() & 1u) ? 1.0f : -1.0f;
        m_recoilYaw.pos =
            std::clamp(m_recoilYaw.pos + sign * recoilHoriz * kRecoilYawPerHoriz, -kRecoilYawCapRad, kRecoilYawCapRad);
        m_flashUntil = m_clock + kFlashLifeSec;

        // Secondary motion: transient barrel flex (weapon mesh only) + a queued
        // charm/strap impulse consumed by Render (needs the camera basis there).
        m_barrelFlex.pos = std::min(m_barrelFlex.pos + vert * kBarrelFlexPerVert, kBarrelFlexCapRad);
        m_pendingFireKick = std::min(m_pendingFireKick + vert, kCharmFireKickCap);
    }

    void TFViewModel::NotifyDryFire()
    {
        m_recoilPitch.pos -= kDryFireDipRad; // muzzle dip: reads as a dead-trigger click
    }

    void TFViewModel::Reset()
    {
        m_recoilPitch = SpringVal{};
        m_recoilYaw = SpringVal{};
        m_recoilBack = SpringVal{};
        m_swayX = SpringVal{};
        m_swayY = SpringVal{};
        m_lower = SpringVal{};
        m_bobPhase = 0.0f;
        m_hasPrevView = false;
        m_flashUntil = -1.0;

        // Secondary motion: drop the chains (rebuilt + snapped on next Render)
        // and clear the transient springs/impulses.
        TFSecondaryMotion& motion = TFSecondaryMotion::Get();
        motion.Detach(m_charmChain);
        motion.Detach(m_strapChain);
        m_charmChain = 0;
        m_strapChain = 0;
        m_chainSlot = "\n"; // impossible slot => rebuild on next Render
        m_barrelFlex = SpringVal{};
        m_pendingFireKick = 0.0f;
        m_hasPrevVel = false;
    }

    Mesh* TFViewModel::GetOrLoadWeaponMesh(GraphicsEngine* gfx, const std::string& assetPath)
    {
        if (assetPath.empty())
            return nullptr;
        if (auto it = m_weaponMeshCache.find(assetPath); it != m_weaponMeshCache.end())
            return it->second.get();

        auto mesh = std::make_unique<Mesh>();
        // Same tinyobjloader path TFWorldSetup uses for scene/ECS meshes (the
        // engine AssetPipeline OBJ loader is unreliable on Windows); falls back to
        // a unit cube when the OBJ is missing.
        LoadOrPlaceholderMesh(*mesh, gfx->GetDevice(), gfx->GetContext(),
                              std::wstring(assetPath.begin(), assetPath.end()));
        Mesh* raw = mesh.get();
        m_weaponMeshCache.emplace(assetPath, std::move(mesh));
        return raw;
    }

    const std::string& TFViewModel::ResolveSlotForModel(TFGameContext& ctx, const std::string& assetPath)
    {
        if (assetPath == m_cachedModelPath)
            return m_cachedSlot;
        m_cachedModelPath = assetPath;
        m_cachedSlot.clear();
        if (ctx.data && ctx.data->IsLoaded())
        {
            // assetPath is "Assets/" + WeaponDef::model (TFWeaponSystem::
            // ActiveWeaponModel convention) — match by suffix. Weapons sharing a
            // model OBJ share a silhouette family, so any match's slot is right.
            for (const WeaponDef& w : ctx.data->AllWeapons())
            {
                if (!w.model.empty() && assetPath.size() >= w.model.size() &&
                    assetPath.compare(assetPath.size() - w.model.size(), w.model.size(), w.model) == 0)
                {
                    m_cachedSlot = w.slot;
                    break;
                }
            }
        }
        return m_cachedSlot;
    }

    TFViewModel::GripPose TFViewModel::PoseForSlot(const std::string& slot)
    {
        GripPose pose{}; // default: rifle-family two-handed grip
        if (slot == "pistol")
        {
            pose.leftGrip[0] = -0.010f;
            pose.leftGrip[1] = -0.050f;
            pose.leftGrip[2] = 0.070f; // support hand cups under the grip
            pose.muzzleForwardM = 0.30f;
        }
        else if (slot == "sniper")
        {
            pose.leftGrip[2] = 0.26f; // long forestock
            pose.muzzleForwardM = 0.70f;
        }
        else if (slot == "launcher")
        {
            pose.leftGrip[0] = -0.020f;
            pose.leftGrip[1] = -0.060f;
            pose.leftGrip[2] = 0.15f; // underslung support
            pose.muzzleForwardM = 0.50f;
        }
        else if (slot == "melee" || slot == "tool")
        {
            pose.twoHanded = false; // free left hand
            pose.muzzleForwardM = 0.25f;
        }
        return pose;
    }

    // ---------------------------------------------------------------------------

    void TFViewModel::Render(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!gfx || !gfx->GetDevice() || !gfx->GetContext() || !ctx.HasLocalPlayer() || !ctx.players || !ctx.weapons)
            return;

        PawnInfo pawn{};
        if (!ctx.players->GetPawnByPlayer(ctx.localPlayer, pawn) || !pawn.alive ||
            (ctx.vehicles && ctx.vehicles->IsSeated(ctx.localPlayer)))
        {
            Reset(); // no stale kick/sway on respawn or vehicle exit
            return;
        }

        const std::string vmPath = ctx.weapons->ActiveWeaponModel();
        if (vmPath.empty())
        {
            Reset();
            return;
        }

        // ------------------------------------------------------------ time step
        const double nowReal = NowRealSec();
        float dt = (m_lastRealTime < 0.0) ? 0.0f : static_cast<float>(nowReal - m_lastRealTime);
        m_lastRealTime = nowReal;
        dt = std::clamp(dt, 0.0f, kMaxFrameDtSec);
        m_clock += dt;

        // ------------------------------------------------------------ springs
        // View-turn lag: the weapon trails camera rotation slightly.
        float yawRate = 0.0f, pitchRate = 0.0f;
        if (m_hasPrevView && dt > 1.0e-4f)
        {
            yawRate = WrapPi(pawn.yaw - m_prevYaw) / dt;
            pitchRate = (pawn.pitch - m_prevPitch) / dt;
        }
        m_prevYaw = pawn.yaw;
        m_prevPitch = pawn.pitch;
        m_hasPrevView = true;

        const float swayTargetX = std::clamp(-yawRate * kSwayPerYawRate, -kSwayMaxM, kSwayMaxM);
        const float swayTargetY = std::clamp(-pitchRate * kSwayPerPitchRate, -kSwayMaxM, kSwayMaxM);
        SpringTo(m_swayX.pos, m_swayX.vel, swayTargetX, kSwaySpringK, dt);
        SpringTo(m_swayY.pos, m_swayY.vel, swayTargetY, kSwaySpringK, dt);

        // Walk bob from predicted horizontal speed; sprint detection compares the
        // same class speeds TFClientNet predicts with (fallbacks match ClassDef).
        const float speed = std::sqrt(pawn.vel[0] * pawn.vel[0] + pawn.vel[2] * pawn.vel[2]);
        float runSpeed = 5.2f, sprintSpeed = 7.2f;
        if (ctx.data && ctx.data->IsLoaded())
        {
            if (const ClassDef* cd = ctx.data->GetClass(pawn.cls))
            {
                runSpeed = cd->runSpeed;
                sprintSpeed = cd->sprintSpeed;
            }
        }
        m_bobPhase += speed * kBobPhasePerMeter * dt;
        if (m_bobPhase > kTwoPi * 1024.0f)
            m_bobPhase -= kTwoPi * 1024.0f; // keep sin() precision over long sessions
        const float bobAmp = std::min(speed / kBobRefSpeed, 1.2f);
        const float bobX = std::sin(m_bobPhase) * kBobLatAmpM * bobAmp;
        const float bobY = -std::fabs(std::sin(m_bobPhase)) * kBobVertAmpM * bobAmp;

        const bool sprinting = speed > 0.5f * (runSpeed + sprintSpeed);
        SpringTo(m_lower.pos, m_lower.vel, sprinting ? 1.0f : 0.0f, kLowerSpringK, dt);

        // Jump / land detection from predicted vertical velocity: kick the
        // vertical sway spring (the whole viewmodel dips/rises) and queue a
        // world-space impulse for the charm/strap chains below.
        float verticalKickMps = 0.0f;
        {
            const float velY = pawn.vel[1];
            if (m_hasPrevVel && dt > 1.0e-4f)
            {
                if (m_prevVelY < -kLandMinFallMps && velY > m_prevVelY + kJumpVelDeltaMps)
                {
                    const float impact = std::min(-m_prevVelY / kLandRefFallMps, 1.5f);
                    m_swayY.vel -= kLandSwayDipMps * impact;
                    verticalKickMps = -kCharmLandKickMps * impact; // chains keep falling
                }
                else if (velY > m_prevVelY + kJumpVelDeltaMps && velY > kJumpMinUpMps)
                {
                    m_swayY.vel += kJumpSwayRiseMps;
                    verticalKickMps = -kCharmJumpKickMps; // chains lag the rising weapon
                }
            }
            m_prevVelY = velY;
            m_hasPrevVel = true;
        }

        // Recoil recovery.
        SpringTo(m_recoilPitch.pos, m_recoilPitch.vel, 0.0f, kRecoilPitchSpringK, dt);
        SpringTo(m_recoilYaw.pos, m_recoilYaw.vel, 0.0f, kRecoilPitchSpringK, dt);
        SpringTo(m_recoilBack.pos, m_recoilBack.vel, 0.0f, kRecoilBackSpringK, dt);
        SpringTo(m_barrelFlex.pos, m_barrelFlex.vel, 0.0f, kBarrelFlexSpringK, dt);

        const float idleX = std::sin(static_cast<float>(m_clock) * kTwoPi * kIdleHz) * kIdleAmpM;
        const float idleY = std::sin(static_cast<float>(m_clock) * kTwoPi * kIdleHz * 0.5f) * kIdleAmpM;

        // ------------------------------------------------------------ transforms
        // Presentation constants: same data-driven ViewmodelDef (with the same
        // default-constructed fallback) the old static pass-3 draw used.
        static const WorldPresentationDef s_defaultPres{};
        const WorldPresentationDef& pres =
            (ctx.data && ctx.data->IsLoaded()) ? ctx.data->GetPresentation() : s_defaultPres;
        const ViewmodelDef& vmDef = pres.viewmodel;

        const std::string& slot = ResolveSlotForModel(ctx, vmPath);
        const GripPose pose = PoseForSlot(slot);

        // Grip frame: recoil rotation about the grip, then place at the ViewmodelDef
        // anchor plus all translation offsets, then out of view space via inv(view).
        // view space: +x right, +y up, +z forward; XMMatrixRotationX(+a) pitches
        // the forward axis DOWN (row-vector convention), so muzzle-up is -a.
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const float offX = m_swayX.pos + bobX + idleX;
        const float offY = m_swayY.pos + bobY + idleY - m_lower.pos * kSprintLowerM;
        const float offZ = -m_recoilBack.pos;
        const XMMATRIX gripFrame =
            XMMatrixRotationX(-m_recoilPitch.pos + m_lower.pos * kSprintLowerPitchRad) *
            XMMatrixRotationY(m_recoilYaw.pos) *
            XMMatrixTranslation(vmDef.place[0] + offX, vmDef.place[1] + offY, vmDef.place[2] + offZ) * invView;

        // ---------------------------------------------------- secondary motion
        // Charm + strap chains simulate in WORLD space, anchored through the
        // grip frame — so turn sway, bob, sprint lower, recoil kick, jumping,
        // and camera translation all drive them with no extra plumbing.
        TFSecondaryMotion& motion = TFSecondaryMotion::Get();
        const CharmStyle charmStyle = CharmStyleForSlot(slot);
        if (m_chainSlot != slot)
        {
            motion.Detach(m_charmChain);
            motion.Detach(m_strapChain);
            m_charmChain = motion.AttachPendulum(CharmParams(charmStyle));
            m_strapChain = motion.AttachPendulum(StrapParams());
            m_chainSlot = slot;
        }
        TFPendulumChain* charm = motion.Find(m_charmChain);
        TFPendulumChain* strap = motion.Find(m_strapChain);

        // Punch impulses: fire recoil along the camera basis (inv(view) rows:
        // r[1] = up, r[2] = forward), jump/land straight down in world space.
        if (m_pendingFireKick > 0.0f || verticalKickMps != 0.0f)
        {
            XMVECTOR imp = XMVectorSet(0.0f, verticalKickMps, 0.0f, 0.0f);
            if (m_pendingFireKick > 0.0f)
            {
                imp = XMVectorAdd(imp, XMVectorScale(invView.r[2], -m_pendingFireKick * kCharmFireBackMps));
                imp = XMVectorAdd(imp, XMVectorScale(invView.r[1], m_pendingFireKick * kCharmFireUpMps));
                m_pendingFireKick = 0.0f;
            }
            const XMFLOAT3 impulse{XMVectorGetX(imp), XMVectorGetY(imp), XMVectorGetZ(imp)};
            if (charm)
                charm->AddImpulse(impulse);
            if (strap)
                strap->AddImpulse(XMFLOAT3{impulse.x * kStrapImpulseScale, impulse.y * kStrapImpulseScale,
                                           impulse.z * kStrapImpulseScale});
        }

        const auto gripToWorld = [&](const float local[3])
        {
            const XMVECTOR w = XMVector3TransformCoord(XMVectorSet(local[0], local[1], local[2], 1.0f), gripFrame);
            return XMFLOAT3{XMVectorGetX(w), XMVectorGetY(w), XMVectorGetZ(w)};
        };
        if (charm)
            charm->Update(gripToWorld(charmStyle.anchor), dt);
        if (strap)
            strap->Update(gripToWorld(kStrapAnchor), dt);

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr); // arms/charm/muzzle below are solid color

        // ------------------------------------------------------------ weapon
        if (Mesh* vm = GetOrLoadWeaponMesh(gfx, vmPath); vm && vm->GetVertexCount() > 0 && vm->GetIndexCount() > 0)
        {
            // Recenter/scale/orient exactly as the old pass-3 draw, then follow the
            // animated grip frame.
            // Barrel flex: a brief extra muzzle-up bend about the grip on recoil,
            // applied to the weapon mesh ONLY (the arms hold steady, so the gun
            // visibly flexes in the hands instead of the whole rig rotating).
            const XMMATRIX weaponWorld = XMMatrixTranslation(vmDef.recenter[0], vmDef.recenter[1], vmDef.recenter[2]) *
                                         XMMatrixScaling(vmDef.scale, vmDef.scale, vmDef.scale) *
                                         XMMatrixRotationY(vmDef.rotationYRad) * XMMatrixRotationX(-m_barrelFlex.pos) *
                                         gripFrame;
            const XMFLOAT4 gunmetal{vmDef.gunmetal[0], vmDef.gunmetal[1], vmDef.gunmetal[2], vmDef.gunmetal[3]};

            // Draw each MTL material range with its own map_Kd (the P0 weapon
            // atlas) at 1:1 UVs, exactly like TFWorldSetup's scene path; ranges
            // with no texture fall back to flat gunmetal so untextured weapons
            // still read as silhouettes.
            const auto& submeshes = vm->GetSubmeshes();
            if (submeshes.empty())
            {
                gfx->UpdateBasicConstants(weaponWorld, view, proj, gunmetal, {1.0f, 1.0f});
                gfx->SetBasicTexture(nullptr);
                vm->Render(dc);
            }
            else
            {
                for (const MeshSubmesh& smesh : submeshes)
                {
                    ID3D11ShaderResourceView* srv =
                        smesh.diffuseTexture.empty() ? nullptr : gfx->GetOrLoadTextureSRV(smesh.diffuseTexture);
                    const XMFLOAT4 tint = srv ? XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f} : gunmetal;
                    gfx->UpdateBasicConstants(weaponWorld, view, proj, tint, {1.0f, 1.0f});
                    gfx->SetBasicTexture(srv);
                    vm->RenderRange(dc, smesh.indexStart, smesh.indexCount);
                }
                gfx->SetBasicTexture(nullptr);
            }
        }

        // ------------------------------------------------------------ arms
        if (!m_cube)
        {
            m_cube = std::make_unique<Mesh>();
            m_cube->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_cube->CreateCube(1.0f);
        }
        if (m_cube && m_cube->GetIndexCount() > 0)
        {
            float fcol[4];
            FactionColor(pawn.faction, fcol);
            const XMFLOAT4 sleeve{kSleeveGrey[0] + kSleeveTintK * (fcol[0] - kSleeveGrey[0]),
                                  kSleeveGrey[1] + kSleeveTintK * (fcol[1] - kSleeveGrey[1]),
                                  kSleeveGrey[2] + kSleeveTintK * (fcol[2] - kSleeveGrey[2]), 1.0f};
            const XMFLOAT4 glove{kGloveColor[0], kGloveColor[1], kGloveColor[2], kGloveColor[3]};

            const auto drawBox = [&](const XMMATRIX& local, const XMFLOAT4& color)
            {
                gfx->UpdateBasicConstants(local * gripFrame, view, proj, color, {1.0f, 1.0f});
                m_cube->Render(dc);
            };
            const auto drawHand = [&](const float at[3])
            {
                drawBox(XMMatrixScaling(kHandScaleM[0], kHandScaleM[1], kHandScaleM[2]) *
                            XMMatrixTranslation(at[0], at[1], at[2]),
                        glove);
            };

            // Right arm: shoulder -> elbow -> grip.
            drawBox(BoxBetween(kRightShoulder, kRightElbow, kUpperArmThickM), sleeve);
            drawBox(BoxBetween(kRightElbow, kRightHand, kForearmThickM), sleeve);
            drawHand(kRightHand);

            // Left (support) arm: only for two-handed weapons.
            if (pose.twoHanded)
            {
                drawBox(BoxBetween(kLeftShoulder, kLeftElbow, kUpperArmThickM), sleeve);
                drawBox(BoxBetween(kLeftElbow, pose.leftGrip, kForearmThickM), sleeve);
                drawHand(pose.leftGrip);
            }

            // Charm + sling strap: the chains return WORLD matrices (their sim
            // already composed every viewmodel motion source via the anchors).
            const auto drawWorldBox = [&](const XMMATRIX& world, const XMFLOAT4& color)
            {
                gfx->UpdateBasicConstants(world, view, proj, color, {1.0f, 1.0f});
                m_cube->Render(dc);
            };
            if (charm && charm->Ready())
            {
                const XMFLOAT4 linkColor{kCharmLinkColor[0], kCharmLinkColor[1], kCharmLinkColor[2],
                                         kCharmLinkColor[3]};
                for (int i = 0; i < charm->LinkCount(); ++i)
                    drawWorldBox(charm->LinkWorld(i, kCharmLinkThickM), linkColor);

                // Fob tinted by faction — FactionDef secondary color when tables
                // are loaded (reads as unit insignia), FactionColor fallback.
                float base[4];
                FactionColor(pawn.faction, base);
                if (ctx.data && ctx.data->IsLoaded())
                {
                    if (const FactionDef* fd = ctx.data->GetFaction(pawn.faction))
                    {
                        base[0] = fd->colorSec[0];
                        base[1] = fd->colorSec[1];
                        base[2] = fd->colorSec[2];
                    }
                }
                const XMFLOAT4 fobColor{kCharmFobBase[0] + kCharmFobTintK * (base[0] - kCharmFobBase[0]),
                                        kCharmFobBase[1] + kCharmFobTintK * (base[1] - kCharmFobBase[1]),
                                        kCharmFobBase[2] + kCharmFobTintK * (base[2] - kCharmFobBase[2]), 1.0f};
                drawWorldBox(
                    charm->TipWorld(XMFLOAT3{charmStyle.fobScale[0], charmStyle.fobScale[1], charmStyle.fobScale[2]}),
                    fobColor);
            }
            if (strap && strap->Ready())
            {
                const XMFLOAT4 strapColor{kStrapColor[0], kStrapColor[1], kStrapColor[2], kStrapColor[3]};
                for (int i = 0; i < strap->LinkCount(); ++i)
                    drawWorldBox(strap->LinkWorld(i, kStrapLinkThickM), strapColor);
            }
        }

        // ------------------------------------------------------------ muzzle flash
        // Attached to the animated grip frame so it tracks the kicking weapon (the
        // world-space SpawnMuzzleFx flash stays eye-anchored). Skipped for melee.
        if (m_clock < m_flashUntil && slot != "melee" && m_cube && m_cube->GetIndexCount() > 0)
        {
            const XMMATRIX flashWorld = XMMatrixScaling(kFlashScaleM[0], kFlashScaleM[1], kFlashScaleM[2]) *
                                        XMMatrixTranslation(0.0f, 0.02f, pose.muzzleForwardM) * gripFrame;
            const MuzzleFxDef& fx = pres.muzzleFx;
            // Additive + fully emissive so the flash reads as a burst of light
            // that adds to the scene rather than an opaque colored box.
            gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
            gfx->UpdateBasicConstants(flashWorld, view, proj,
                                      XMFLOAT4(fx.flashColor[0], fx.flashColor[1], fx.flashColor[2], fx.flashColor[3]),
                                      {1.0f, 1.0f}, /*emissive*/ 1.0f);
            m_cube->Render(dc);
            gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        }

        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
