/**
 * @file TFNameplates.h
 * @brief Over-pawn MMO nameplates (W10): character name + outfit tag +
 *        faction-colored health/shield bar projected over visible remote
 *        pawns, drawn on the ImGui foreground drawlist.
 *
 * OWNERSHIP: nameplates lane (this header + TFNameplates.cpp).
 *
 * No wire traffic — every input is data the client already mirrors:
 *  - pawn registry ... TFPlayerSystem::ForEachAlivePawn (replicated pos /
 *                      faction / class / alive / health / shield, all roles)
 *  - names ........... TFSocialSystem roster mirror (NameOfPlayer), falling
 *                      back to the TFScoreboard labels ("HOST" / "P<n>")
 *  - outfit tag ...... TFOutfitSystem::GetOutfitTag via OutfitTaggedLabel
 *  - max vitals ...... classes.json ClassDef health/shield (PawnView pattern)
 *  - veil ............ TFAbilitySystem::IsPawnVeiled — an ENEMY veiled Ghost
 *                      drops its plate exactly while UpdateVeilVisuals ghosts
 *                      its mesh (same faction test), so the plate can never
 *                      out a cloaked Ghost; friendly veiled Ghosts keep both.
 *
 * Visibility rules:
 *  - friendlies (local faction): always named, squadmates tinted green
 *    (TFHUD minimap convention).
 *  - enemies: named only while aimed at (~2 deg cone around the camera
 *    forward) OR damaged in the last kTFNameplateRevealSec seconds. "Damaged"
 *    is detected client-side from replicated health+shield drops (works on
 *    every role, no protocol change) plus the EvPlayerDamaged attacker when
 *    the LOCAL pawn is the victim.
 *  - fade: fully opaque inside kTFNameplateFadeStartM, gone at
 *    kTFNameplateMaxDistM.
 *
 * Projection mirrors TFWorldSetup::ComputeViewProj's first-person branch
 * (LookAtLH from the module camera pose + PerspectiveFovLH(XM_PIDIV4 * 1.6,
 * aspect, 0.3, 6000)) — see the MUST-MATCH note in the .cpp before touching
 * either side.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>
#include <unordered_map>

namespace Terrafront
{

    // Tuning (UI-only; nothing here affects movement, collision, or damage).
    constexpr float kTFNameplateMaxDistM = 80.0f;      ///< plates gone beyond this
    constexpr float kTFNameplateFadeStartM = 30.0f;    ///< full alpha inside this
    constexpr float kTFNameplateHeadM = 1.9f;          ///< feet -> plate anchor offset
    constexpr float kTFNameplateRevealSec = 5.0f;      ///< enemy reveal after damage
    constexpr float kTFNameplateAimConeCos = 0.99939f; ///< cos(2 deg) aim cone

    class TFNameplates
    {
      public:
        TFNameplates();
        ~TFNameplates();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

      private:
        void OnPlayerDamagedBus(const EvPlayerDamaged& ev);
        void SweepReplicatedVitals(); ///< health-drop detection + reveal upkeep
        void RevealPawn(EntityId pawnNetEntity);

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        double m_clock{0.0}; ///< local UI clock (reveal timing only — not sim time)

        // Replicated health+shield per remote alive pawn, last frame vs this
        // frame (drop => "damaged recently"). Swapped, never reallocated.
        std::unordered_map<EntityId, float> m_prevVitals;
        std::unordered_map<EntityId, float> m_curVitals;

        /// Enemy pawns temporarily revealed by damage: net entity -> clock
        /// deadline. Expired entries are pruned inside the vitals sweep.
        std::unordered_map<EntityId, double> m_revealUntil;

        // Debug counters (RenderDebugUI).
        uint32_t m_platesDrawn{0};
        uint32_t m_revealedNow{0};
    };

} // namespace Terrafront
