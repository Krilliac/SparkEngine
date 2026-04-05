/**
 * @file SubsystemConsoleCommandsExt.cpp
 * @brief Console commands for input, scripting, cinematic, replay, localization,
 *        destruction, dialogue, and settings-driven subsystem control
 *
 * Part 2 of subsystem console command registration. Part 1 is in
 * SubsystemConsoleCommands.cpp (camera, network, AI, animation, weather, post-processing).
 */

#include "EngineContext.h"
#include "EngineSettings.h"
#include "Camera/SparkEngineCamera.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Localization/LocalizationSystem.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/Scripting/ScriptHotReload.h"
#include "Input/InputManager.h"
#include "Utils/SparkConsole.h"

#include <sstream>
#include <string>

namespace Spark
{

    // ========================================================================
    // Input commands
    // ========================================================================

    static void RegisterInputCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "input_sensitivity",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_sensitivity <value>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                try
                {
                    input->Console_SetMouseSensitivity(std::stof(args[0]));
                    return "Mouse sensitivity set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set mouse sensitivity", "Input");

        console.RegisterCommand(
            "input_invert_y",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_invert_y <on|off>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                bool invert = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                input->Console_SetInvertMouseY(invert);
                return invert ? "Y-axis inverted" : "Y-axis normal";
            },
            "Toggle mouse Y-axis inversion", "Input");

        console.RegisterCommand(
            "input_raw_mouse",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_raw_mouse <on|off>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                bool raw = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                input->Console_SetRawMouseInput(raw);
                return raw ? "Raw mouse input enabled" : "Raw mouse input disabled";
            },
            "Toggle raw mouse input", "Input");

        console.RegisterCommand(
            "input_accel",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_accel <on|off>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                bool accel = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                input->Console_SetMouseAcceleration(accel);
                return accel ? "Mouse acceleration enabled" : "Mouse acceleration disabled";
            },
            "Toggle mouse acceleration", "Input");

        console.RegisterCommand(
            "input_deadzone",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_deadzone <value>  (0.0-1.0)";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                try
                {
                    input->Console_SetMouseDeadZone(std::stof(args[0]));
                    return "Mouse dead zone set to " + args[0];
                }
                catch (const std::exception&)
                {
                    return "Invalid number";
                }
            },
            "Set mouse dead zone threshold", "Input");

        console.RegisterCommand(
            "input_bind",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "Usage: input_bind <action> <key>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                bool ok = input->Console_BindKey(args[0], args[1]);
                return ok ? "Bound " + args[0] + " to " + args[1] : "Failed to bind key";
            },
            "Bind a key to an action", "Input");

        console.RegisterCommand(
            "input_unbind",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_unbind <action>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                input->Console_UnbindKey(args[0]);
                return "Unbound action: " + args[0];
            },
            "Unbind an action", "Input");

        console.RegisterCommand(
            "input_bindings",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                return input->Console_ListKeyBindings();
            },
            "List all key bindings", "Input");

        console.RegisterCommand(
            "input_logging",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: input_logging <on|off>";
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                bool log = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                input->Console_SetInputLogging(log);
                return log ? "Input logging enabled" : "Input logging disabled";
            },
            "Toggle input event logging", "Input");

        console.RegisterCommand(
            "input_metrics",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                auto m = input->Console_GetMetrics();
                std::stringstream ss;
                ss << "=== Input Metrics ===\n"
                   << "  Key presses: " << m.keyPressCount << "\n"
                   << "  Mouse presses: " << m.mousePressCount << "\n"
                   << "  Active keys: " << m.activeKeys << "\n"
                   << "  Bindings: " << m.totalKeyBindings << "\n"
                   << "  Sensitivity: " << m.mouseSensitivity << "\n"
                   << "  Mouse captured: " << (m.mouseCaptured ? "YES" : "NO") << "\n";
                return ss.str();
            },
            "Show input system metrics", "Input");

        console.RegisterCommand(
            "input_reset",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
                if (!input)
                    return "Input manager not available";
                input->Console_ResetToDefaults();
                return "Input settings reset to defaults";
            },
            "Reset input settings to defaults", "Input");
    }

    // ========================================================================
    // Scripting commands
    // ========================================================================

    static void RegisterScriptingCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "script_hotreload_status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* ctx = EngineContext::Get();
                if (!ctx)
                    return "Engine context not available";
                auto* hotReload = ctx->GetSystem<Scripting::ScriptHotReloadManager>();
                if (!hotReload)
                    return "Script hot-reload not available";
                return hotReload->Console_GetStatus();
            },
            "Show script hot-reload status and watched files", "Scripting");
    }

    // ========================================================================
    // Cinematic commands
    // ========================================================================

    static void RegisterCinematicCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "cinematic_list",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* cin = EngineContext::Get() ? EngineContext::Get()->GetCinematic() : nullptr;
                if (!cin)
                    return "Cinematic system not available";
                return cin->Console_ListSequences();
            },
            "List all cinematic sequences", "Cinematic");

        console.RegisterCommand(
            "cinematic_info",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: cinematic_info <sequence_name>";
                auto* cin = EngineContext::Get() ? EngineContext::Get()->GetCinematic() : nullptr;
                if (!cin)
                    return "Cinematic system not available";
                return cin->Console_GetSequenceInfo(args[0]);
            },
            "Show details of a cinematic sequence", "Cinematic");
    }

    // ========================================================================
    // Replay commands
    // ========================================================================

    static void RegisterReplayCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "replay_status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* replay = EngineContext::Get() ? EngineContext::Get()->GetReplay() : nullptr;
                if (!replay)
                    return "Replay system not available";
                return replay->Console_GetStatus();
            },
            "Show replay system status", "Replay");
    }

    // ========================================================================
    // Localization commands
    // ========================================================================

    static void RegisterLocalizationCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "locale_status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* loc = EngineContext::Get() ? EngineContext::Get()->GetLocalization() : nullptr;
                if (!loc)
                    return "Localization system not available";
                return loc->Console_GetStatus();
            },
            "Show localization system status and current language", "Localization");

        console.RegisterCommand(
            "locale_keys",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* loc = EngineContext::Get() ? EngineContext::Get()->GetLocalization() : nullptr;
                if (!loc)
                    return "Localization system not available";
                return loc->Console_ListKeys();
            },
            "List all localization keys", "Localization");
    }

    // ========================================================================
    // Destruction commands
    // ========================================================================

    static void RegisterDestructionCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "destruction_status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* dest = EngineContext::Get() ? EngineContext::Get()->GetDestruction() : nullptr;
                if (!dest)
                    return "Destruction system not available";
                return dest->Console_GetStatus();
            },
            "Show destruction system status", "Destruction");
    }

    // ========================================================================
    // Dialogue commands
    // ========================================================================

    static void RegisterDialogueCommands(SimpleConsole& console)
    {
        console.RegisterCommand(
            "dialogue_status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* dlg = EngineContext::Get() ? EngineContext::Get()->GetDialogue() : nullptr;
                if (!dlg)
                    return "Dialogue system not available";
                return dlg->Console_GetStatus();
            },
            "Show dialogue system status", "Dialogue");

        console.RegisterCommand(
            "dialogue_list",
            [](const std::vector<std::string>&) -> std::string
            {
                auto* dlg = EngineContext::Get() ? EngineContext::Get()->GetDialogue() : nullptr;
                if (!dlg)
                    return "Dialogue system not available";
                return dlg->Console_ListTrees();
            },
            "List all dialogue trees", "Dialogue");
    }

    // ========================================================================
    // Settings shortcut commands
    // ========================================================================

    static void RegisterSettingsShortcutCommands(SimpleConsole& console)
    {
        // Quick-access commands that map to commonly tweaked settings
        console.RegisterCommand(
            "ui_scale",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "UI Scale: " + EngineSettings::GetInstance().GetValue("UI", "UIScale");
                EngineSettings::GetInstance().SetValue("UI", "UIScale", args[0]);
                return "UI scale set to " + args[0];
            },
            "Get/set UI scale factor", "UI");

        console.RegisterCommand(
            "hud",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: hud <on|off>";
                bool show = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                EngineSettings::GetInstance().SetValue("UI", "ShowHUD", show ? "true" : "false");
                return show ? "HUD shown" : "HUD hidden";
            },
            "Toggle HUD visibility", "UI");

        console.RegisterCommand(
            "crosshair",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: crosshair <on|off>";
                bool show = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                EngineSettings::GetInstance().SetValue("UI", "ShowCrosshair", show ? "true" : "false");
                return show ? "Crosshair shown" : "Crosshair hidden";
            },
            "Toggle crosshair visibility", "UI");

        console.RegisterCommand(
            "subtitles",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Usage: subtitles <on|off>";
                bool show = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                EngineSettings::GetInstance().SetValue("UI", "ShowSubtitles", show ? "true" : "false");
                return show ? "Subtitles enabled" : "Subtitles disabled";
            },
            "Toggle subtitle display", "UI");

        console.RegisterCommand(
            "fps_cap",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Target FPS: " + EngineSettings::GetInstance().GetValue("Performance", "TargetFPS") +
                           " (0=uncapped)";
                EngineSettings::GetInstance().SetValue("Performance", "TargetFPS", args[0]);
                return "Target FPS set to " + args[0] + (args[0] == "0" ? " (uncapped)" : "");
            },
            "Get/set target frame rate (0=uncapped)", "Performance");

        console.RegisterCommand(
            "draw_distance",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Draw distance: " + EngineSettings::GetInstance().GetValue("Streaming", "DrawDistance");
                EngineSettings::GetInstance().SetValue("Streaming", "DrawDistance", args[0]);
                return "Draw distance set to " + args[0];
            },
            "Get/set draw distance", "Streaming");

        console.RegisterCommand(
            "shadow_distance",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Shadow distance: " +
                           EngineSettings::GetInstance().GetValue("Streaming", "ShadowDrawDistance");
                EngineSettings::GetInstance().SetValue("Streaming", "ShadowDrawDistance", args[0]);
                return "Shadow draw distance set to " + args[0];
            },
            "Get/set shadow draw distance", "Streaming");

        console.RegisterCommand(
            "lod_bias",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "LOD bias: " + EngineSettings::GetInstance().GetValue("Streaming", "LODBias");
                EngineSettings::GetInstance().SetValue("Streaming", "LODBias", args[0]);
                return "LOD bias set to " + args[0];
            },
            "Get/set LOD distance bias (0=default, positive=lower detail)", "Streaming");

        console.RegisterCommand(
            "difficulty",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Difficulty: " + EngineSettings::GetInstance().GetValue("Game", "Difficulty");
                EngineSettings::GetInstance().SetValue("Game", "Difficulty", args[0]);
                return "Difficulty set to " + args[0];
            },
            "Get/set game difficulty", "Game");

        console.RegisterCommand(
            "fov",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "FOV: " + EngineSettings::GetInstance().GetValue("Game", "FieldOfView");
                EngineSettings::GetInstance().SetValue("Game", "FieldOfView", args[0]);
                // Also update the live camera
                auto* cam = EngineContext::Get() ? EngineContext::Get()->GetCamera() : nullptr;
                if (cam)
                    cam->Console_SetFOV(std::stof(args[0]));
                return "FOV set to " + args[0];
            },
            "Get/set field of view", "Game");

        console.RegisterCommand(
            "show_fps",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "Show FPS: " + EngineSettings::GetInstance().GetValue("Game", "ShowFPS");
                bool show = (args[0] == "on" || args[0] == "true" || args[0] == "1");
                EngineSettings::GetInstance().SetValue("Game", "ShowFPS", show ? "true" : "false");
                return show ? "FPS counter shown" : "FPS counter hidden";
            },
            "Toggle FPS counter", "Game");

        console.RegisterCommand(
            "volume",
            [](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                {
                    auto& s = EngineSettings::GetInstance();
                    std::stringstream ss;
                    ss << "Master: " << s.GetValue("Audio", "MasterVolume")
                       << "  SFX: " << s.GetValue("Audio", "SFXVolume")
                       << "  Music: " << s.GetValue("Audio", "MusicVolume")
                       << "  Voice: " << s.GetValue("Audio", "VoiceVolume");
                    return ss.str();
                }
                if (args.size() < 2)
                    return "Usage: volume <master|sfx|music|voice> <0.0-1.0>";

                static const std::unordered_map<std::string, std::string> keys = {{"master", "MasterVolume"},
                                                                                  {"sfx", "SFXVolume"},
                                                                                  {"music", "MusicVolume"},
                                                                                  {"voice", "VoiceVolume"}};
                auto it = keys.find(args[0]);
                if (it == keys.end())
                    return "Unknown channel. Options: master, sfx, music, voice";
                EngineSettings::GetInstance().SetValue("Audio", it->second, args[1]);
                return args[0] + " volume set to " + args[1];
            },
            "Get/set audio volume (master/sfx/music/voice)", "Audio");
    }

    // ========================================================================
    // Public API
    // ========================================================================

    void RegisterExtendedSubsystemConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        RegisterInputCommands(console);
        RegisterScriptingCommands(console);
        RegisterCinematicCommands(console);
        RegisterReplayCommands(console);
        RegisterLocalizationCommands(console);
        RegisterDestructionCommands(console);
        RegisterDialogueCommands(console);
        RegisterSettingsShortcutCommands(console);
    }

} // namespace Spark
