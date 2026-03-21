/**
 * @file MMOCraftingSystem.cpp
 * @brief Recipe registry, crafting execution, and skill progression
 */

#include "MMOCraftingSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <sstream>

namespace MMO
{

    bool MMOCraftingSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultRecipes();
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Crafting system initialized (" +
                                                    std::to_string(m_recipes.size()) + " recipes)");
        return true;
    }

    void MMOCraftingSystem::Shutdown()
    {
        m_recipes.clear();
    }

    void MMOCraftingSystem::RegisterDefaultRecipes()
    {
        RegisterRecipe({100,
                        "Health Potion",
                        "Brew a potion restoring 50 HP",
                        CraftingDiscipline::Alchemy,
                        CraftingStation::AlchemyLab,
                        1,
                        {{10, 2}, {11, 1}},
                        1,
                        1,
                        3.0f,
                        15});

        RegisterRecipe({101,
                        "Shield Elixir",
                        "Grants temporary shields",
                        CraftingDiscipline::Alchemy,
                        CraftingStation::AlchemyLab,
                        10,
                        {{10, 3}, {11, 1}, {12, 1}},
                        3,
                        1,
                        5.0f,
                        25});

        RegisterRecipe({200,
                        "Iron Sword",
                        "Forge a sturdy blade",
                        CraftingDiscipline::Weaponsmithing,
                        CraftingStation::Forge,
                        1,
                        {{13, 3}, {14, 1}},
                        30,
                        1,
                        8.0f,
                        30});

        RegisterRecipe({201,
                        "Steel Rifle Barrel",
                        "Machine a precision barrel",
                        CraftingDiscipline::Weaponsmithing,
                        CraftingStation::Forge,
                        25,
                        {{15, 5}, {16, 2}},
                        31,
                        1,
                        15.0f,
                        50});

        RegisterRecipe({300,
                        "Frag Grenade",
                        "Explosive fragmentation grenade",
                        CraftingDiscipline::Engineering,
                        CraftingStation::Workbench,
                        5,
                        {{13, 2}, {17, 1}},
                        40,
                        3,
                        4.0f,
                        20});

        RegisterRecipe({301,
                        "Turret Kit",
                        "Portable automated turret",
                        CraftingDiscipline::Engineering,
                        CraftingStation::Workbench,
                        30,
                        {{15, 4}, {16, 2}, {18, 1}},
                        41,
                        1,
                        20.0f,
                        60});

        RegisterRecipe({400,
                        "Combat Ration",
                        "Meal boosting stamina regen",
                        CraftingDiscipline::Cooking,
                        CraftingStation::CookingFire,
                        1,
                        {{19, 2}},
                        50,
                        2,
                        5.0f,
                        10});

        RegisterRecipe({500,
                        "Field Bandage",
                        "Quick bandage from cloth",
                        CraftingDiscipline::Alchemy,
                        CraftingStation::None,
                        1,
                        {{20, 2}},
                        51,
                        3,
                        2.0f,
                        5});

        RegisterRecipe({600,
                        "Iron Chestplate",
                        "Basic iron body armor",
                        CraftingDiscipline::Armorsmithing,
                        CraftingStation::Forge,
                        5,
                        {{13, 5}, {14, 3}},
                        60,
                        1,
                        12.0f,
                        35});

        RegisterRecipe({601,
                        "Steel Helmet",
                        "Reinforced steel helmet",
                        CraftingDiscipline::Armorsmithing,
                        CraftingStation::Forge,
                        15,
                        {{15, 3}, {14, 2}},
                        61,
                        1,
                        10.0f,
                        40});
    }

    void MMOCraftingSystem::RegisterRecipe(const CraftingRecipe& recipe)
    {
        m_recipes[recipe.id] = recipe;
    }

    const CraftingRecipe* MMOCraftingSystem::GetRecipe(uint32_t id) const
    {
        auto it = m_recipes.find(id);
        return it != m_recipes.end() ? &it->second : nullptr;
    }

    std::vector<const CraftingRecipe*> MMOCraftingSystem::GetAvailableRecipes(const CraftingState& crafter) const
    {
        std::vector<const CraftingRecipe*> result;
        for (const auto& [id, recipe] : m_recipes)
        {
            if (!KnowsRecipe(crafter, id))
                continue;
            if (GetSkillLevel(crafter, recipe.discipline) < recipe.requiredSkillLevel)
                continue;
            if (recipe.station != CraftingStation::None && recipe.station != crafter.nearbyStation)
                continue;
            result.push_back(&recipe);
        }
        return result;
    }

    bool MMOCraftingSystem::CanCraft(const CraftingState& crafter, const InventoryData& inv, uint32_t recipeId) const
    {
        const auto* recipe = GetRecipe(recipeId);
        if (!recipe || !KnowsRecipe(crafter, recipeId))
            return false;
        if (GetSkillLevel(crafter, recipe->discipline) < recipe->requiredSkillLevel)
            return false;
        if (recipe->station != CraftingStation::None && recipe->station != crafter.nearbyStation)
            return false;

        if (m_invSystem)
        {
            for (const auto& mat : recipe->materials)
            {
                if (m_invSystem->CountItem(inv, mat.itemDefId) < mat.count)
                    return false;
            }
            if (m_invSystem->IsFull(inv))
                return false;
        }
        return true;
    }

    bool MMOCraftingSystem::StartCraft(CraftingState& crafter, InventoryData& inv, uint32_t recipeId)
    {
        if (crafter.isCrafting || !CanCraft(crafter, inv, recipeId))
            return false;

        const auto* recipe = GetRecipe(recipeId);

        // Consume materials
        if (m_invSystem)
        {
            for (const auto& mat : recipe->materials)
                m_invSystem->RemoveItem(inv, mat.itemDefId, mat.count);
        }

        crafter.isCrafting = true;
        crafter.activeRecipeId = recipeId;
        crafter.craftProgress = 0.0f;
        return true;
    }

    void MMOCraftingSystem::CancelCraft(CraftingState& crafter)
    {
        crafter.isCrafting = false;
        crafter.activeRecipeId = 0;
        crafter.craftProgress = 0.0f;
    }

    void MMOCraftingSystem::Update(float dt, CraftingState& crafter, InventoryData& inv)
    {
        if (!crafter.isCrafting)
            return;

        const auto* recipe = GetRecipe(crafter.activeRecipeId);
        if (!recipe)
        {
            CancelCraft(crafter);
            return;
        }

        crafter.craftProgress += dt / recipe->craftTime;
        if (crafter.craftProgress >= 1.0f)
        {
            CompleteCraft(crafter, inv);
            crafter.isCrafting = false;
            crafter.activeRecipeId = 0;
            crafter.craftProgress = 0.0f;
        }
    }

    void MMOCraftingSystem::CompleteCraft(CraftingState& crafter, InventoryData& inv)
    {
        const auto* recipe = GetRecipe(crafter.activeRecipeId);
        if (!recipe)
            return;

        if (m_invSystem)
            m_invSystem->AddItem(inv, recipe->resultItemId, recipe->resultCount);

        AwardSkillXP(crafter, recipe->discipline, recipe->skillXP);

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[MMO] Crafted: " + recipe->name);
    }

    void MMOCraftingSystem::AwardSkillXP(CraftingState& crafter, CraftingDiscipline disc, int xp)
    {
        int key = static_cast<int>(disc);
        auto& skill = crafter.skills[key];
        if (skill.discipline != disc)
        {
            skill.discipline = disc;
            skill.level = 1;
            skill.currentXP = 0;
        }
        if (skill.level >= skill.maxLevel)
            return;

        skill.currentXP += xp;
        while (skill.currentXP >= skill.GetXPForNext() && skill.level < skill.maxLevel)
        {
            skill.currentXP -= skill.GetXPForNext();
            skill.level++;
        }
    }

    void MMOCraftingSystem::LearnRecipe(CraftingState& crafter, uint32_t recipeId)
    {
        if (!KnowsRecipe(crafter, recipeId) && GetRecipe(recipeId))
            crafter.knownRecipes.push_back(recipeId);
    }

    bool MMOCraftingSystem::KnowsRecipe(const CraftingState& crafter, uint32_t recipeId) const
    {
        return std::find(crafter.knownRecipes.begin(), crafter.knownRecipes.end(), recipeId) !=
               crafter.knownRecipes.end();
    }

    int MMOCraftingSystem::GetSkillLevel(const CraftingState& crafter, CraftingDiscipline disc) const
    {
        auto it = crafter.skills.find(static_cast<int>(disc));
        return it != crafter.skills.end() ? it->second.level : 0;
    }

    std::string MMOCraftingSystem::GetStatusString(const CraftingState& crafter) const
    {
        std::ostringstream ss;
        ss << "Recipes known: " << crafter.knownRecipes.size() << "\n";
        ss << "Crafting: " << (crafter.isCrafting ? "Yes" : "No") << "\n";
        if (!crafter.skills.empty())
        {
            ss << "Skills:\n";
            for (const auto& [k, s] : crafter.skills)
                ss << "  Discipline " << k << ": Lv" << s.level << "\n";
        }
        return ss.str();
    }

    void MMOCraftingSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Crafting System"))
        {
            ImGui::Text("Recipes: %zu", m_recipes.size());
            if (ImGui::TreeNode("Recipe List"))
            {
                for (const auto& [id, r] : m_recipes)
                    ImGui::Text("[%u] %s (Skill Lv%d, %.1fs)", id, r.name.c_str(), r.requiredSkillLevel, r.craftTime);
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
