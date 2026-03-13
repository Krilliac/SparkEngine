/**
 * @file SparkGame.h
 * @brief SparkGame module - Spark Engine FPS showcase
 * @author Spark Engine Team
 * @date 2025
 *
 * SparkGame is the initial engine showcase module loaded as a DLL at runtime.
 * It demonstrates all major engine subsystems: rendering, physics, AI,
 * animation, audio, networking, ECS, vehicles, and class-based FPS gameplay.
 *
 * Implements both the new IModule interface (via Spark::IModule) and
 * the legacy IGameModule interface for backward compatibility.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include "Core/IGameModule.h"
#include <memory>

// Forward declarations
class Game;
class Console;

// Global game pointer shared between SparkGame and SparkEngineLib (SparkConsole)
extern SPARK_GAME_API std::unique_ptr<Game> g_game;

/**
 * @brief Game module implementation for SparkGame
 *
 * Implements both the new Spark::IModule interface and the legacy IGameModule.
 * The new interface receives an IEngineContext; the legacy interface receives
 * individual system pointers. Both paths ultimately initialize the same Game.
 */
class SparkGameModule : public Spark::IModule, public IGameModule
{
  public:
    SparkGameModule();
    ~SparkGameModule() override;

    // --- Spark::IModule interface (new) ---
    Spark::ModuleInfo GetModuleInfo() const override;
    bool OnLoad(Spark::IEngineContext* context) override;
    void OnUnload() override;
    void OnUpdate(float deltaTime) override;
    void OnRender() override;
    void OnResize(int width, int height) override;

    // --- IGameModule interface (legacy) ---
    const char* GetGameName() const override;
    const char* GetGameVersion() const override;
    bool Initialize(GraphicsEngine* graphics, InputManager* input) override;
    void Shutdown() override;
    void Update(float deltaTime) override;
    void Render() override;
    // OnResize is shared via override above
    void Pause() override;
    void Resume() override;
    bool IsPaused() const override;

  private:
    void RegisterGameConsoleCommands();

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
};

// New module exports (preferred by ModuleManager)
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}

// Legacy exports (backward compatibility)
extern "C"
{
    SPARK_GAME_API IGameModule* CreateGameModule();
    SPARK_GAME_API void DestroyGameModule(IGameModule* module);
}
