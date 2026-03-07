/**
 * @file SparkGame.h
 * @brief SparkGame module - implements IGameModule for the engine to load
 * @author Spark Engine Team
 * @date 2025
 *
 * SparkGame is a DLL that the SparkEngine executable loads at runtime.
 * It implements the IGameModule interface, wrapping the Game class and
 * all game-specific logic. The engine drives the game loop through this
 * interface.
 */

#pragma once

#include "Core/IGameModule.h"
#include "Core/SparkExport.h"
#include <memory>

// Forward declarations
class Game;
class Console;

/**
 * @brief Game module implementation for SparkGame
 *
 * Wraps the existing Game class behind the IGameModule interface so the
 * engine can load and drive it dynamically.
 */
class SparkGameModule : public IGameModule
{
public:
    SparkGameModule();
    ~SparkGameModule() override;

    const char* GetGameName() const override;
    const char* GetGameVersion() const override;

    bool Initialize(GraphicsEngine* graphics, InputManager* input) override;
    void Shutdown() override;
    void Update(float deltaTime) override;
    void Render() override;
    void OnResize(int width, int height) override;
    void Pause() override;
    void Resume() override;
    bool IsPaused() const override;

private:
    void RegisterGameConsoleCommands();

    std::unique_ptr<Game> m_game;
    std::unique_ptr<Console> m_console;
    bool m_initialized{ false };
};

// DLL exports - these are what the engine looks for when loading the game DLL
extern "C"
{
    SPARK_GAME_API IGameModule* CreateGameModule();
    SPARK_GAME_API void DestroyGameModule(IGameModule* module);
}
