/**
 * @file TFViewModelRender.cpp
 * @brief TFViewModel per-frame Render pass: animation springs (recoil recovery,
 *        turn sway, walk bob, sprint lower, jump/land kicks), the charm /
 *        sling-strap secondary-motion chains, and the weapon / arms / muzzle-
 *        flash draws. All geometry is procedural boxes except the weapon
 *        itself, which reuses the real weapons.json OBJ recentered/scaled by
 *        the data-driven ViewmodelDef — byte-compatible with the constants the
 *        old static pass-3 draw in TFWorldSetup::RenderWorld used. Split from
 *        TFViewModel.cpp; the shared feel-tuning constants and the charm/strap
 *        dressing helpers live in TFViewModelInternal.h.
 */
#include "Game/TFViewModel.h"

#include "Core/Platform.h"
#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "Game/TFOpticsSystem.h" // W11 weapon-optics lane: ADS sights
#include "Game/TFPlayerSystem.h"
#include "Game/TFSecondaryMotion.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFViewModelInternal.h"
#include "Game/TFWeaponSystem.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace Terrafront
{

    using namespace ViewModelDetail;

    namespace
    {

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

        /// P0 muzzle-flash sprite sheet for the active weapon: the weapon OBJ
        /// key prefix picks the family sheet (mra_/auc_/hlx_; np_/tool_ use the
        /// common sheet), with the local faction as fallback for legacy models.
        std::string MuzzleSheetForWeapon(const std::string& modelPath, FactionId faction)
        {
            const char* tag = nullptr;
            if (modelPath.find("/mra_") != std::string::npos)
                tag = "mra";
            else if (modelPath.find("/auc_") != std::string::npos)
                tag = "auc";
            else if (modelPath.find("/hlx_") != std::string::npos)
                tag = "hlx";
            else if (modelPath.find("/np_") != std::string::npos || modelPath.find("/tool_") != std::string::npos)
                tag = "common";
            else if (faction == FactionId::MRA)
                tag = "mra";
            else if (faction == FactionId::AUC)
                tag = "auc";
            else if (faction == FactionId::HLX)
                tag = "hlx";
            else
                tag = "common";
            return std::string("Assets/Textures/MMOFPS/fx/muzzle_flash_") + tag + ".png";
        }

    } // namespace

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

        // ------------------------------------------------------------ optics
        // W11 weapon-optics lane: ADS sight state (blend / B-key cycle / sway
        // clocks). This call site already guarantees the gates the optics need
        // (gfx present, local pawn alive, unseated, weapon in hand). FOV zoom +
        // hold-breath sway are consumed by TFWorldSetup::ComputeViewProj, the
        // reticle overlays draw later in the OnImGui phase (TFNameplates hook);
        // here we only need to know whether the 4x scope hides the viewmodel.
        TFOpticsSystem& optics = TFOpticsSystem::Get();
        optics.Update(ctx, dt);
        const bool hideForScope = optics.HideViewModel();

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
        // Fully scoped with the 4x optic: the weapon/arms/charm/flash draws are
        // skipped (scope overlay owns the screen) but every spring/chain above
        // kept simulating, so dropping out of ADS never pops stale motion.
        if (Mesh* vm = GetOrLoadWeaponMesh(gfx, vmPath);
            !hideForScope && vm && vm->GetVertexCount() > 0 && vm->GetIndexCount() > 0)
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
        if (m_cube && m_cube->GetIndexCount() > 0 && !hideForScope)
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
        // 4-frame flipbook: a camera-facing quad at the per-slot muzzle point of
        // the animated grip frame (so it tracks the kicking weapon), running the
        // P0 faction sprite strip via UV offset x = 0/0.25/0.5/0.75 over the
        // flash lifetime. Additive + fully emissive so it reads as a burst of
        // light. Skipped for melee. The world-space SpawnMuzzleFx flash stays
        // eye-anchored for other players.
        if (m_clock < m_flashUntil && slot != "melee" && !hideForScope)
        {
            if (!m_quad)
            {
                m_quad = std::make_unique<Mesh>();
                m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
                m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
            }
            if (m_quad && m_quad->GetIndexCount() > 0)
            {
                // Flipbook frame from normalized flash age (0..1 over kFlashLifeSec).
                const float age =
                    std::clamp(1.0f - static_cast<float>((m_flashUntil - m_clock) / kFlashLifeSec), 0.0f, 1.0f);
                const int frame = std::min(kFlashFrames - 1, static_cast<int>(age * kFlashFrames));

                // Billboard basis from inv(view) rows (camera axes in world space):
                // plane local X -> camera right, local Z -> camera up (v=0 edge is
                // +Z, so the sheet's top stays screen-up), local Y (the plane's
                // front face) -> toward the camera. Determinant stays +1, so the
                // CreatePlane winding remains front-facing under back-face cull.
                const XMVECTOR muzzleW =
                    XMVector3TransformCoord(XMVectorSet(0.0f, 0.02f, pose.muzzleForwardM, 1.0f), gripFrame);
                XMMATRIX bb = XMMatrixIdentity();
                bb.r[0] = XMVectorScale(XMVector3Normalize(invView.r[0]), kFlashQuadSizeM);
                bb.r[1] = XMVectorNegate(XMVector3Normalize(invView.r[2]));
                bb.r[2] = XMVectorScale(XMVector3Normalize(invView.r[1]), kFlashQuadSizeM);
                bb.r[3] = XMVectorSetW(muzzleW, 1.0f);

                const MuzzleFxDef& fx = pres.muzzleFx;
                gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
                // Depth READ-ONLY (W9): the flash is a burst of light, not
                // geometry — it must never write depth over the view model.
                gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);
                gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(MuzzleSheetForWeapon(vmPath, pawn.faction)));
                gfx->UpdateBasicConstants(
                    bb, view, proj, XMFLOAT4(fx.flashColor[0], fx.flashColor[1], fx.flashColor[2], fx.flashColor[3]),
                    {1.0f / kFlashFrames, 1.0f}, /*emissive*/ 1.0f, /*alpha*/ 1.0f,
                    XMFLOAT2(static_cast<float>(frame) / kFlashFrames, 0.0f));
                m_quad->Render(dc);
                gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
                gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
            }
        }

        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
