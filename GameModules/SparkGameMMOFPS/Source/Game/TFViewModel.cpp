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

        // Recoil recovery.
        SpringTo(m_recoilPitch.pos, m_recoilPitch.vel, 0.0f, kRecoilPitchSpringK, dt);
        SpringTo(m_recoilYaw.pos, m_recoilYaw.vel, 0.0f, kRecoilPitchSpringK, dt);
        SpringTo(m_recoilBack.pos, m_recoilBack.vel, 0.0f, kRecoilBackSpringK, dt);

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

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr); // everything below is untextured solid color

        // ------------------------------------------------------------ weapon
        if (Mesh* vm = GetOrLoadWeaponMesh(gfx, vmPath); vm && vm->GetVertexCount() > 0 && vm->GetIndexCount() > 0)
        {
            // Recenter/scale/orient exactly as the old pass-3 draw, then follow the
            // animated grip frame. Whole model in flat gunmetal: the weapon OBJs'
            // own MTL textures are near-black and read as silhouettes.
            const XMMATRIX weaponWorld = XMMatrixTranslation(vmDef.recenter[0], vmDef.recenter[1], vmDef.recenter[2]) *
                                         XMMatrixScaling(vmDef.scale, vmDef.scale, vmDef.scale) *
                                         XMMatrixRotationY(vmDef.rotationYRad) * gripFrame;
            const XMFLOAT4 gunmetal{vmDef.gunmetal[0], vmDef.gunmetal[1], vmDef.gunmetal[2], vmDef.gunmetal[3]};
            gfx->UpdateBasicConstants(weaponWorld, view, proj, gunmetal, {1.0f, 1.0f});
            vm->Render(dc);
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
        }

        // ------------------------------------------------------------ muzzle flash
        // Attached to the animated grip frame so it tracks the kicking weapon (the
        // world-space SpawnMuzzleFx flash stays eye-anchored). Skipped for melee.
        if (m_clock < m_flashUntil && slot != "melee" && m_cube && m_cube->GetIndexCount() > 0)
        {
            const XMMATRIX flashWorld = XMMatrixScaling(kFlashScaleM[0], kFlashScaleM[1], kFlashScaleM[2]) *
                                        XMMatrixTranslation(0.0f, 0.02f, pose.muzzleForwardM) * gripFrame;
            const MuzzleFxDef& fx = pres.muzzleFx;
            gfx->UpdateBasicConstants(flashWorld, view, proj,
                                      XMFLOAT4(fx.flashColor[0], fx.flashColor[1], fx.flashColor[2], fx.flashColor[3]),
                                      {1.0f, 1.0f});
            m_cube->Render(dc);
        }

        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
