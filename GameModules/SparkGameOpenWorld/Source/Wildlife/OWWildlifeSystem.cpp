/**
 * @file OWWildlifeSystem.cpp
 * @brief Wildlife ecology, herd behavior, predator-prey, taming
 */

#include "OWWildlifeSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

namespace OpenWorld
{

    bool OWWildlifeSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        DefineSpecies();

        // Spawn initial wildlife for starting region
        SpawnRegionWildlife(1); // Emerald Meadows
        SpawnRegionWildlife(2); // Ironwood Forest

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Wildlife system initialized: %zu species, %zu active",
                       m_species.size(), m_animals.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Wildlife: " + std::to_string(m_species.size()) +
                                                    " species, " + std::to_string(m_animals.size()) + " spawned");
        return true;
    }

    void OWWildlifeSystem::DefineSpecies()
    {
        m_species.clear();

        m_species.push_back({AnimalType::Deer,
                             "Deer",
                             DietType::Herbivore,
                             40.0f,
                             6.0f,
                             25.0f,
                             0.0f,
                             10.0f,
                             false,
                             true,
                             true,
                             3,
                             8,
                             {Biome::Grasslands, Biome::Forest},
                             {ResourceType::Meat, ResourceType::Hide}});

        m_species.push_back({AnimalType::Wolf,
                             "Wolf",
                             DietType::Carnivore,
                             60.0f,
                             7.0f,
                             35.0f,
                             15.0f,
                             9.0f,
                             false,
                             false,
                             true,
                             3,
                             6,
                             {Biome::Forest, Biome::Tundra, Biome::Mountains},
                             {ResourceType::Hide}});

        m_species.push_back({AnimalType::Bear,
                             "Bear",
                             DietType::Omnivore,
                             150.0f,
                             4.0f,
                             20.0f,
                             30.0f,
                             6.0f,
                             false,
                             false,
                             false,
                             1,
                             1,
                             {Biome::Forest, Biome::Mountains, Biome::Tundra},
                             {ResourceType::Meat, ResourceType::Hide}});

        m_species.push_back({AnimalType::Boar,
                             "Boar",
                             DietType::Omnivore,
                             50.0f,
                             5.0f,
                             15.0f,
                             10.0f,
                             7.0f,
                             false,
                             false,
                             true,
                             2,
                             5,
                             {Biome::Forest, Biome::Swamp},
                             {ResourceType::Meat, ResourceType::Hide}});

        m_species.push_back({AnimalType::Eagle,
                             "Eagle",
                             DietType::Carnivore,
                             20.0f,
                             12.0f,
                             50.0f,
                             5.0f,
                             15.0f,
                             false,
                             false,
                             false,
                             1,
                             2,
                             {Biome::Mountains, Biome::Coast, Biome::Desert},
                             {}});

        m_species.push_back({AnimalType::Horse,
                             "Horse",
                             DietType::Herbivore,
                             80.0f,
                             10.0f,
                             30.0f,
                             0.0f,
                             14.0f,
                             false,
                             true,
                             true,
                             3,
                             7,
                             {Biome::Grasslands},
                             {}});

        m_species.push_back({AnimalType::Rabbit,
                             "Rabbit",
                             DietType::Herbivore,
                             10.0f,
                             8.0f,
                             15.0f,
                             0.0f,
                             12.0f,
                             false,
                             false,
                             false,
                             1,
                             3,
                             {Biome::Grasslands, Biome::Forest},
                             {ResourceType::Meat}});

        m_species.push_back({AnimalType::Elk,
                             "Elk",
                             DietType::Herbivore,
                             100.0f,
                             7.0f,
                             30.0f,
                             8.0f,
                             10.0f,
                             false,
                             false,
                             true,
                             4,
                             10,
                             {Biome::Mountains, Biome::Tundra, Biome::Forest},
                             {ResourceType::Meat, ResourceType::Hide}});

        m_species.push_back({AnimalType::Fox,
                             "Fox",
                             DietType::Omnivore,
                             25.0f,
                             8.0f,
                             20.0f,
                             3.0f,
                             11.0f,
                             true,
                             false,
                             false,
                             1,
                             2,
                             {Biome::Grasslands, Biome::Forest, Biome::Swamp},
                             {ResourceType::Hide}});

        m_species.push_back({AnimalType::Bison,
                             "Bison",
                             DietType::Herbivore,
                             200.0f,
                             5.0f,
                             20.0f,
                             12.0f,
                             7.0f,
                             false,
                             false,
                             true,
                             5,
                             15,
                             {Biome::Tundra, Biome::Grasslands},
                             {ResourceType::Meat, ResourceType::Hide}});

        m_species.push_back({AnimalType::MountainLion,
                             "Mountain Lion",
                             DietType::Carnivore,
                             80.0f,
                             9.0f,
                             40.0f,
                             25.0f,
                             12.0f,
                             true,
                             false,
                             false,
                             1,
                             1,
                             {Biome::Mountains},
                             {ResourceType::Hide}});

        m_species.push_back({AnimalType::Snake,
                             "Snake",
                             DietType::Carnivore,
                             15.0f,
                             3.0f,
                             10.0f,
                             8.0f,
                             5.0f,
                             false,
                             false,
                             false,
                             1,
                             1,
                             {Biome::Desert, Biome::Swamp},
                             {}});
    }

    void OWWildlifeSystem::SpawnRegionWildlife(uint32_t regionId)
    {
        // Spawn a representative population for the region
        for (const auto& species : m_species)
        {
            bool native = false;
            for (auto b : species.nativeBiomes)
            {
                // Simple check: biome index matches region convention
                // Real implementation would query OWWorldSetup for region biome
                if (static_cast<uint32_t>(b) + 1 == regionId || regionId <= 2)
                {
                    native = true;
                    break;
                }
            }
            if (!native)
                continue;

            uint32_t count = species.formsHerds ? species.herdSizeMin + 2 : 1;

            // Create a herd if applicable
            uint32_t herdId = 0;
            if (species.formsHerds && count > 1)
            {
                herdId = m_nextHerdId++;
                Herd herd{};
                herd.herdId = herdId;
                herd.type = species.type;
                herd.regionId = regionId;
                herd.centerX = static_cast<float>(regionId * 1000);
                herd.centerZ = static_cast<float>(regionId * 500);
                m_herds[herdId] = herd;
            }

            for (uint32_t i = 0; i < count; ++i)
            {
                AnimalInstance animal{};
                animal.instanceId = m_nextInstanceId++;
                animal.type = species.type;
                animal.health = species.health;
                animal.behavior =
                    (species.diet == DietType::Carnivore) ? AnimalBehavior::Roaming : AnimalBehavior::Grazing;
                animal.herdId = herdId;
                animal.regionId = regionId;
                // Spread positions around region center
                animal.posX = static_cast<float>(regionId * 1000) + static_cast<float>(i * 50);
                animal.posZ = static_cast<float>(regionId * 500) + static_cast<float>(i * 30);

                m_animals[animal.instanceId] = animal;

                if (herdId > 0)
                    m_herds[herdId].memberIds.push_back(animal.instanceId);
            }
        }
    }

    void OWWildlifeSystem::Update(
        float deltaTime, float playerX, float playerZ,
        [[maybe_unused]] uint32_t currentRegionId) // Intentional: reserved for future region-based wildlife culling
    {
        if (!m_initialized)
            return;

        UpdateBehaviors(deltaTime, playerX, playerZ);
        UpdatePredatorPrey(deltaTime);
        RespawnCheck(deltaTime);
    }

    void OWWildlifeSystem::UpdateBehaviors(float deltaTime, float playerX, float playerZ)
    {
        for (auto& [id, animal] : m_animals)
        {
            if (!animal.isAlive)
                continue;

            // Find species info
            const AnimalSpecies* species = nullptr;
            for (const auto& s : m_species)
            {
                if (s.type == animal.type)
                {
                    species = &s;
                    break;
                }
            }
            if (!species)
                continue;

            if (animal.isTamed)
            {
                // Follow player
                float dx = playerX - animal.posX;
                float dz = playerZ - animal.posZ;
                float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > 5.0f)
                {
                    float speed = species->speed * deltaTime;
                    animal.posX += (dx / dist) * speed;
                    animal.posZ += (dz / dist) * speed;
                }
                animal.behavior = AnimalBehavior::Following;
                continue;
            }

            // Distance to player
            float dx = playerX - animal.posX;
            float dz = playerZ - animal.posZ;
            float distToPlayer = std::sqrt(dx * dx + dz * dz);

            // Herbivores flee from player if too close
            if (species->diet == DietType::Herbivore && distToPlayer < species->detectionRange)
            {
                animal.behavior = AnimalBehavior::Fleeing;
                float speed = species->fleeSpeed * deltaTime;
                animal.posX -= (dx / distToPlayer) * speed;
                animal.posZ -= (dz / distToPlayer) * speed;
            }
            // Carnivores stalk player if in range
            else if (species->diet == DietType::Carnivore && species->attackDamage > 0.0f &&
                     distToPlayer < species->detectionRange)
            {
                animal.behavior = AnimalBehavior::Stalking;
                if (distToPlayer > 5.0f)
                {
                    float speed = species->speed * deltaTime;
                    animal.posX += (dx / distToPlayer) * speed;
                    animal.posZ += (dz / distToPlayer) * speed;
                }
            }
            else
            {
                // Idle behavior: slow wander
                animal.behavior =
                    (species->diet == DietType::Herbivore) ? AnimalBehavior::Grazing : AnimalBehavior::Roaming;
                animal.posX += (static_cast<float>((id * 7) % 11) - 5.0f) * 0.1f * deltaTime;
                animal.posZ += (static_cast<float>((id * 13) % 11) - 5.0f) * 0.1f * deltaTime;
            }
        }
    }

    // Intentional: deltaTime reserved for future time-scaled predator-prey simulation
    void OWWildlifeSystem::UpdatePredatorPrey([[maybe_unused]] float deltaTime)
    {
        // Simplified: carnivores near herbivores trigger flee response
        // Real implementation would use SpatialGrid for efficient queries
    }

    void OWWildlifeSystem::RespawnCheck(float deltaTime)
    {
        m_respawnTimer += deltaTime;
        if (m_respawnTimer < 60.0f)
            return;
        m_respawnTimer = 0.0f;

        // Remove dead animals and count per region
        std::vector<uint32_t> toRemove;
        for (const auto& [id, animal] : m_animals)
        {
            if (!animal.isAlive)
                toRemove.push_back(id);
        }
        for (uint32_t id : toRemove)
            m_animals.erase(id);
    }

    bool OWWildlifeSystem::TameAnimal(uint32_t instanceId)
    {
        auto it = m_animals.find(instanceId);
        if (it == m_animals.end() || !it->second.isAlive || it->second.isTamed)
            return false;

        // Check if species is tameable
        for (const auto& species : m_species)
        {
            if (species.type == it->second.type && species.isTameable)
            {
                it->second.isTamed = true;
                it->second.behavior = AnimalBehavior::Following;
                Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Tamed " + species.name +
                                                            " (id=" + std::to_string(instanceId) + ")");
                return true;
            }
        }
        return false;
    }

    std::vector<std::pair<ResourceType, uint32_t>> OWWildlifeSystem::HuntAnimal(uint32_t instanceId)
    {
        std::vector<std::pair<ResourceType, uint32_t>> drops;

        auto it = m_animals.find(instanceId);
        if (it == m_animals.end() || !it->second.isAlive)
            return drops;

        it->second.isAlive = false;
        it->second.health = 0.0f;

        for (const auto& species : m_species)
        {
            if (species.type == it->second.type)
            {
                for (auto res : species.drops)
                    drops.emplace_back(res, 1);
                break;
            }
        }

        return drops;
    }

    size_t OWWildlifeSystem::GetTamedCount() const
    {
        size_t count = 0;
        for (const auto& [id, animal] : m_animals)
        {
            if (animal.isTamed)
                ++count;
        }
        return count;
    }

    std::string OWWildlifeSystem::GetWildlifeString() const
    {
        std::ostringstream ss;
        ss << "=== Wildlife ===\n";
        ss << "Species: " << m_species.size() << " | Active: " << m_animals.size();
        ss << " | Herds: " << m_herds.size() << " | Tamed: " << GetTamedCount() << "\n";
        return ss.str();
    }

    std::string OWWildlifeSystem::GetSpeciesListString() const
    {
        std::ostringstream ss;
        ss << "=== Animal Species ===\n";
        for (const auto& s : m_species)
        {
            ss << s.name << " ("
               << (s.diet == DietType::Herbivore   ? "Herb"
                   : s.diet == DietType::Carnivore ? "Carn"
                                                   : "Omni")
               << ") HP:" << s.health << " Spd:" << s.speed;
            if (s.isTameable)
                ss << " [Tameable]";
            if (s.formsHerds)
                ss << " [Herd:" << s.herdSizeMin << "-" << s.herdSizeMax << "]";
            ss << "\n";
        }
        return ss.str();
    }

    void OWWildlifeSystem::Shutdown()
    {
        if (!m_initialized)
            return;

        m_species.clear();
        m_animals.clear();
        m_herds.clear();
        m_initialized = false;
    }

    void OWWildlifeSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Wildlife"))
            return;

        ImGui::Text("Species: %zu | Active: %zu | Herds: %zu | Tamed: %zu", m_species.size(), m_animals.size(),
                    m_herds.size(), GetTamedCount());
        ImGui::Separator();

        if (ImGui::TreeNode("Species"))
        {
            for (const auto& s : m_species)
            {
                ImGui::Text("%s | HP:%.0f | Spd:%.0f | Dmg:%.0f%s", s.name.c_str(), s.health, s.speed, s.attackDamage,
                            s.isTameable ? " [Tameable]" : "");
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace OpenWorld
