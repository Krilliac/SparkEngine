/**
 * @file TestUnwiredSystemRemoval.cpp
 * @brief Pins the removal of two built-but-unwired engine paths found by security triage
 *
 * 1. MaterialSystem / Material carried a file import, export, reload and hot-reload surface
 *    (LoadMaterial, ReloadMaterial, Console_ImportMaterial, Material::LoadFromFile,
 *    Material::LoadTexture, ...) that nothing in the engine, editor or game modules called. Its
 *    texture and material paths had no asset-root containment, so it was deleted rather than
 *    wired in. Materials are created in memory and receive shader resource views through
 *    Material::SetTexture(). The concept checks below fail to hold if any of those entry points
 *    comes back.
 * 2. LifecycleSystem fired a death callback that production never registered, so every death
 *    only logged a warning. The callback is gone and the system latches
 *    HealthComponent::deathProcessed. HealthComponent::SetHealth() re-arms the latch when an
 *    authoritative health write brings an entity back, which the MMOFPS pawn and vehicle
 *    replication paths now use instead of clearing isDead by hand.
 */

#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Systems/ECSystems.h"
#include "Graphics/MaterialSystem.h"

#include <string>

namespace
{
    // -- MaterialSystem file surface ----------------------------------------------------------

    template <typename T>
    concept HasLoadMaterial = requires(T& system, const std::string& path) { system.LoadMaterial(path); };

    template <typename T>
    concept HasReloadMaterialByName = requires(T& system, const std::string& name) { system.ReloadMaterial(name); };

    template <typename T>
    concept HasReloadAllMaterials = requires(T& system) { system.ReloadAllMaterials(); };

    template <typename T>
    concept HasTextureLoadByPath = requires(T& system, const std::string& path) { system.LoadTexture(path); };

    template <typename T>
    concept HasHotReloadToggle = requires(T& system) { system.EnableHotReloading(true); };

    template <typename T>
    concept HasConsoleImport = requires(T& system, const std::string& path) { system.Console_ImportMaterial(path); };

    template <typename T>
    concept HasConsoleExport = requires(T& system, const std::string& name, const std::string& path) {
        system.Console_ExportMaterial(name, path);
    };

    template <typename T>
    concept HasConsoleReload = requires(T& system, const std::string& name) {
        system.Console_ReloadMaterial(name);
        system.Console_ReloadAllMaterials();
    };

    template <typename T>
    concept HasConsoleTextureSlotLoad =
        requires(T& system, const std::string& text) { system.Console_LoadTextureToSlot(text, text, text); };

    // -- Material file surface ----------------------------------------------------------------

    template <typename T>
    concept HasMaterialLoadFromFile =
        requires(T& material, const std::string& path, ID3D11Device* device) { material.LoadFromFile(path, device); };

    template <typename T>
    concept HasMaterialSaveToFile = requires(const T& material, const std::string& path) { material.SaveToFile(path); };

    template <typename T>
    concept HasMaterialTextureLoadByPath = requires(T& material, MaterialTextureType type, const std::string& path,
                                                    ID3D11Device* device) { material.LoadTexture(type, path, device); };

    template <typename T>
    concept HasMaterialReload = requires(T& material, ID3D11Device* device) {
        material.ReloadMaterial(device);
        material.Console_ReloadTextures(device);
    };

    // -- LifecycleSystem ----------------------------------------------------------------------

    template <typename T>
    concept HasDeathCallback = requires(T& system) { system.SetDeathCallback(nullptr); };

    EntityID SpawnWithHealth(World& world, float health)
    {
        const EntityID entity = world.CreateEntity();
        world.AddComponent<HealthComponent>(entity, HealthComponent{health, 100.0f, false, false});
        return entity;
    }
} // namespace

TEST(UnwiredSys_MaterialSystemHasNoFileImportSurface)
{
    EXPECT_FALSE(HasLoadMaterial<MaterialSystem>);
    EXPECT_FALSE(HasReloadMaterialByName<MaterialSystem>);
    EXPECT_FALSE(HasReloadAllMaterials<MaterialSystem>);
    EXPECT_FALSE(HasTextureLoadByPath<MaterialSystem>);
    EXPECT_FALSE(HasHotReloadToggle<MaterialSystem>);
    EXPECT_FALSE(HasConsoleImport<MaterialSystem>);
    EXPECT_FALSE(HasConsoleExport<MaterialSystem>);
    EXPECT_FALSE(HasConsoleReload<MaterialSystem>);
    EXPECT_FALSE(HasConsoleTextureSlotLoad<MaterialSystem>);

    EXPECT_FALSE(HasMaterialLoadFromFile<Material>);
    EXPECT_FALSE(HasMaterialSaveToFile<Material>);
    EXPECT_FALSE(HasMaterialTextureLoadByPath<Material>);
    EXPECT_FALSE(HasMaterialReload<Material>);

    // The in-memory path the engine does use is still there: a created material is found by name.
    MaterialSystem system;
    auto created = system.CreateMaterial("UnwiredSysMaterial");
    ASSERT_TRUE(created != nullptr);
    EXPECT_TRUE(system.GetMaterial("UnwiredSysMaterial") == created);
}

TEST(UnwiredSys_LifecycleLatchesEachDeathOnce)
{
    EXPECT_FALSE(HasDeathCallback<Spark::ECS::LifecycleSystem>);

    World world;
    const EntityID dying = SpawnWithHealth(world, 10.0f);
    const EntityID living = SpawnWithHealth(world, 100.0f);
    world.GetComponent<HealthComponent>(dying)->TakeDamage(25.0f);

    Spark::ECS::LifecycleSystem lifecycle;
    lifecycle.Update(world, 0.016f);

    const HealthComponent* dead = world.GetComponent<HealthComponent>(dying);
    const HealthComponent* alive = world.GetComponent<HealthComponent>(living);
    ASSERT_TRUE(dead != nullptr);
    ASSERT_TRUE(alive != nullptr);
    EXPECT_TRUE(dead->isDead);
    EXPECT_TRUE(dead->deathProcessed);
    EXPECT_FALSE(alive->isDead);
    EXPECT_FALSE(alive->deathProcessed);

    // A second update sees nothing new and leaves both flags as they are.
    lifecycle.Update(world, 0.016f);
    EXPECT_TRUE(world.GetComponent<HealthComponent>(dying)->deathProcessed);
    EXPECT_FALSE(world.GetComponent<HealthComponent>(living)->deathProcessed);
}

TEST(UnwiredSys_SetHealthRearmsDeathLatch)
{
    World world;
    const EntityID pawn = SpawnWithHealth(world, 100.0f);
    Spark::ECS::LifecycleSystem lifecycle;

    // Server-side kill: an authoritative zero marks the pawn dead and the lifecycle latches it.
    world.GetComponent<HealthComponent>(pawn)->SetHealth(0.0f);
    lifecycle.Update(world, 0.016f);
    HealthComponent* health = world.GetComponent<HealthComponent>(pawn);
    ASSERT_TRUE(health != nullptr);
    EXPECT_TRUE(health->isDead);
    EXPECT_TRUE(health->deathProcessed);

    // A repeated zero write (a replication snapshot of the same death) keeps the latch set.
    health->SetHealth(0.0f);
    EXPECT_TRUE(health->deathProcessed);

    // Respawn: a positive write revives the pawn and clears the latch with isDead, so the
    // deathProcessed-implies-isDead invariant InvalidStateDetector checks keeps holding.
    health->SetHealth(75.0f);
    EXPECT_FALSE(health->isDead);
    EXPECT_FALSE(health->deathProcessed);
    EXPECT_EQ(health->health, 75.0f);

    // The next death is latched again rather than silently skipped.
    health->TakeDamage(200.0f);
    EXPECT_TRUE(health->isDead);
    EXPECT_FALSE(health->deathProcessed);
    lifecycle.Update(world, 0.016f);
    EXPECT_TRUE(world.GetComponent<HealthComponent>(pawn)->deathProcessed);
}
