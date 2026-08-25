/**
 * @file OWWildlifeSystem.h
 * @brief Wildlife ecology: animal spawning, herds, predator-prey, taming
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages the open world's animal population: species templates with biome
 * affinity, herd formation, predator-prey AI interactions, day/night
 * activity cycles, and a taming system for mounts and companions.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/OpenWorldEnums.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenWorld
{

    /// @brief Template defining an animal species
    struct AnimalSpecies
    {
        AnimalType type = AnimalType::Deer;
        std::string name;
        DietType diet = DietType::Herbivore;
        float health = 50.0f;
        float speed = 5.0f;
        float detectionRange = 30.0f;
        float attackDamage = 0.0f; ///< 0 for passive animals
        float fleeSpeed = 8.0f;
        bool isNocturnal = false;
        bool isTameable = false;
        bool formsHerds = false;
        uint32_t herdSizeMin = 1;
        uint32_t herdSizeMax = 1;
        std::vector<Biome> nativeBiomes;
        std::vector<ResourceType> drops; ///< Resources dropped when hunted
    };

    /// @brief Runtime state for a single spawned animal
    struct AnimalInstance
    {
        uint32_t instanceId = 0;
        AnimalType type = AnimalType::Deer;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float health = 50.0f;
        AnimalBehavior behavior = AnimalBehavior::Grazing;
        uint32_t herdId = 0; ///< 0 = solitary
        uint32_t regionId = 0;
        bool isTamed = false;
        bool isAlive = true;
    };

    /// @brief A group of animals moving together
    struct Herd
    {
        uint32_t herdId = 0;
        AnimalType type = AnimalType::Deer;
        std::vector<uint32_t> memberIds;
        float centerX = 0.0f;
        float centerZ = 0.0f;
        uint32_t regionId = 0;
    };

    /// @brief Mutable wildlife population stored in an OpenWorld save.
    struct WildlifeSaveState
    {
        std::vector<AnimalInstance> animals;
        std::vector<Herd> herds;
        uint32_t nextInstanceId = 1;
        uint32_t nextHerdId = 1;
        float respawnTimer = 0.0f;
    };

    /**
     * @brief Wildlife population, ecology, and taming system
     */
    class OWWildlifeSystem
    {
      public:
        OWWildlifeSystem() = default;
        ~OWWildlifeSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime, float playerX, float playerZ, uint32_t currentRegionId);
        void Shutdown();
        void RenderDebugUI();

        size_t GetSpeciesCount() const { return m_species.size(); }
        size_t GetActiveAnimalCount() const { return m_animals.size(); }
        size_t GetHerdCount() const { return m_herds.size(); }
        size_t GetTamedCount() const;

        /// @brief Attempt to tame an animal (returns true if successful)
        bool TameAnimal(uint32_t instanceId);

        /// @brief Hunt an animal, returning its resource drops
        std::vector<std::pair<ResourceType, uint32_t>> HuntAnimal(uint32_t instanceId);

        WildlifeSaveState CaptureSaveState() const;
        bool RestoreSaveState(const WildlifeSaveState& state, std::string* error = nullptr);

        std::string GetWildlifeString() const;
        std::string GetSpeciesListString() const;

      private:
        void DefineSpecies();
        void SpawnRegionWildlife(uint32_t regionId);
        void UpdateBehaviors(float deltaTime, float playerX, float playerZ);
        void UpdatePredatorPrey(float deltaTime);
        void RespawnCheck(float deltaTime);

        Spark::IEngineContext* m_context{nullptr};
        std::vector<AnimalSpecies> m_species;
        std::unordered_map<uint32_t, AnimalInstance> m_animals;
        std::unordered_map<uint32_t, Herd> m_herds;
        uint32_t m_nextInstanceId{1};
        uint32_t m_nextHerdId{1};
        float m_respawnTimer{0.0f};
        bool m_initialized{false};
    };

} // namespace OpenWorld
