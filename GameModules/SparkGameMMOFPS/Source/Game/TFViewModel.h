/**
 * @file TFViewModel.h
 * @brief First-person viewmodel: stylized arms + the equipped weapon, drawn at
 *        the camera with idle sway, walk bob, sprint lower, and a spring-damped
 *        recoil kick + a 4-frame flipbook muzzle flash (faction sprite sheet,
 *        camera-facing additive quad) when the local player fires. Secondary
 *        motion (TFSecondaryMotion chains): a faction-tinted charm and a
 *        sling-strap hint dangle from the weapon and react to look/move/jump/
 *        land/fire, plus a transient barrel flex on recoil.
 *
 * OWNERSHIP: this header + TFViewModel.cpp belong to ONE implementation agent
 * (viewmodel lane). Pure client-side presentation — no net or server state.
 *
 * Wiring (two one-line call sites, see the lane wiring notes):
 *  - TFWorldSetup::RenderWorld calls Get().Render(...) where the old static
 *    pass-3 weapon draw lived (this class supersedes that block).
 *  - TFWeaponSystem::ClientTriggerFire calls Get().NotifyLocalFire(...) next to
 *    SpawnMuzzleFx, and Get().NotifyDryFire() in the empty-mag branch.
 *
 * Geometry is fully procedural (Mesh::CreateCube boxes oriented joint-to-joint)
 * — the asset pipeline is OBJ/PNG only, no rigged meshes. The weapon itself is
 * the real weapons.json OBJ, recentered/scaled by the data-driven ViewmodelDef
 * (presentation.json), so every weapon keeps its own silhouette. Known
 * limitation: drawn in the normal world pass with world depth, so the weapon
 * can clip into walls at point-blank range (engine has no overlay depth pass).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Game/TFSecondaryMotion.h" // charm / sling-strap pendulum chains

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <memory>
#include <random>
#include <string>
#include <unordered_map>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    class TFViewModel
    {
      public:
        /// Process-wide instance. Justified singleton: one local player per client
        /// process, pure presentation, and the two producers (TFWeaponSystem fire
        /// path, TFWorldSetup render path) are owned by other lanes — a static
        /// accessor keeps their wiring to a single line each with no TFGameContext
        /// (frozen header) or lifecycle (Main.cpp) changes.
        static TFViewModel& Get();

        /// Client fired the active weapon this frame (same call site as the muzzle
        /// FX). Kicks the viewmodel: pitch/yaw rotation + backward slide with
        /// spring-damper recovery, plus a brief first-person muzzle flash.
        /// recoilVert/recoilHoriz are the weapons.json per-shot recoil numbers.
        void NotifyLocalFire(float recoilVert, float recoilHoriz);

        /// Trigger pulled on an empty mag (client-visible dry-fire): light
        /// muzzle-dip twitch, no flash.
        void NotifyDryFire();

        /// Draw the arms + weapon for the local player. Call once per frame from
        /// TFWorldSetup::RenderWorld after the world/ECS passes (BeginFrame must be
        /// active; basic shaders are (re)bound internally). Hidden when headless,
        /// dead, pawnless, or seated in a vehicle. Advances all animation springs
        /// from an internal clock — no separate Update call needed.
        void Render(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                    const DirectX::XMMATRIX& proj);

      private:
        TFViewModel() = default;

        /// Critically-damped spring value (position + velocity).
        struct SpringVal
        {
            float pos = 0.0f;
            float vel = 0.0f;
        };

        /// Per-slot grip layout: where the support hand holds the weapon and how
        /// far forward the muzzle tip sits (all view-space meters relative to the
        /// weapon anchor / right-hand grip). muzzleForwardM is measured from the
        /// shipped P0 weapon OBJs (pivot at grip, +Z muzzle — see the length
        /// table in BATCH_REPORT_P0.md), per slot family.
        struct GripPose
        {
            bool twoHanded = true;
            float leftGrip[3] = {-0.015f, -0.025f, 0.20f};
            float muzzleForwardM = 0.72f; ///< rifle-family default (avg 0.97 m OBJ)
        };

        void Reset();
        Mesh* GetOrLoadWeaponMesh(GraphicsEngine* gfx, const std::string& assetPath);
        /// weapons.json slot string ("rifle"/"pistol"/"tool"/...) of the weapon
        /// whose model path matches the active viewmodel path; empty if unknown.
        const std::string& ResolveSlotForModel(TFGameContext& ctx, const std::string& assetPath);
        static GripPose PoseForSlot(const std::string& slot);

        // animation state (view-space unless noted)
        double m_lastRealTime = -1.0; ///< std::chrono seconds; -1 == first frame
        double m_clock = 0.0;         ///< accumulated render-clock seconds
        SpringVal m_recoilPitch;      ///< rad, + == muzzle up
        SpringVal m_recoilYaw;        ///< rad, signed per-shot
        SpringVal m_recoilBack;       ///< m, + == toward the eye
        SpringVal m_swayX, m_swayY;   ///< view-turn lag, m
        SpringVal m_lower;            ///< 0..1 sprint-lower blend
        float m_bobPhase = 0.0f;      ///< rad, advanced by horizontal speed
        float m_prevYaw = 0.0f, m_prevPitch = 0.0f;
        bool m_hasPrevView = false;
        double m_flashUntil = -1.0;      ///< render-clock time the muzzle flash ends
        std::minstd_rand m_rng{0x7EA5u}; ///< horizontal recoil sign/jitter

        // secondary motion (TFSecondaryMotion world-space chains + local springs)
        TFPendulumHandle m_charmChain = 0; ///< dangling weapon charm/trinket
        TFPendulumHandle m_strapChain = 0; ///< sling-strap hint under the stock
        std::string m_chainSlot = "\n";    ///< slot the chains were built for
                                           ///< (impossible sentinel forces first build)
        SpringVal m_barrelFlex;            ///< rad, transient muzzle flex on recoil
        float m_pendingFireKick = 0.0f;    ///< recoilVert queued for a charm impulse
        float m_prevVelY = 0.0f;           ///< pawn vertical velocity last frame
        bool m_hasPrevVel = false;         ///< m_prevVelY valid (jump/land detection)

        // GPU resources (lazy; client render thread only)
        std::unique_ptr<Mesh> m_cube; ///< unit cube reused for every arm segment
        std::unique_ptr<Mesh> m_quad; ///< unit plane billboarded for the muzzle-flash flipbook
        std::unordered_map<std::string, std::unique_ptr<Mesh>> m_weaponMeshCache;

        // slot lookup cache (avoid scanning AllWeapons() every frame)
        std::string m_cachedModelPath;
        std::string m_cachedSlot;
    };

} // namespace Terrafront
