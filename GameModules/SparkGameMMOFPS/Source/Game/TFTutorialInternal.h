/**
 * @file TFTutorialInternal.h
 * @brief Shared internals for the TFTutorial*.cpp split parts: the step-label
 *        table, the move/sprint hold tuning and the class-terminal anchor
 *        used by both step detection and the checklist UI. Include only from
 *        the TFTutorial translation units.
 */
#pragma once

#include "Game/TFTutorial.h"

namespace Terrafront
{
    namespace TutorialDetail
    {

        // ---- step tuning --------------------------------------------------------
        inline constexpr float kMoveHoldSec = 1.5f;   ///< cumulative WASD time for Move
        inline constexpr float kSprintHoldSec = 1.0f; ///< cumulative Shift+move time

        // ---- world anchors (own copies — cosmetic markers only) ----------------
        // MUST match TFSanctuaryDecor.cpp kClassTermX / kClassTermZ.
        inline constexpr float kClassTermX = 320.0f;
        inline constexpr float kClassTermZ = 3826.0f;

        inline const char* StepLabel(TFTutorial::Step s)
        {
            switch (s)
            {
            case TFTutorial::Step::Move:
                return "Move around (W A S D)";
            case TFTutorial::Step::Sprint:
                return "Sprint (hold SHIFT while moving)";
            case TFTutorial::Step::Jump:
                return "Jump (SPACE)";
            case TFTutorial::Step::RangeHits:
                return "Firing range: hit a dummy 5 times";
            case TFTutorial::Step::Optics:
                return "Cycle your weapon sights (B)";
            case TFTutorial::Step::Ability:
                return "Use your class ability (F)";
            case TFTutorial::Step::Map:
                return "Open the continent map (M)";
            case TFTutorial::Step::Ping:
                return "Place a ping (Q)";
            case TFTutorial::Step::ClassTerm:
                return "Visit the class terminal";
            case TFTutorial::Step::Travel:
                return "Travel terminal: deploy when ready (E)";
            default:
                return "?";
            }
        }

    } // namespace TutorialDetail
} // namespace Terrafront
