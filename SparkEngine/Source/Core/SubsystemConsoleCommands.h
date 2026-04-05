/**
 * @file SubsystemConsoleCommands.h
 * @brief Console commands for subsystems: camera, network, AI, animation, weather,
 *        post-processing, input, scripting, cinematic, replay, localization, destruction, and dialogue
 *
 * Split from EngineConsoleCommands to keep individual files under the 500-line threshold.
 * Each subsystem has its own static registration function.
 */

#pragma once

namespace Spark
{

    /**
     * @brief Register console commands for all engine subsystems
     *
     * Registers commands for: camera, network, AI, animation, weather, post-processing,
     * input, scripting, cinematic, replay, localization, destruction, and dialogue.
     * Called from RegisterEngineConsoleCommands() during engine startup.
     */
    void RegisterSubsystemConsoleCommands();

} // namespace Spark
