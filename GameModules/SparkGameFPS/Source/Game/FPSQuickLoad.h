/**
 * @file FPSQuickLoad.h
 * @brief Transactional load of an FPS save slot together with its local profile block.
 *
 * Contract:
 * - Thread affinity: game thread (it restores the live ECS world).
 * - Ownership: borrows every argument for the duration of the call.
 * - Allocation: the parsed save and custom-state map; runs on quick-load, never per frame.
 * - Scalability tier: one call per player-initiated load.
 */

#pragma once

#include "FPSLocalProfile.h"

#include <string>

class World;

namespace Spark
{
    class SaveSystem;

    /// Outcome of LoadSlotWithProfile.
    enum class FPSQuickLoadStatus
    {
        Loaded,          ///< World and profile both come from the slot.
        LoadFailed,      ///< The slot did not parse or restore; the world is unchanged.
        ProfileRejected, ///< The profile block failed validation; the world is unchanged.
    };

    /**
     * @brief Load @p slotName and its FPS profile block as one transaction.
     *
     * The profile is parsed inside SaveSystem's pre-commit custom-state validator, which runs
     * after the file parses and before the world is replaced. A missing, malformed, retired
     * or future profile block therefore rejects the load with the live world untouched,
     * instead of committing the saved world next to the previous session's profile.
     *
     * @param saveSystem      Save system that owns the slot.
     * @param slotName        Slot to load.
     * @param world           Live world, replaced only on Loaded.
     * @param outProfile      Receives the validated profile; unchanged unless Loaded.
     * @param outProfileError Receives FPSLocalProfile::ReadFrom's reason on ProfileRejected.
     * @return The outcome; only Loaded changes @p world or @p outProfile.
     */
    [[nodiscard]] FPSQuickLoadStatus LoadSlotWithProfile(SaveSystem& saveSystem, const std::string& slotName,
                                                         ::World& world, FPSLocalProfile& outProfile,
                                                         std::string& outProfileError);
} // namespace Spark
