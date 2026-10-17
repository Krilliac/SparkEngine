/**
 * @file RTSScriptedCommander.h
 * @brief Tick-scheduled Human player for automated skirmishes (`rts_autoplay`)
 *
 * The commander issues the local Human player's orders from simulation state alone: train marines at fixed ticks,
 * hold the base while the Swarm AI attacks, counter-attack at COUNTER_ATTACK_TICK, then once per simulated second
 * send the idle army at the oldest remaining Swarm structure (or unit). RTSSkirmishSimulation::Step() calls it at the
 * start of every tick, so the order stream -- and with it the whole skirmish -- depends only on the tick, never on
 * frame pacing or wall-clock time.
 *
 * Contract: game thread only, called from inside RTSSkirmishSimulation::Step(). Stateless: owned by the RTS module
 * (or a test) and bound with RTSSkirmishSimulation::SetScriptedCommander(); saves are unaffected. Allocates only the
 * id lists the systems return.
 */

#pragma once

#include <cstdint>

namespace RTS
{
    struct RTSSkirmishSystems;

    /// @brief Deterministic Human-side input stream for the default skirmish.
    class RTSScriptedCommander
    {
      public:
        static constexpr uint64_t TICKS_PER_SECOND = 32; ///< 1 / RTSSkirmishSimulation::TICK_SECONDS
        static constexpr uint64_t COUNTER_ATTACK_TICK = TICKS_PER_SECOND * 45;

        /// @brief Issue this tick's Human orders; reads only simulation state.
        void Apply(const RTSSkirmishSystems& systems, uint64_t tick) const;
    };
} // namespace RTS
