/**
 * @file TFViewModel.cpp
 * @brief First-person arms + weapon viewmodel (see TFViewModel.h for the
 *        wiring contract): singleton accessor, fire / dry-fire recoil intake,
 *        state reset, the weapon-mesh cache, and the per-slot grip / slot
 *        lookup. The per-frame Render pass lives in TFViewModelRender.cpp;
 *        the shared feel-tuning constants and the charm/strap dressing
 *        helpers live in TFViewModelInternal.h.
 */
#include "Game/TFViewModel.h"

#include "Data/TFDataTables.h"
#include "Game/TFOpticsSystem.h" // W11 weapon-optics lane: ADS sights
#include "Game/TFSecondaryMotion.h"
#include "Game/TFViewModelInternal.h"

#include "Game/PlaceholderMesh.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <algorithm>

namespace Terrafront
{

    using namespace ViewModelDetail;

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

        // W11 weapon-optics: death / vehicle entry / holster snap the zoom out.
        TFOpticsSystem::Get().NotifyInactive();
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
        // muzzleForwardM per slot, measured from the shipped P0 weapon OBJs
        // (pivot at grip, +Z muzzle; BATCH_REPORT_P0.md length table): family
        // average length minus the behind-grip receiver/stock run. Rifle 0.72,
        // sniper 1.04, pistol 0.21, launcher 0.60 are the report's own numbers;
        // carbine/lmg/shotgun/tool scale from their family lengths.
        GripPose pose{}; // default: rifle-family two-handed grip (0.72 m)
        if (slot == "carbine")
        {
            pose.muzzleForwardM = 0.57f; // avg 0.77 m family
        }
        else if (slot == "lmg")
        {
            pose.leftGrip[2] = 0.23f;    // long handguard ahead of the drum
            pose.muzzleForwardM = 0.82f; // avg 1.11 m family
        }
        else if (slot == "shotgun")
        {
            pose.muzzleForwardM = 0.75f; // Bulkhead 1.02 m
        }
        else if (slot == "pistol")
        {
            pose.leftGrip[0] = -0.010f;
            pose.leftGrip[1] = -0.050f;
            pose.leftGrip[2] = 0.070f; // support hand cups under the grip
            pose.muzzleForwardM = 0.21f;
        }
        else if (slot == "sniper")
        {
            pose.leftGrip[2] = 0.26f; // long forestock
            pose.muzzleForwardM = 1.04f;
        }
        else if (slot == "launcher")
        {
            pose.leftGrip[0] = -0.020f;
            pose.leftGrip[1] = -0.060f;
            pose.leftGrip[2] = 0.15f; // underslung support
            pose.muzzleForwardM = 0.60f;
        }
        else if (slot == "melee")
        {
            pose.twoHanded = false; // free left hand
            pose.muzzleForwardM = 0.25f;
        }
        else if (slot == "tool")
        {
            pose.twoHanded = false;      // free left hand
            pose.muzzleForwardM = 0.22f; // torch/applicator tip ~0.26-0.30 m OBJ
        }
        return pose;
    }

} // namespace Terrafront
