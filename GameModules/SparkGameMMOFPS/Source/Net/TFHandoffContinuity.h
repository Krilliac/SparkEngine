/**
 * @file TFHandoffContinuity.h
 * @brief Where a pawn checkpoint may cross continents, and how the destination places it.
 *
 * Each continent authority simulates its own analytic ground (TFTerrainModel.h), and those grounds differ by
 * tens of meters. The one place both agree exactly is the Sanctuary Haven pad, so a checkpoint is carried only
 * from there: the destination then resumes the pawn at the same pose, velocity and grounded state, and the next
 * movement tick continues the source's trajectory. Game-thread value helpers; no allocation.
 */
#pragma once

#include "Net/TFHandoffState.h"
#include "World/TFSanctuaryZone.h"

#include <algorithm>

namespace Terrafront
{
    /// Departure and arrival gate: a valid checkpoint standing on ground every continent shares. Used by the
    /// source's capture and by the destination before it accepts (and before it installs) the payload.
    inline bool TFHandoff_CanCarry(const TFHandoffState& state)
    {
        return state.IsValid() && TFTravel_IsOnSharedPad(state.position[0], state.position[2]);
    }

    /// Kinematic state a carried pawn resumes with on the destination.
    struct TFHandoffArrival
    {
        float position[3]{};
        float velocity[3]{};
        bool grounded = false;
    };

    /// Place @p state in the destination world. @p resolve is the destination's own post-move collision hook,
    /// resolve(const float prev[3], float pos[3], float vel[3], bool* grounded) (TFWorldSetup::
    /// ResolveMoveCollision on the server), applied as a zero-length move so bodies and the terrain backstop of
    /// THIS world, never the source's, decide contact.
    template <typename ResolveFn> TFHandoffArrival TFHandoff_Arrive(const TFHandoffState& state, ResolveFn&& resolve)
    {
        TFHandoffArrival arrival;
        std::copy_n(state.position, 3, arrival.position);
        std::copy_n(state.velocity, 3, arrival.velocity);
        arrival.grounded = state.grounded;
        resolve(state.position, arrival.position, arrival.velocity, &arrival.grounded);
        return arrival;
    }
} // namespace Terrafront
