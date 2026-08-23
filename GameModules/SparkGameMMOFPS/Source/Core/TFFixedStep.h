/**
 * @file TFFixedStep.h
 * @brief Shared TERRAFRONT authoritative fixed-step schedule.
 */

#pragma once

namespace Terrafront::Detail
{
    /// Keep the production fixed-step order in one testable function. Each
    /// argument only needs to expose FixedUpdate(float), which lets the test
    /// suite verify the complete schedule without booting graphics/network UI.
    template <typename ServerSim, typename Players, typename Abilities, typename Grenades, typename Weapons,
              typename Damage, typename Vehicles, typename Regions, typename Travel, typename Bots,
              typename Replication>
    void RunFixedStep(float fixedDeltaTime, ServerSim& serverSim, Players& players, Abilities& abilities,
                      Grenades& grenades, Weapons& weapons, Damage& damage, Vehicles& vehicles, Regions& regions,
                      Travel& travel, Bots& bots, Replication& replication)
    {
        serverSim.FixedUpdate(fixedDeltaTime);
        players.FixedUpdate(fixedDeltaTime);
        abilities.FixedUpdate(fixedDeltaTime);
        grenades.FixedUpdate(fixedDeltaTime);
        weapons.FixedUpdate(fixedDeltaTime);
        damage.FixedUpdate(fixedDeltaTime);
        vehicles.FixedUpdate(fixedDeltaTime);
        regions.FixedUpdate(fixedDeltaTime);
        travel.FixedUpdate(fixedDeltaTime);
        bots.FixedUpdate(fixedDeltaTime);
        replication.FixedUpdate(fixedDeltaTime);
    }
} // namespace Terrafront::Detail
