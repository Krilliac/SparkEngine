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

#include <string>
#include <vector>
#include <memory>

namespace SparkGameFPS
{
    class EngineWeatherAdapter;
}

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

    Spark::IEngineContext* m_context{nullptr};
    std::unique_ptr<SparkGameFPS::EngineWeatherAdapter> m_weatherAdapter;
    std::vector<std::string> m_registeredConsoleCommands;
    bool m_initialized{false};
};

// Installed SDK module exports consumed by ModuleManager.
extern "C"
{
    SPARK_MODULE_API Spark::IModule* CreateModule();
    SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
}
