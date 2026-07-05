/**
 * @file IAreaSimulation.h
 * @brief Plug-in interface for game-supplied authoritative area simulation
 * @author Spark Engine Team
 * @date 2026
 *
 * AreaServer's built-in UpdateSimulation() is only a placeholder velocity
 * integrator. Game modules provide real authoritative per-area simulation
 * (movement validation, combat, capture logic, ...) by implementing this
 * interface and attaching it via AreaServer::SetSimulation().
 *
 * The interface is deliberately minimal and game-agnostic: the engine drives
 * the tick, the game owns the state.
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    /**
     * @brief Game-module simulation hook driven by an AreaServer tick loop.
     *
     * Threading contract: OnAreaTick() is invoked on the AreaServer tick
     * thread (see AreaServer::SetSimulation()), once per simulation tick.
     * Implementations must not assume they run on the main/game thread.
     */
    class IAreaSimulation
    {
      public:
        virtual ~IAreaSimulation() = default;

        /**
         * @brief Run one fixed-step simulation tick for the area.
         * @param fixedDt Fixed tick delta time in seconds (1 / tickRate)
         */
        virtual void OnAreaTick(float fixedDt) = 0;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
