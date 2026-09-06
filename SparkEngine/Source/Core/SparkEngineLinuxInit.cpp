/**
 * @file SparkEngineLinuxInit.cpp
 * @brief Shared POSIX startup, per-frame tick, and shutdown helpers (Linux + macOS).
 *
 * Split from SparkEngineLinux.cpp to keep files under the ~500-line guideline.
 * Contains module discovery, core-subsystem initialization, gameplay subsystem
 * registration, the common TickFrame, and the common shutdown sequence used by
 * the headless, SDL2 windowed, and no-SDL2 fallback paths. The main() entry
 * point stays in SparkEngineLinux.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "SparkEngineLinuxInternal.h"
#include "SparkEngineMacOS.h"
#include "RuntimePackage.h"
#include "EngineRuntime.h"
#include "ModuleManager.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "FaultIsolation.h" // SPARK_GUARDED_UPDATE / SubsystemFaultIsolator (mirrors SparkEngineWindows.cpp)
#include "EngineConsoleCommands.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/GraphicsConsoleCommands.h"
#include "Input/InputManager.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioBackendFactory.h"
#include "Utils/SparkConsole.h"
#include "Utils/ConsoleProcessManager.h"
#include "EngineSetup.h"
#include "Engine/ECS/Components.h" // ::World — engine-owned ECS world service
#include "AssetIntegration.h"
#include "GameplaySystemLifecycle.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "ModuleHotReload.h"
#include "Graphics/Neural/NeuralInference.h"
#include "Utils/Logger.h"
#include "Utils/LogMacros.h" // SPARK_LOG_*
#include "Utils/Assert.h"
#include "Utils/FreezeDetector.h" // SPARK_HEARTBEAT / SPARK_FREEZE_RECOVERY_*
#include "FixedTimestepAccumulator.h"
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#ifndef SPARK_PLATFORM_WINDOWS

static std::filesystem::path GetExecutableDirectoryLinux()
{
    if (auto executableDirectory = Spark::RuntimePackage::GetExecutableDirectory(); !executableDirectory.empty())
        return executableDirectory;
    return std::filesystem::current_path();
}

static bool HasArgument(int argc, char* argv[], std::string_view option)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] && option == argv[i])
            return true;
    return false;
}

static std::optional<std::string> ArgumentValue(int argc, char* argv[], std::string_view option)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (argv[i] && option == argv[i] && argv[i + 1] && argv[i + 1][0] != '\0')
            return std::string(argv[i + 1]);
    return std::nullopt;
}

static bool LoadGameModulesLinux(ModuleManager& manager, int argc, char* argv[])
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const auto exeDir = GetExecutableDirectoryLinux();

    // 1. Check command line for specific module
    if (HasArgument(argc, argv, "-game"))
    {
        const auto module = ArgumentValue(argc, argv, "-game");
        if (!module)
        {
            console.LogError("-game requires a non-empty path");
            return false;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(*module), error) || error)
        {
            console.LogError("Explicit game module not found: " + *module);
            return false;
        }
        return manager.LoadModule(*module);
    }

    // 2. An explicit project manifest wins over the executable-directory fallback.
    if (HasArgument(argc, argv, "-manifest"))
    {
        const auto manifest = ArgumentValue(argc, argv, "-manifest");
        if (!manifest)
        {
            console.LogError("-manifest requires a non-empty path");
            return false;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(*manifest), error) || error)
        {
            console.LogError("Explicit module manifest not found: " + *manifest);
            return false;
        }
        return manager.LoadModulesFromManifest(*manifest);
    }

    // 3. Check for the executable-directory module manifest.
    const auto manifestPath = exeDir / "spark.modules.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(manifestPath, error) && !error)
    {
        const std::u8string utf8 = manifestPath.generic_u8string();
        return manager.LoadModulesFromManifest(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    }

    // 4. Fall back to directory scan.
    const std::u8string utf8 = exeDir.generic_u8string();
    return manager.LoadModulesFromDirectory(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
}


// ===================================================================================
//                    Linux extracted helpers
// ===================================================================================

/**
 * @brief Common per-frame tick logic shared by SDL2 windowed and no-SDL2 fallback modes.
 *
 * Updates input, modules, gameplay/debug systems, and console processing.
 */
void TickFrame(float dt)
{
    SPARK_HEARTBEAT();

    // If the freeze detector requested recovery, skip this frame
    if (SPARK_FREEZE_RECOVERY_REQUESTED())
    {
        SPARK_FREEZE_RECOVERY_ACK();
        return;
    }

    // Advance the global fixed-timestep accumulator for deterministic fixed-rate updates.
    Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

    SPARK_GUARDED_UPDATE("Input", "Core", {
        if (GetEngineRuntime().input)
            GetEngineRuntime().input->Update();
    });

    if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasInitializedModules())
    {
        SPARK_GUARDED_UPDATE("Modules", "Core", {
            GetEngineRuntime().moduleManager->UpdateAll(dt);
            auto& fixedAcc = Spark::FixedTimestepAccumulator::GetInstance();
            const float fixedDt = fixedAcc.GetFixedTimestep();
            for (uint32_t i = fixedAcc.GetFixedStepCount(); i > 0; --i)
                GetEngineRuntime().moduleManager->FixedUpdateAll(fixedDt);
            GetEngineRuntime().moduleManager->RenderAll();
        });
    }
    else if (GetEngineRuntime().graphics)
    {
        GetEngineRuntime().graphics->BeginFrame();
        GetEngineRuntime().graphics->EndFrame();
    }

    if (GetEngineRuntime().moduleHotReload)
        GetEngineRuntime().moduleHotReload->PollChanges();

    // Pump the audio engine: advances source state machine (stops finished
    // sources), applies 3D spatialization, and processes distance attenuation.
    // Pre-existing bug: AudioEngine::Update was never called from the main
    // loop on any platform.
    SPARK_GUARDED_UPDATE("Audio", "Core", {
        if (GetEngineRuntime().audioEngine)
            GetEngineRuntime().audioEngine->Update(dt);
    });

    UpdateGameplaySystems(dt);
    UpdateDebugSystems(dt);
    SPARK_GUARDED_UPDATE("Console", "Core", {
        Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
        Spark::SimpleConsole::GetInstance().Update();
    });
}

/**
 * @brief Register gameplay subsystems (Weather, UI, Dialogue, Modding) with EngineContext.
 *
 * Uses function-local statics so these objects live for the process lifetime
 * without polluting the global namespace.
 */
static void RegisterGameplaySubsystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — gameplay subsystems skipped");
        return;
    }

    static Spark::WeatherSystem s_weatherSystem;
    ctx->SetWeather(&s_weatherSystem);

    // TimeOfDay — singleton, registered with context for game-module access
    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    // Wire WeatherSystem to EventBus for WeatherChangedEvent publishing
    if (GetEngineRuntime().eventBus)
    {
        s_weatherSystem.SetEventBus(GetEngineRuntime().eventBus.get());
    }

    static Spark::UI::UISystem s_uiSystem;
    ctx->SetUI(&s_uiSystem);

    static Spark::DialogueSystem s_dialogueSystem;
    ctx->SetDialogue(&s_dialogueSystem);

    static Spark::ModSystem s_modSystem;
    ctx->SetModSystem(&s_modSystem);
}

/**
 * @brief Initialize engine core subsystems common to all Linux startup paths.
 *
 * Creates EngineContext, physics, core subsystem registration, save system,
 * coroutine scheduler, and gameplay subsystem registration.
 *
 * @param registerGameplay If true, registers Weather/UI/Dialogue/Modding and the windowed AssetRegistry.
 */
void InitLinuxCoreSubsystems(bool registerGameplay)
{
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(GetEngineRuntime().graphics.get(), GetEngineRuntime().input.get(),
                                        GetEngineRuntime().timer.get(), GetEngineRuntime().eventBus.get()));

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null after SetOwned — Linux init aborted");
        return;
    }

    InitPhysics();

    Spark::EngineSetup::RegisterCoreSubsystems(*ctx);
    if (!g_noJobSystem)
    {
        Spark::EngineSetup::InitializeJobSystem(g_maxWorkerThreads);
    }
    else
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "-no-jobsystem: JobSystem worker threads skipped");
    }

    // Same per-user location (and same one-time legacy-saves migration) as the
    // Windows entry points so all paths share saves.
    if (!Spark::SaveSystem::GetInstance().Initialize(Spark::UserPaths::ResolveSaveDirectory()))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    if (!registerGameplay)
    {
        // Dedicated/headless modules still require CPU asset handle lookup and
        // cached file access. Do not create GraphicsEngine/AssetPipeline here.
        GetEngineRuntime().InitializeHeadlessAssetServices(*ctx);
    }

    // ECS world service — parity with InitEngineContext/InitHeadlessEngineContext
    // on Windows: modules reach the ECS only through IEngineContext::GetWorld().
    // Without it, headless/dedicated servers hand modules a null World and every
    // pawn-position read collapses to (0,0,0) (breaks TERRAFRONT region capture).
    // Owned by g_engineEcsWorld (SparkEngine.cpp); ShutdownEngine destroys it
    // after module ShutdownAll but before the module DLLs are unmapped.
    extern std::unique_ptr<::World> g_engineEcsWorld;
    g_engineEcsWorld = std::make_unique<::World>();
    ctx->SetWorld(g_engineEcsWorld.get());

    // AssetPipeline (owned by GraphicsEngine, exposed via EngineContext for SDK access)
    if (GetEngineRuntime().graphics && GetEngineRuntime().graphics->GetAssetPipeline())
    {
        ctx->SetAssetPipeline(GetEngineRuntime().graphics->GetAssetPipeline());
    }

    // Initialize neural inference engine (GPU compute-based, no external ML deps)
    auto& neuralInference = Spark::Graphics::Neural::NeuralInferenceEngine::GetInstance();
    if (!neuralInference.Initialize())
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "NeuralInferenceEngine::Initialize failed — continuing without ML");
    }
    ctx->RegisterSystem<Spark::Graphics::Neural::NeuralInferenceEngine>(&neuralInference);

    if (registerGameplay)
    {
        static Spark::AssetRegistry s_assetRegistry;
        ctx->SetAssetRegistry(&s_assetRegistry);
        RegisterGameplaySubsystems();
    }
}

/**
 * @brief Load game modules, initialize hot-reload watcher, and register console commands.
 *
 * Shared by all Linux startup paths. Handles module loading via LoadGameModulesLinux,
 * hot-reload setup, audio engine init (if windowed), and console command registration.
 *
 * @param argc Argument count from main().
 * @param argv Argument values from main().
 * @param initAudio If true, creates and initializes AudioEngine.
 */
void InitLinuxModulesAndCommands(int argc, char* argv[], bool initAudio)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    if (initAudio)
    {
        GetEngineRuntime().audioEngine = std::make_unique<AudioEngine>();
        if (SUCCEEDED(GetEngineRuntime().audioEngine->Initialize(32)))
        {
            console.LogInfo("AudioEngine initialized (32 sources)");
            if (auto* ctx = EngineContext::Get())
                ctx->SetAudio(GetEngineRuntime().audioEngine.get());
        }
        else
        {
            console.LogWarning("AudioEngine initialization failed");
            GetEngineRuntime().audioEngine.reset();
        }

        // Create cross-platform audio backend (OpenAL on Linux, wraps AudioEngine on Windows)
        GetEngineRuntime().audioBackend = Spark::Audio::CreateAudioBackend(Spark::Audio::AudioBackendType::Auto,
                                                                           GetEngineRuntime().audioEngine.get());
        if (GetEngineRuntime().audioBackend && !GetEngineRuntime().audioBackend->IsAvailable() &&
            !GetEngineRuntime().audioBackend->Initialize(32))
        {
            console.LogWarning("Cross-platform audio backend initialization failed; sequencer audio will be silent");
        }
        Spark::Cinematic::SequencerManager::GetInstance().SetAudioBackend(GetEngineRuntime().audioBackend.get());
    }
    else
    {
        Spark::Cinematic::SequencerManager::GetInstance().SetAudioBackend(nullptr);
    }

    GetEngineRuntime().moduleManager = std::make_unique<ModuleManager>();

    if (LoadGameModulesLinux(*GetEngineRuntime().moduleManager, argc, argv))
    {
        GetEngineRuntime().moduleManager->InitializeAll(EngineContext::Get());
        const size_t initializedModules = GetEngineRuntime().moduleManager->GetInitializedModuleCount();
        console.LogSuccess("Loaded " + std::to_string(initializedModules) + " module(s)");
        if (initializedModules > 0)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "SPARK_MODULE_READY count=%zu", initializedModules);
            std::fprintf(stdout, "SPARK_MODULE_READY count=%zu\n", initializedModules);
            std::fflush(stdout);
        }
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
    }

    // Module hot-reload watcher
    GetEngineRuntime().moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    GetEngineRuntime().moduleHotReload->Initialize(GetEngineRuntime().moduleManager.get(), EngineContext::Get());
    GetEngineRuntime().moduleHotReload->WatchAllLoadedModules();
    GetEngineRuntime().moduleHotReload->Start();

    // Console commands
    if (GetEngineRuntime().graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*GetEngineRuntime().graphics);
    Spark::RegisterEngineConsoleCommands(GetEngineRuntime().moduleManager.get(), GetEngineRuntime().audioEngine.get(),
                                         GetEngineRuntime().moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();

    LogMissingModuleWarnings();
}

/**
 * @brief Common shutdown sequence for all Linux startup paths.
 */
void ShutdownLinuxAfterPreflight()
{
    // Every Linux loop reaches this function only after a successful
    // CanShutdownEngine checkpoint. Commit without a second fallible gate.
    GetEngineRuntime().moduleHotReload.reset();
    Spark::SimpleConsole::GetInstance().LogInfo("Shutting down...");
    ShutdownEngineAfterPreflight();
}

#endif // !SPARK_PLATFORM_WINDOWS
