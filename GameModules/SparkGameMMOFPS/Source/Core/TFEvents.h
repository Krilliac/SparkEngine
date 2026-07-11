/**
 * @file TFEvents.h
 * @brief TERRAFRONT in-process event structs + a tiny synchronous event bus.
 *
 * FROZEN CONTRACT (see DESIGN.md §3). Systems communicate loosely via
 * TFEventBus (owned by TerrafrontModule, reachable through TFGameContext
 * consumers by reference). Events are fired synchronously on the game thread.
 */
#pragma once

#include "TFTypes.h"
#include <functional>
#include <unordered_map>
#include <vector>
#include <typeindex>

namespace Terrafront
{

    // --------------------------------------------------------------------------
    // Event payloads
    // --------------------------------------------------------------------------

    struct EvPlayerSpawned
    {
        PlayerId player;
        EntityId pawn;
        ClassId cls;
        FactionId faction;
    };
    struct EvPlayerKilled
    {
        PlayerId victim;
        PlayerId killer;
        WeaponId weapon;
        bool headshot;
    };
    struct EvPlayerDamaged
    {
        EntityId victim;
        EntityId attacker;
        float amount;
        uint8_t kind;
    };
    struct EvRegionCaptured
    {
        RegionId region;
        FactionId newOwner;
        FactionId oldOwner;
    };
    struct EvRegionContested
    {
        RegionId region;
        bool contested;
    };
    struct EvDominion
    {
        FactionId faction;
    }; // continent locked
    struct EvVehicleSpawned
    {
        EntityId vehicle;
        VehicleId kind;
        PlayerId owner;
    };
    struct EvVehicleDestroyed
    {
        EntityId vehicle;
        VehicleId kind;
        PlayerId destroyer;
    };
    struct EvAegisDeployed
    {
        EntityId vehicle;
        bool deployed;
    };
    struct EvXPAwarded
    {
        PlayerId player;
        uint16_t amount;
        uint8_t reason;
    };
    struct EvRankUp
    {
        PlayerId player;
        uint16_t newRank;
    };
    struct EvSquadChanged
    {
        SquadId squad;
        PlayerId player;
        bool joined;
    };
    struct EvLocalPlayerDied
    {
        PlayerId player;
        float respawnDelay;
    };
    struct EvDeployablePlaced
    {
        EntityId entity;
        DeployableKind kind;
        PlayerId owner;
        FactionId faction;
    }; // W8: fired server-side on successful placement (TFDeployableSystem -> TFDirectiveSystem)
    struct EvDataReloaded
    {
    }; // tf_reload_data

    // --------------------------------------------------------------------------
    // Minimal synchronous bus
    // --------------------------------------------------------------------------

    class TFEventBus
    {
      public:
        template <typename E> void Subscribe(std::function<void(const E&)> fn)
        {
            auto& vec = m_handlers[std::type_index(typeid(E))];
            vec.push_back([fn = std::move(fn)](const void* p) { fn(*static_cast<const E*>(p)); });
        }

        template <typename E> void Fire(const E& ev) const
        {
            auto it = m_handlers.find(std::type_index(typeid(E)));
            if (it == m_handlers.end())
                return;
            for (auto& h : it->second)
                h(&ev);
        }

        void Clear() { m_handlers.clear(); }

      private:
        std::unordered_map<std::type_index, std::vector<std::function<void(const void*)>>> m_handlers;
    };

} // namespace Terrafront
