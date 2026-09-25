/**
 * @file SparkGameFPS.h
 * @brief SparkGameFPS module - Spark Engine FPS showcase
 * @author Spark Engine Team
 * @date 2025
 *
 * SparkGameFPS is the FPS showcase module loaded as a DLL at runtime.
 * It demonstrates all major engine subsystems: rendering, physics, AI,
 * animation, audio, networking, ECS, vehicles, and class-based FPS gameplay.
 *
 * Implements the installed SDK's Spark::IModule interface.
 */

#pragma once

#include "Spark/SparkSDK.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace SparkGameFPS
{
    class EngineWeatherAdapter;
}

namespace Spark
{
    class GameMode;
    class RespawnSystem;
} // namespace Spark

// Forward declarations
class Game;

// Global game pointer shared between SparkGame and SparkEngineLib (SparkConsole).
// Raw pointer — owned by SparkGameModule, set during Initialize, cleared during Shutdown.
// Not exported as unique_ptr to avoid ABI/CRT mismatch across DLL boundaries.
extern SPARK_GAME_API Game* g_game;

/**
 * @brief Game module implementation for SparkGame
 *
 * Receives all engine services through the injected public IEngineContext.
 */
class SparkGameModule : public Spark::IModule
{
  public:
    SparkGameModule();
    ~SparkGameModule() override;

    Spark::ModuleInfo GetModuleInfo() const override;
    bool OnLoad(Spark::IEngineContext* context) override;
    void OnUnload() override;
    void OnUpdate(float deltaTime) override;
    void OnFixedUpdate(float fixedDeltaTime) override;
    void OnRender() override;
    void OnResize(int width, int height) override;
    void OnPause() override;
    void OnResume() override;
    void OnImGui() override;

  private:
    bool InitializeFromContext();
    void Shutdown();
    void RegisterGameConsoleCommands();

    /**
     * @brief Load the authored arena for the no-render headless lifecycle.
     *
     * Parses Scenes/level1.scene through the data-only SceneManager path (no
     * GraphicsEngine or InputManager), binds the RespawnSystem and GameMode to
     * its authored default spawns and starts a Deathmatch match that OnUpdate
     * ticks. Fails closed when the scene or its spawns are unusable.
     */
    bool LoadHeadlessArena();

    /**
     * @brief Print the SPARK_FPS_HEADLESS_ARENA record and release the arena.
     *
     * Emitted once, only when LoadHeadlessArena() succeeded, so the record is
     * evidence of a real scene load plus the ticks that followed it.
     */
    void ShutdownHeadlessArena();

    Spark::IEngineContext* m_context{nullptr};
    std::unique_ptr<SparkGameFPS::EngineWeatherAdapter> m_weatherAdapter;
    std::vector<std::string> m_registeredConsoleCommands;
    bool m_initialized{false};

    // Headless arena simulation state (null outside the headless lifecycle).
    std::unique_ptr<Spark::RespawnSystem> m_headlessRespawn;
    std::unique_ptr<Spark::GameMode> m_headlessMode;
    int m_headlessArenaObjects{0};
    int m_headlessArenaSpawns{0};
    int m_headlessArenaBoundSpawns{0};
    std::uint64_t m_headlessArenaTicks{0};
};

// Installed SDK module exports consumed by ModuleManager.
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
