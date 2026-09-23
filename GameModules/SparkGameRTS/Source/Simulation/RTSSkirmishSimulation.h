/**
 * @file RTSSkirmishSimulation.h
 * @brief Deterministic fixed-step skirmish tick: commands, combat, economy, fog, AI, and win/loss
 * @author Spark Engine Team
 * @date 2026
 *
 * The skirmish advances only in whole fixed ticks of TICK_SECONDS. Wall-clock frame time is accumulated and
 * converted into ticks, so the simulated outcome depends only on the initial state and the command stream, never on
 * frame pacing. Every per-tick loop walks entities in ascending id order, and all simulation arithmetic is IEEE-754
 * single precision with correctly rounded operations only (no std::hypot / transcendental calls), so the same inputs
 * produce bit-identical state on every run.
 */

#pragma once

#include "Enums/RTSEnums.h"

#include <cstdint>

namespace Spark
{
    class IEngineContext;
}

namespace RTS
{
    class RTSBuildingSystem;
    class RTSCommandSystem;
    class RTSFogOfWarSystem;
    class RTSMatchSystem;
    class RTSResourceSystem;
    class RTSUnitSystem;

    /// @brief Non-owning pointers to the gameplay systems the skirmish tick drives
    struct RTSSkirmishSystems
    {
        RTSUnitSystem* units{nullptr};
        RTSBuildingSystem* buildings{nullptr};
        RTSResourceSystem* resources{nullptr};
        RTSCommandSystem* commands{nullptr};
        RTSFogOfWarSystem* fog{nullptr};
        RTSMatchSystem* match{nullptr};
    };

    /**
     * @brief Owns the ordered, fixed-step skirmish loop
     *
     * Tick order: AI opponents -> commands/movement -> combat -> unit cleanup -> construction/production ->
     * economy -> fog of war -> elimination and win/loss.
     */
    class RTSSkirmishSimulation
    {
      public:
        static constexpr float TICK_SECONDS = 1.0f / 32.0f;  ///< Exactly representable fixed step
        static constexpr uint32_t MAX_TICKS_PER_ADVANCE = 8; ///< Drop backlog instead of spiralling after a hitch
        static constexpr int MAP_SIZE = 96;
        static constexpr uint32_t AI_DECISION_TICKS = 32; ///< AI opponents think once per simulated second
        static constexpr int AI_ATTACK_WAVE_SIZE = 4;     ///< Idle army size that triggers an AI attack wave

        /**
         * @brief Bind the gameplay systems. All pointers are required.
         * @return false if any system pointer is null.
         */
        bool Initialize(Spark::IEngineContext* context, const RTSSkirmishSystems& systems);
        void Shutdown();

        /**
         * @brief Re-initialize every gameplay system and spawn the default Human-versus-Swarm-AI skirmish.
         * @return false if a system fails to initialize.
         */
        bool StartDefaultSkirmish();

        /**
         * @brief Convert wall-clock frame time into whole fixed ticks.
         * @param frameSeconds  Elapsed real time; non-finite or negative values are ignored.
         * @return Number of fixed ticks executed.
         */
        uint32_t Advance(float frameSeconds);

        /** @brief Execute exactly one fixed tick. No-op once the match has left the Playing state. */
        void Step();

        /** @brief Discard accumulated wall-clock time and restart the tick counter (after reset or load). */
        void ResetClock();

        uint64_t GetTick() const;

        /** @brief FNV-1a hash of the complete simulation state, walked in canonical (id) order. */
        uint64_t ComputeStateHash() const;

      private:
        void RunAIOpponents();
        void ResolveCombat();
        void RefreshVision();
        void UpdateEliminations();

        Spark::IEngineContext* m_context{nullptr};
        RTSSkirmishSystems m_systems;
        double m_accumulatedSeconds{0.0};
        uint64_t m_tick{0};
    };

} // namespace RTS
