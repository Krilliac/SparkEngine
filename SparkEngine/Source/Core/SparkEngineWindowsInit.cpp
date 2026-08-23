/**
 * @file SparkEngineWindowsInit.cpp
 * @brief Windowed-mode subsystem initialization (Windows).
 *
 * Split from SparkEngineWindows.cpp to keep files under the ~500-line guideline.
 * Contains the engine-context, gameplay-subsystem, module, audio, ImGui-overlay,
 * and console-command initialization run after the Win32 window is created.
 * Linux counterpart lives in SparkEngineLinuxInit.cpp. The wWinMain entry point
 * stays in SparkEngineWindows.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "framework.h"
#include "SparkEngineWindowsInternal.h"
#include "AssetIntegration.h"
#include "Audio/AudioBackendFactory.h"
#include "Audio/AudioEngine.h"
#include "Core/AssetValidator.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/ECS/Components.h" // ::World — engine-owned ECS world service
#include "Engine/Events/EventSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/UI/UISystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "EngineConsoleCommands.h"
#include "EngineContext.h"
#include "EngineRuntime.h"
#include "EngineSettings.h"
#include "EngineSetup.h"
#include "FaultIsolation.h"
#include "GameImGuiLayer.h"
#include "GameplaySystemLifecycle.h"
#include "Graphics/GraphicsConsoleCommands.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Neural/NeuralInference.h"
#include "Graphics/WeatherSystem.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Utils/Assert.h"
#include "Utils/AssetStallDetector.h"
#include "Utils/BenchmarkFramework.h"
#include "Utils/DeadlockDetector.h"
#include "Utils/FreezeDetector.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/HitchDetector.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/LocalFileCache.h"
#include "Utils/Logger.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/SparkConsole.h"
#include <memory>
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS

// Graphics console commands are registered via GraphicsConsoleCommands.cpp

/**
 * @brief Initialize windowed-mode subsystems: engine context, physics, modules,
 *        audio, save system, console commands, and debug/gameplay systems.
 *
 * Called after the Win32 window has been created and InitInstance() succeeded.
 */
static void InitEngineContext()
{
    GetEngineRuntime().eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(GetEngineRuntime().graphics.get(), GetEngineRuntime().input.get(),
                                        GetEngineRuntime().timer.get(), GetEngineRuntime().eventBus.get()));

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null after SetOwned — cannot initialize");
        return;
    }

    g_fileCache = std::make_unique<Spark::LocalFileCache>();
    ctx->SetFileCache(g_fileCache.get());

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

    ctx->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    ctx->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    static Spark::AssetRegistry g_assetRegistry;
    ctx->SetAssetRegistry(&g_assetRegistry);

    // ECS world service. Nothing else provides one in module mode, yet
    // IEngineContext::GetWorld() is the documented way for game modules to
    // reach the ECS — without this, ECS-driven modules (e.g. TERRAFRONT)
    // silently got nullptr and fell back to degenerate non-ECS stubs.
    // Owned by g_engineEcsWorld (SparkEngine.cpp): ShutdownEngine destroys it
    // BEFORE module DLLs are unmapped — a static here destructed after
    // FreeLibrary and called into unmapped module-instantiated entt pools,
    // hanging shutdown inside the crash handler.
    extern std::unique_ptr<::World> g_engineEcsWorld;
    g_engineEcsWorld = std::make_unique<::World>();
    ctx->SetWorld(g_engineEcsWorld.get());

    if (GetEngineRuntime().graphics && GetEngineRuntime().graphics->GetAssetPipeline())
    {
        ctx->SetAssetPipeline(GetEngineRuntime().graphics->GetAssetPipeline());
    }

    // Initialize neural inference engine (GPU compute-based, no external ML deps)
    auto& neuralInference = Spark::Graphics::Neural::NeuralInferenceEngine::GetInstance();
    neuralInference.Initialize();
    ctx->RegisterSystem<Spark::Graphics::Neural::NeuralInferenceEngine>(&neuralInference);
}

static void InitGameplaySubsystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — gameplay subsystems skipped");
        return;
    }

    g_weatherSystem = std::make_unique<Spark::WeatherSystem>();
    ctx->SetWeather(g_weatherSystem.get());
    ctx->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    g_uiSystem = std::make_unique<Spark::UI::UISystem>();
    ctx->SetUI(g_uiSystem.get());

    g_dialogueSystem = std::make_unique<Spark::DialogueSystem>();
    ctx->SetDialogue(g_dialogueSystem.get());

    g_modSystem = std::make_unique<Spark::ModSystem>();
    ctx->SetModSystem(g_modSystem.get());
}

static void LoadAndInitModules(LPWSTR lpCmdLine)
{
    GetEngineRuntime().moduleManager = std::make_unique<ModuleManager>();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*GetEngineRuntime().moduleManager, lpCmdLine))
    {
        GetEngineRuntime().moduleManager->InitializeAll(EngineContext::Get());

        auto* primary = GetEngineRuntime().moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::wstring title = L"Spark Engine - ";
            std::string modName(info.name);
            title.append(modName.begin(), modName.end());
            HWND hWnd = FindWindowW(g_szClass, g_szTitle);
            if (hWnd)
                SetWindowTextW(hWnd, title.c_str());
        }

        console.LogSuccess("Loaded " + std::to_string(GetEngineRuntime().moduleManager->GetModuleCount()) +
                           " module(s)");
    }
    else if (!g_projectSelectorCandidates.empty())
    {
#ifdef SPARK_HAS_IMGUI
        // Bare launch with several game modules available: show the picker.
        Spark::GameImGui::SetAuxPanel(&DrawProjectSelectorPanel);
        console.LogInfo("Project selector open — pick a module to launch.");
#else
        console.LogError("Multiple game modules found but ImGui is compiled out — pass -game <dll> to choose one.");
        g_projectSelectorCandidates.clear();
#endif
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
        console.LogInfo("Place a game DLL (e.g. SparkGame.dll) next to the engine executable,");
        console.LogInfo("use -game <path> on the command line,");
        console.LogInfo("or create a spark.modules.json manifest.");
    }

    GetEngineRuntime().moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    GetEngineRuntime().moduleHotReload->Initialize(GetEngineRuntime().moduleManager.get(), EngineContext::Get());
    GetEngineRuntime().moduleHotReload->WatchAllLoadedModules();
    GetEngineRuntime().moduleHotReload->Start();
}

void InitializeWindowedSubsystems(HINSTANCE hInstance, LPWSTR lpCmdLine)
{
    InitEngineContext();
    SPARK_HEARTBEAT();
    InitGameplaySubsystems();
    SPARK_HEARTBEAT();

    auto& console = Spark::SimpleConsole::GetInstance();

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        console.LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    console.LogInfo("SaveSystem initialized");

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    GetEngineRuntime().audioEngine = std::make_unique<AudioEngine>();
    if (SUCCEEDED(GetEngineRuntime().audioEngine->Initialize(32)))
    {
        console.LogInfo("AudioEngine initialized (32 sources)");
        if (auto* ctx = EngineContext::Get())
            ctx->SetAudio(GetEngineRuntime().audioEngine.get());
    }
    else
    {
        console.LogWarning("AudioEngine initialization failed - audio commands will be unavailable");
        GetEngineRuntime().audioEngine.reset();
    }

    // Create cross-platform audio backend (wraps AudioEngine on Windows, OpenAL on Linux)
    GetEngineRuntime().audioBackend =
        Spark::Audio::CreateAudioBackend(Spark::Audio::AudioBackendType::Auto, GetEngineRuntime().audioEngine.get());
    if (GetEngineRuntime().audioBackend && !GetEngineRuntime().audioBackend->IsAvailable() &&
        !GetEngineRuntime().audioBackend->Initialize(32))
    {
        console.LogWarning("Cross-platform audio backend initialization failed; sequencer audio will be silent");
    }
    Spark::Cinematic::SequencerManager::GetInstance().SetAudioBackend(GetEngineRuntime().audioBackend.get());

    // Game-mode ImGui overlay: init BEFORE modules load so the exe context +
    // allocators can be injected into each module DLL at load time, then hook
    // the overlay render into GraphicsEngine's pre-present callback so module
    // HUDs (IModule::OnImGui) draw every presented frame — no editor involved.
    if (GetEngineRuntime().graphics)
    {
        auto* gfx = GetEngineRuntime().graphics.get();
        HWND hWnd = FindWindowW(g_szClass, nullptr);
        if (Spark::GameImGui::Init(hWnd, gfx->GetDevice(), gfx->GetContext()))
        {
            void *imguiCtx = nullptr, *allocFn = nullptr, *freeFn = nullptr, *userData = nullptr;
            Spark::GameImGui::GetInjectionData(&imguiCtx, &allocFn, &freeFn, &userData);
            ModuleManager::SetImGuiInjection(imguiCtx, allocFn, freeFn, userData);
            gfx->SetPrePresentHook(
                [](void*) { Spark::GameImGui::RenderOverlay(GetEngineRuntime().moduleManager.get()); }, nullptr);
            Spark::SimpleConsole::GetInstance().LogSuccess("Game-mode ImGui overlay initialized");
        }
        else
        {
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("Game-mode ImGui overlay init FAILED (hwnd=") +
                                                           (hWnd ? "ok" : "null") + ")");
        }
    }

    SPARK_HEARTBEAT();
    LoadAndInitModules(lpCmdLine);
    SPARK_HEARTBEAT();

    if (GetEngineRuntime().graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*GetEngineRuntime().graphics);
    Spark::RegisterEngineConsoleCommands(GetEngineRuntime().moduleManager.get(), GetEngineRuntime().audioEngine.get(),
                                         GetEngineRuntime().moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    Spark::FreezeDetector::GetInstance().RegisterConsoleCommands();
    // Init is done — switch the startup watchdog (started in wWinMain with
    // lenient thresholds) over to the tighter runtime config and clear any
    // recovery flag a slow init stage may have raised.
    Spark::FreezeDetector::GetInstance().Stop();
    Spark::FreezeDetector::GetInstance().Configure(Spark::FreezeDetectorConfig{});
    Spark::FreezeDetector::GetInstance().Start();
    Spark::FreezeDetector::GetInstance().AcknowledgeRecovery();
    Spark::DeadlockDetector::GetInstance().RegisterConsoleCommands();
    Spark::HitchDetector::GetInstance().RegisterConsoleCommands();
    Spark::BenchmarkFramework::GetInstance().RegisterConsoleCommands();
    Spark::AssetStallDetector::GetInstance().RegisterConsoleCommands();
    Spark::AssetValidator::GetInstance().RegisterConsoleCommands();
    Spark::NetworkHealthMonitor::GetInstance().RegisterConsoleCommands();
    Spark::GPUResourceLeakDetector::GetInstance().RegisterConsoleCommands();
    Spark::InvalidStateDetector::GetInstance().RegisterConsoleCommands();
    Assert::RegisterConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    LogMissingModuleWarnings();

    if (g_weatherSystem && GetEngineRuntime().eventBus)
    {
        g_weatherSystem->SetEventBus(GetEngineRuntime().eventBus.get());
    }

    InitConsole();
}

#endif // SPARK_PLATFORM_WINDOWS
