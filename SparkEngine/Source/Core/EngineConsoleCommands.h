/**
 * @file EngineConsoleCommands.h
 * @brief Registers engine-wide console commands (modules, physics, audio, save, architecture)
 *
 * Extracted from SparkEngine.cpp to keep the entry point focused on initialization
 * and the main loop. Each command category delegates to its respective subsystem.
 */

#pragma once

class AudioEngine;
class ModuleManager;

namespace Spark
{

    /**
     * @brief Register all engine console commands (physics, audio, save, modules, architecture)
     * @param moduleManager Module manager for module_info/module_reload commands (may be null)
     * @param audioEngine Audio engine for audio_* commands (may be null)
     */
    void RegisterEngineConsoleCommands(ModuleManager* moduleManager, AudioEngine* audioEngine);

} // namespace Spark
