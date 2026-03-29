/**
 * @file GameplaySystemLifecycle.h
 * @brief Initialization, update, and shutdown for all gameplay and debug subsystems
 *
 * Extracted from SparkEngine.cpp to reduce file size. These functions are called
 * by the platform-specific entry points (wWinMain / main) during engine startup,
 * the per-frame update loop, and shutdown.
 */

#pragma once

/**
 * @brief Log warnings about missing optional modules (Bullet, miniz, SDL2)
 */
void LogMissingModuleWarnings();

/**
 * @brief Initialize debug/diagnostic systems (Logger, ChromeTracing, MemoryDebugger, etc.)
 */
void InitDebugSystems();

/**
 * @brief Initialize all gameplay subsystems (AI, animation, physics integration, etc.)
 *
 * Requires EngineContext to be set up with EventBus and World.
 */
void InitGameplaySystems();

/**
 * @brief Per-frame update for all gameplay subsystems
 *
 * Updates non-ECS systems (Weather, Dialogue, UI), ECS-dependent systems
 * (AbilitySystem, Destruction, AI), clustered lighting, extended systems
 * (SeamlessArea, Tween, Cinematic, Replay), and the ECS executor.
 *
 * @param dt Delta time in seconds
 */
void UpdateGameplaySystems(float dt);

/**
 * @brief Per-frame update for debug/diagnostic systems
 *
 * Updates TweenManager, DebugDraw, DebugOverlay, MemoryMonitor, and DecalSystem.
 *
 * @param dt Delta time in seconds
 */
void UpdateDebugSystems(float dt);

/**
 * @brief Shut down all gameplay subsystems in reverse initialization order
 */
void ShutdownGameplaySystems();

/**
 * @brief Shut down all debug/diagnostic systems
 */
void ShutdownDebugSystems();

/**
 * @brief Get the current gameplay frame counter (for debug hooks at shutdown)
 */
uint64_t GetGameplayFrameCount();
