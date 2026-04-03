/**
 * @file OWGatheringSystem.cpp
 * @brief Resource nodes, harvesting, and crafting recipes
 */

#include "OWGatheringSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <sstream>

namespace OpenWorld
{

    static const char* ResourceName(ResourceType type)
    {
        switch (type)
        {
        case ResourceType::Wood:
            return "Wood";
        case ResourceType::Stone:
            return "Stone";
        case ResourceType::Iron:
            return "Iron";
        case ResourceType::Herbs:
            return "Herbs";
        case ResourceType::Fiber:
            return "Fiber";
        case ResourceType::Hide:
            return "Hide";
        case ResourceType::Meat:
            return "Meat";
        case ResourceType::Water:
            return "Water";
        case ResourceType::Gold:
            return "Gold";
        case ResourceType::Crystal:
            return "Crystal";
        case ResourceType::Clay:
            return "Clay";
        default:
            return "Unknown";
        }
    }

    bool OWGatheringSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        DefineResourceNodes();
        DefineCraftingRecipes();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Gathering system initialized: %zu nodes, %zu recipes", m_nodes.size(),
                       m_recipes.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Gathering: " + std::to_string(m_nodes.size()) +
                                                    " nodes, " + std::to_string(m_recipes.size()) + " recipes");
        return true;
    }

    void OWGatheringSystem::DefineResourceNodes()
    {
        m_nodes.clear();
        uint32_t id = 1;

        auto addNode = [&](const std::string& name, ResourceType type, float x, float y, float z, uint32_t region,
                           uint32_t yield, float respawn)
        {
            m_nodes[id] = {id, name, type, x, y, z, region, yield, yield, respawn, 0.0f, false};
            ++id;
        };

        // Emerald Meadows (Region 1)
        addNode("Oak Tree", ResourceType::Wood, 200.0f, 5.0f, 100.0f, 1, 5, 120.0f);
        addNode("Meadow Herbs", ResourceType::Herbs, -100.0f, 2.0f, 300.0f, 1, 3, 90.0f);
        addNode("River Water", ResourceType::Water, 0.0f, 0.0f, -200.0f, 1, 10, 60.0f);
        addNode("Tall Grass", ResourceType::Fiber, 400.0f, 3.0f, -50.0f, 1, 4, 80.0f);
        addNode("Field Stone", ResourceType::Stone, -300.0f, 5.0f, 500.0f, 1, 4, 150.0f);

        // Ironwood Forest (Region 2)
        addNode("Ironwood Tree", ResourceType::Wood, 3000.0f, 10.0f, -200.0f, 2, 8, 180.0f);
        addNode("Forest Mushrooms", ResourceType::Herbs, 2500.0f, 5.0f, 500.0f, 2, 4, 100.0f);
        addNode("Vine Fiber", ResourceType::Fiber, 3500.0f, 8.0f, 100.0f, 2, 5, 90.0f);
        addNode("Mossy Rock", ResourceType::Stone, 4000.0f, 12.0f, -500.0f, 2, 5, 150.0f);

        // Stormcrest Mountains (Region 3)
        addNode("Iron Vein", ResourceType::Iron, 500.0f, 300.0f, 3500.0f, 3, 6, 300.0f);
        addNode("Crystal Deposit", ResourceType::Crystal, -200.0f, 400.0f, 4500.0f, 3, 3, 600.0f);
        addNode("Granite Boulder", ResourceType::Stone, 800.0f, 250.0f, 3000.0f, 3, 8, 200.0f);
        addNode("Mountain Herbs", ResourceType::Herbs, 100.0f, 350.0f, 4000.0f, 3, 3, 120.0f);

        // Ashwind Desert (Region 4)
        addNode("Sandstone Slab", ResourceType::Stone, 6500.0f, 5.0f, -1500.0f, 4, 6, 180.0f);
        addNode("Gold Nuggets", ResourceType::Gold, 7500.0f, -10.0f, -2500.0f, 4, 2, 900.0f);
        addNode("Desert Clay", ResourceType::Clay, 5500.0f, 0.0f, -500.0f, 4, 5, 150.0f);
        addNode("Cactus Water", ResourceType::Water, 6000.0f, 3.0f, -1000.0f, 4, 3, 120.0f);

        // Frosthollow Tundra (Region 5)
        addNode("Frozen Timber", ResourceType::Wood, -3500.0f, 5.0f, 5000.0f, 5, 4, 200.0f);
        addNode("Ice Stone", ResourceType::Stone, -4500.0f, 10.0f, 4000.0f, 5, 6, 180.0f);
        addNode("Tundra Lichen", ResourceType::Herbs, -2500.0f, 3.0f, 3500.0f, 5, 2, 150.0f);

        // Mistveil Swamp (Region 6)
        addNode("Bogwood", ResourceType::Wood, -3500.0f, 2.0f, -800.0f, 6, 4, 120.0f);
        addNode("Marsh Reeds", ResourceType::Fiber, -4000.0f, 0.0f, 200.0f, 6, 6, 80.0f);
        addNode("Rare Herbs", ResourceType::Herbs, -3000.0f, 1.0f, -1500.0f, 6, 2, 180.0f);
        addNode("Bog Clay", ResourceType::Clay, -4500.0f, -2.0f, 0.0f, 6, 5, 150.0f);

        // Sunbreak Coast (Region 7)
        addNode("Driftwood", ResourceType::Wood, 500.0f, 1.0f, -5500.0f, 7, 3, 100.0f);
        addNode("Coastal Fiber", ResourceType::Fiber, -1000.0f, 2.0f, -6000.0f, 7, 4, 90.0f);
        addNode("Shell Deposits", ResourceType::Gold, 2000.0f, 0.0f, -6500.0f, 7, 1, 600.0f);

        // Cinderforge Caldera (Region 8)
        addNode("Obsidian Shard", ResourceType::Stone, 500.0f, 200.0f, 9000.0f, 8, 4, 300.0f);
        addNode("Volcanic Iron", ResourceType::Iron, -500.0f, 180.0f, 8500.0f, 8, 8, 240.0f);
        addNode("Fire Crystal", ResourceType::Crystal, 0.0f, 250.0f, 10000.0f, 8, 2, 900.0f);
    }

    void OWGatheringSystem::DefineCraftingRecipes()
    {
        m_recipes.clear();
        uint32_t id = 1;

        // Tools
        m_recipes.push_back({id++,
                             "Stone Axe",
                             "A crude but effective chopping tool",
                             CraftingCategory::Tools,
                             {{ResourceType::Wood, 2}, {ResourceType::Stone, 3}},
                             5.0f,
                             false});
        m_recipes.push_back({id++,
                             "Iron Pickaxe",
                             "A sturdy mining tool",
                             CraftingCategory::Tools,
                             {{ResourceType::Wood, 3}, {ResourceType::Iron, 4}},
                             8.0f,
                             true});
        m_recipes.push_back({id++,
                             "Fishing Rod",
                             "For catching fish at water sources",
                             CraftingCategory::Tools,
                             {{ResourceType::Wood, 2}, {ResourceType::Fiber, 3}},
                             5.0f,
                             false});

        // Weapons
        m_recipes.push_back({id++,
                             "Wooden Spear",
                             "A simple pointed weapon",
                             CraftingCategory::Weapons,
                             {{ResourceType::Wood, 4}, {ResourceType::Fiber, 1}},
                             6.0f,
                             false});
        m_recipes.push_back({id++,
                             "Iron Sword",
                             "A reliable combat blade",
                             CraftingCategory::Weapons,
                             {{ResourceType::Iron, 5}, {ResourceType::Wood, 2}, {ResourceType::Hide, 1}},
                             12.0f,
                             true});
        m_recipes.push_back({id++,
                             "Hunting Bow",
                             "A bow for ranged hunting and combat",
                             CraftingCategory::Weapons,
                             {{ResourceType::Wood, 3}, {ResourceType::Fiber, 4}},
                             8.0f,
                             false});
        m_recipes.push_back({id++,
                             "Crystal Blade",
                             "A weapon infused with crystal energy",
                             CraftingCategory::Weapons,
                             {{ResourceType::Iron, 6}, {ResourceType::Crystal, 3}, {ResourceType::Gold, 1}},
                             20.0f,
                             true});

        // Armor
        m_recipes.push_back({id++,
                             "Leather Vest",
                             "Basic torso protection",
                             CraftingCategory::Armor,
                             {{ResourceType::Hide, 5}, {ResourceType::Fiber, 2}},
                             10.0f,
                             false});
        m_recipes.push_back({id++,
                             "Iron Chainmail",
                             "Medium armor with good protection",
                             CraftingCategory::Armor,
                             {{ResourceType::Iron, 8}, {ResourceType::Hide, 2}},
                             15.0f,
                             true});
        m_recipes.push_back({id++,
                             "Fur Cloak",
                             "Provides warmth in cold biomes",
                             CraftingCategory::Armor,
                             {{ResourceType::Hide, 4}, {ResourceType::Fiber, 3}},
                             8.0f,
                             false});

        // Consumables
        m_recipes.push_back({id++,
                             "Herbal Potion",
                             "Restores health over time",
                             CraftingCategory::Consumables,
                             {{ResourceType::Herbs, 3}, {ResourceType::Water, 1}},
                             5.0f,
                             false});
        m_recipes.push_back({id++,
                             "Cooked Meat",
                             "Satisfying meal that restores hunger",
                             CraftingCategory::Consumables,
                             {{ResourceType::Meat, 2}},
                             3.0f,
                             false});
        m_recipes.push_back({id++,
                             "Purified Water",
                             "Clean drinking water",
                             CraftingCategory::Consumables,
                             {{ResourceType::Water, 2}},
                             2.0f,
                             false});
        m_recipes.push_back({id++,
                             "Antidote",
                             "Cures poison effects",
                             CraftingCategory::Consumables,
                             {{ResourceType::Herbs, 4}, {ResourceType::Water, 1}},
                             6.0f,
                             false});

        // Building
        m_recipes.push_back({id++,
                             "Campfire Kit",
                             "Materials for a basic camp",
                             CraftingCategory::Building,
                             {{ResourceType::Wood, 5}, {ResourceType::Stone, 3}},
                             8.0f,
                             false});
        m_recipes.push_back({id++,
                             "Lean-To Frame",
                             "Upgrade materials for camp shelter",
                             CraftingCategory::Building,
                             {{ResourceType::Wood, 10}, {ResourceType::Fiber, 5}},
                             12.0f,
                             false});
        m_recipes.push_back({id++,
                             "Tent Canvas",
                             "Fabric for a proper tent",
                             CraftingCategory::Building,
                             {{ResourceType::Fiber, 12}, {ResourceType::Hide, 4}},
                             15.0f,
                             false});
        m_recipes.push_back({id++,
                             "Cabin Logs",
                             "Heavy timber for a permanent structure",
                             CraftingCategory::Building,
                             {{ResourceType::Wood, 25}, {ResourceType::Stone, 10}, {ResourceType::Iron, 5}},
                             30.0f,
                             true});

        // Alchemy
        m_recipes.push_back({id++,
                             "Warmth Tonic",
                             "Temporarily raises body temperature",
                             CraftingCategory::Alchemy,
                             {{ResourceType::Herbs, 2}, {ResourceType::Water, 1}, {ResourceType::Crystal, 1}},
                             8.0f,
                             true});
        m_recipes.push_back({id++,
                             "Stamina Elixir",
                             "Boosts stamina recovery",
                             CraftingCategory::Alchemy,
                             {{ResourceType::Herbs, 3}, {ResourceType::Crystal, 1}},
                             10.0f,
                             true});
    }

    void OWGatheringSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        UpdateNodeRespawns(deltaTime);
    }

    void OWGatheringSystem::UpdateNodeRespawns(float deltaTime)
    {
        for (auto& [id, node] : m_nodes)
        {
            if (!node.isDepleted)
                continue;

            node.respawnTimer += deltaTime;
            if (node.respawnTimer >= node.respawnTime)
            {
                node.isDepleted = false;
                node.currentYield = node.maxYield;
                node.respawnTimer = 0.0f;
            }
        }
    }

    uint32_t OWGatheringSystem::HarvestNode(uint32_t nodeId)
    {
        auto it = m_nodes.find(nodeId);
        if (it == m_nodes.end() || it->second.isDepleted)
            return 0;

        auto& node = it->second;
        uint32_t harvested = std::min(node.currentYield, 1u);
        node.currentYield -= harvested;

        if (node.currentYield == 0)
        {
            node.isDepleted = true;
            node.respawnTimer = 0.0f;
        }

        m_inventory.Add(node.resource, harvested);
        m_totalHarvested += harvested;

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Harvested " + std::to_string(harvested) + "x " +
                                                    std::string(ResourceName(node.resource)) + " from " + node.name);
        return harvested;
    }

    void OWGatheringSystem::AddResource(ResourceType type, uint32_t amount)
    {
        m_inventory.Add(type, amount);
    }

    bool OWGatheringSystem::CanCraft(uint32_t recipeId) const
    {
        for (const auto& recipe : m_recipes)
        {
            if (recipe.recipeId != recipeId)
                continue;

            for (const auto& ing : recipe.ingredients)
            {
                if (m_inventory.Get(ing.resource) < ing.amount)
                    return false;
            }
            return true;
        }
        return false;
    }

    bool OWGatheringSystem::Craft(uint32_t recipeId)
    {
        if (!CanCraft(recipeId))
            return false;

        for (const auto& recipe : m_recipes)
        {
            if (recipe.recipeId != recipeId)
                continue;

            for (const auto& ing : recipe.ingredients)
                m_inventory.Spend(ing.resource, ing.amount);

            m_totalCrafted++;
            Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Crafted: " + recipe.resultName);
            return true;
        }
        return false;
    }

    std::string OWGatheringSystem::GetNodeListString() const
    {
        std::ostringstream ss;
        ss << "=== Resource Nodes ===\n";
        for (const auto& [id, node] : m_nodes)
        {
            ss << "[" << id << "] " << node.name << " (" << ResourceName(node.resource) << ")";
            if (node.isDepleted)
                ss << " [DEPLETED]";
            else
                ss << " [" << node.currentYield << "/" << node.maxYield << "]";
            ss << "\n";
        }
        return ss.str();
    }

    std::string OWGatheringSystem::GetRecipeListString() const
    {
        std::ostringstream ss;
        ss << "=== Crafting Recipes ===\n";
        for (const auto& r : m_recipes)
        {
            ss << "[" << r.recipeId << "] " << r.resultName;
            if (r.requiresCraftingStation)
                ss << " [Station]";
            ss << " — ";
            for (size_t i = 0; i < r.ingredients.size(); ++i)
            {
                if (i > 0)
                    ss << ", ";
                ss << r.ingredients[i].amount << "x " << ResourceName(r.ingredients[i].resource);
            }
            ss << "\n";
        }
        return ss.str();
    }

    std::string OWGatheringSystem::GetInventoryString() const
    {
        std::ostringstream ss;
        ss << "=== Resource Inventory ===\n";
        for (const auto& [type, count] : m_inventory.resources)
        {
            if (count > 0)
                ss << ResourceName(type) << ": " << count << "\n";
        }
        ss << "Total Harvested: " << m_totalHarvested << " | Total Crafted: " << m_totalCrafted << "\n";
        return ss.str();
    }

    void OWGatheringSystem::Shutdown()
    {
        if (!m_initialized)
            return;

        m_nodes.clear();
        m_recipes.clear();
        m_initialized = false;
    }

    void OWGatheringSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Gathering & Crafting"))
            return;

        ImGui::Text("Nodes: %zu | Recipes: %zu | Harvested: %u | Crafted: %u", m_nodes.size(), m_recipes.size(),
                    m_totalHarvested, m_totalCrafted);
        ImGui::Separator();

        if (ImGui::TreeNode("Inventory"))
        {
            for (const auto& [type, count] : m_inventory.resources)
            {
                if (count > 0)
                    ImGui::Text("%s: %u", ResourceName(type), count);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Recipes"))
        {
            for (const auto& r : m_recipes)
            {
                bool canCraft = CanCraft(r.recipeId);
                ImGui::Text("[%u] %s %s", r.recipeId, r.resultName.c_str(), canCraft ? "[CAN CRAFT]" : "");
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace OpenWorld
