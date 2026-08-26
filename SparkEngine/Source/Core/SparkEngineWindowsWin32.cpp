/**
 * @file SparkEngineWindowsWin32.cpp
 * @brief Win32 message pump, window creation, and window procedure (Windows).
 *
 * Split from SparkEngineWindows.cpp to keep files under the ~500-line guideline.
 * Contains RunWindowedMainLoop (message pump + per-frame engine tick),
 * MyRegisterClass/InitInstance (window class + window/graphics/input creation),
 * and the WndProc/About callbacks. The wWinMain entry point stays in
 * SparkEngineWindows.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "framework.h"
#include "SparkEngineWindowsInternal.h"
#include "Audio/AudioEngine.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/ECS/Components.h" // ::World — engine-owned ECS world service
#include "Engine/Events/EventSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/UI/UISystem.h"
#include "EngineSettings.h"
#include "EngineContext.h"
#include "EngineRuntime.h"
#include "FaultIsolation.h"
#include "FixedTimestepAccumulator.h"
#include "GameImGuiLayer.h"
#include "GameplaySystemLifecycle.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/ProjectAssetPath.h"
#include "Graphics/WeatherSystem.h"
#include "Graphics/WorldBasicRenderer.h" // -scene: Spark::RenderWorldBasic
#include "Input/InputManager.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "SceneManager/ReflectedSceneSerializer.h" // -scene: Spark::LoadWorld
#include "Utils/Assert.h"
#include "Utils/ConsoleProcessManager.h"
#include "Utils/DeltaSmoother.h"
#include "Utils/FreezeDetector.h"
#include "Utils/LocalFileCache.h"
#include "Utils/SparkConsole.h"
#include "Utils/Timer.h"
#include <format>
#include <memory>
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS

static Spark::DeltaSmoother g_deltaSmoother(10);

/**
 * @brief Run the Win32 message pump and per-frame engine tick loop.
 *
 * Returns the wParam from the WM_QUIT message for use as the process exit code.
 */
int RunWindowedMainLoop(HINSTANCE hInstance)
// NOTE: Intentionally exceeds 50-line guideline — linear main loop dispatch
{
    HACCEL accel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SparkEngine));
    MSG msg = {};
    ASSERT(GetEngineRuntime().timer);

    auto& console = Spark::SimpleConsole::GetInstance();
    Spark::ConsoleProcessManager::GetInstance().SetShutdownRequestHandler([] { PostQuitMessage(0); });
    console.LogInfo("Starting main engine loop...");

    // -scene <path>: load a reflected-scene JSON and render it via the
    // shared WorldBasicRenderer when no game module took over rendering.
    // File-scope statics so they outlive this stack frame for every tick.
    static World g_sceneWorld;
    static Spark::WorldMeshCache g_sceneCache;
    std::string sceneProjectRoot;
    bool haveModules = GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules();
    if (!g_scenePath.empty() && !haveModules)
    {
        if (Spark::LoadWorld(g_sceneWorld, g_scenePath))
        {
            // Explicit scene preview renders this dedicated world rather than
            // the ordinary runtime world. Publish that same instance through
            // EngineContext so camera/debug console commands operate on what
            // the user can actually see.
            if (EngineContext* context = EngineContext::Get())
                context->SetWorld(&g_sceneWorld);
            if (const auto root = Spark::DeriveProjectRootFromScenePath(g_scenePath))
                sceneProjectRoot = *root;
            else
                console.LogWarning("[-scene] Could not derive a project root from a canonical Scenes/... path; "
                                   "relative assets are disabled");
            console.LogSuccess(std::format("[-scene] Loaded '{}' ({} entities)", g_scenePath,
                                           g_sceneWorld.GetRegistry().storage<entt::entity>().size()));
        }
        else
        {
            console.LogError(std::format("[-scene] Failed to load '{}'", g_scenePath));
            g_scenePath.clear();
        }
    }

    if (g_testFrameLimit > 0)
        console.LogInfo(std::format("Test mode: will exit after {} frames", g_testFrameLimit));

    int frameCount = 0;
    bool quitPosted = false;

    // Win32 message pump: PeekMessage with PM_REMOVE gives us non-blocking
    // message processing — the engine ticks in the else branch whenever
    // there are no pending OS messages (resize, input, focus, etc.).
    while (true)
    {
        // Test frame limit: post WM_QUIT once, then KEEP pumping messages so
        // the quit is actually consumed. The old `continue` skipped PeekMessage,
        // spinning forever without SPARK_HEARTBEAT until the FreezeDetector
        // killed the process (exit code 1) on every -test-frames run.
        if (((g_testFrameLimit > 0 && frameCount >= g_testFrameLimit) ||
             (g_testSecondsLimit > 0.0 && ExecElapsedSeconds() >= g_testSecondsLimit)) &&
            !quitPosted)
        {
            if (CanShutdownEngine())
            {
                console.LogInfo(std::format("[TEST] Limit reached (frame {} / t={:.1f}s). Exiting.", frameCount,
                                            ExecElapsedSeconds()));
                PostQuitMessage(0);
                quitPosted = true;
            }
            else
            {
                console.LogError("[TEST] Exit postponed: a module could not checkpoint for unload");
            }
        }
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                // PostQuitMessage is also used by the built-in console `quit`
                // command. Consume a vetoed quit and resume the live loop so
                // modules can retry their persistence checkpoint; returning
                // from WinMain would destroy their state despite the veto.
                if (!CanShutdownEngine())
                {
                    console.LogError("Quit postponed: a module could not checkpoint for safe unload");
                    msg.message = WM_NULL;
                    quitPosted = false;
                    continue;
                }
                break;
            }
            if (!TranslateAccelerator(msg.hwnd, accel, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            SPARK_HEARTBEAT();

            // If the freeze detector requested recovery, skip this frame
            if (SPARK_FREEZE_RECOVERY_REQUESTED())
            {
                SPARK_FREEZE_RECOVERY_ACK();
                continue;
            }

            // Smooth delta time over the last N frames to prevent physics/animation
            // jitter caused by single-frame spikes (e.g. shader compilation stalls,
            // OS scheduling delays). Raw dt is preserved for profiling accuracy.
            float rawDt = GetEngineRuntime().timer ? GetEngineRuntime().timer->GetDeltaTime() : 0.016f;
            float dt = g_deltaSmoother.Smooth(rawDt);

            // Advance the global fixed-timestep accumulator so all systems can
            // query GetFixedStepCount() for deterministic fixed-rate updates.
            Spark::FixedTimestepAccumulator::GetInstance().Advance(rawDt);

            SPARK_GUARDED_UPDATE("Input", "Core", {
                if (GetEngineRuntime().input)
                    GetEngineRuntime().input->Update();
            });

            if (GetEngineRuntime().moduleManager && GetEngineRuntime().moduleManager->HasModules())
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
                // Engine-only mode: clear, optionally render a -scene, present.
                auto* gfx = GetEngineRuntime().graphics.get();
                gfx->BeginFrame();
                if (!g_scenePath.empty())
                {
                    using namespace DirectX;
                    // Prefer the authored main camera (or the first valid
                    // camera as a legacy fallback). The old hard-coded origin
                    // camera ignored template composition and routinely put
                    // scene content outside the preview frame.
                    XMVECTOR eye = XMVectorSet(2.0f, 1.7f, -3.5f, 1.0f);
                    XMVECTOR at = XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f);
                    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                    float fov = 60.0f;
                    float nearPlane = 0.1f;
                    float farPlane = 6000.0f;
                    EntityID fallbackCamera = entt::null;
                    EntityID selectedCamera = entt::null;
                    for (auto cameraEntity : g_sceneWorld.GetEntitiesWith<Transform, Camera>())
                    {
                        const Camera* camera = g_sceneWorld.GetComponent<Camera>(cameraEntity);
                        if (!camera)
                            continue;
                        if (fallbackCamera == entt::null)
                            fallbackCamera = cameraEntity;
                        if (camera->isMainCamera)
                        {
                            selectedCamera = cameraEntity;
                            break;
                        }
                    }
                    if (selectedCamera == entt::null)
                        selectedCamera = fallbackCamera;
                    if (selectedCamera != entt::null)
                    {
                        const Transform* transform = g_sceneWorld.GetComponent<Transform>(selectedCamera);
                        const Camera* camera = g_sceneWorld.GetComponent<Camera>(selectedCamera);
                        if (transform && camera && camera->fov > 0.0f && camera->fov < 180.0f &&
                            camera->nearPlane > 0.0f && camera->farPlane > camera->nearPlane)
                        {
                            const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
                                XMConvertToRadians(transform->rotation.x), XMConvertToRadians(transform->rotation.y),
                                XMConvertToRadians(transform->rotation.z));
                            eye = XMLoadFloat3(&transform->position);
                            const XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
                            up = XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), rotation);
                            at = XMVectorAdd(eye, forward);
                            fov = camera->fov;
                            nearPlane = camera->nearPlane;
                            farPlane = camera->farPlane;
                        }
                    }
                    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
                    int fbW = g_windowWidthOverride > 0 ? g_windowWidthOverride : 1280;
                    int fbH = g_windowHeightOverride > 0 ? g_windowHeightOverride : 720;
                    float aspect = static_cast<float>(fbW) / static_cast<float>(fbH);
                    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fov), aspect, nearPlane, farPlane);
                    Spark::RenderWorldBasic(g_sceneWorld, *gfx, g_sceneCache, view, proj, sceneProjectRoot);
                }
                gfx->EndFrame();
            }

            if (GetEngineRuntime().moduleHotReload)
                GetEngineRuntime().moduleHotReload->PollChanges();

            // Pump the audio engine: advances source state machine, applies
            // 3D spatialization and distance attenuation. Pre-existing bug —
            // AudioEngine::Update was never called from the main loop.
            SPARK_GUARDED_UPDATE("Audio", "Core", {
                if (GetEngineRuntime().audioEngine)
                    GetEngineRuntime().audioEngine->Update(dt);
            });

            UpdateGameplaySystems(dt);
            UpdateDebugSystems(dt);
            SPARK_GUARDED_UPDATE("Console", "Core", {
                Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
                console.Update();
            });

            // Bare-launch project selector: a panel pick lands here, outside
            // the ImGui frame, where module load + init is safe.
            ConsumeProjectSelectorChoice();

            RunDueScriptedCommands(frameCount);
            ++frameCount;
        }
    }

    // Shutdown
    GetEngineRuntime().moduleHotReload.reset();
    // These globals hold graphics/ImGui-adjacent state: destroy them here,
    // in reverse creation order, NOT at static teardown - the C runtime exit
    // path otherwise AVs in ~UIPanel (dead ImGui/graphics) and then hangs
    // inside the crash handler.
    g_modSystem.reset();
    g_dialogueSystem.reset();
    g_uiSystem.reset();
    g_weatherSystem.reset();
    console.LogInfo("Shutting down...");
    g_fileCache.reset();

    // Tear down the game-mode ImGui overlay while the D3D device is still
    // alive, and unhook it so no EndFrame during teardown re-enters ImGui.
    if (GetEngineRuntime().graphics)
        GetEngineRuntime().graphics->SetPrePresentHook(nullptr, nullptr);
    Spark::GameImGui::Shutdown();

    ShutdownEngineAfterPreflight();

    return static_cast<int>(msg.wParam);
}

// ===================================================================================
//                       Win32 boilerplate
// ===================================================================================
ATOM MyRegisterClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SparkEngine));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDC_SparkEngine);
    wc.lpszClassName = g_szClass;
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALL));

    ATOM result = RegisterClassExW(&wc);
    ASSERT_MSG(result != 0, "RegisterClassExW returned zero");
    return result;
}

BOOL InitInstance(HINSTANCE hInst, int nCmdShow)
{
    ASSERT(hInst != nullptr);
    g_hInst = hInst;

    // Load engine settings from INI (before window creation so we can use the dimensions)
    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = g_windowWidthOverride > 0 ? g_windowWidthOverride : settings.Graphics().windowWidth;
    int winH = g_windowHeightOverride > 0 ? g_windowHeightOverride : settings.Graphics().windowHeight;

    // Keep creation explicitly Unicode to match RegisterClassExW and the
    // DefWindowProcW fallback below. Module startup replaces this caption
    // with the project name when a game module is loaded.
    HWND hWnd = CreateWindowW(g_szClass, L"Spark Engine", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, winW, winH, nullptr,
                              nullptr, hInst, nullptr);

    if (!hWnd)
    {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"CreateWindowW failed (0x%08X)", static_cast<unsigned>(err));
        MessageBoxW(nullptr, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }
    g_mainWindow = hWnd;
    SetWindowTextW(hWnd, L"Spark Engine");

    GetEngineRuntime().timer = std::make_unique<Timer>();
    ASSERT(GetEngineRuntime().timer);

    GetEngineRuntime().graphics = std::make_unique<GraphicsEngine>();
    ASSERT(GetEngineRuntime().graphics);
    HRESULT hr = GetEngineRuntime().graphics->Initialize(hWnd);
    if (FAILED(hr))
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Graphics initialization failed (HR=0x%08X)", static_cast<unsigned>(hr));
        MessageBoxW(hWnd, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // Apply VSync setting from INI
    GetEngineRuntime().graphics->Console_SetVSync(settings.Graphics().vsync);

    GetEngineRuntime().input = std::make_unique<InputManager>();
    ASSERT(GetEngineRuntime().input);
    GetEngineRuntime().input->Initialize(hWnd);

    // Apply input settings from INI
    GetEngineRuntime().input->Console_SetMouseSensitivity(settings.Controls().mouseSensitivity);
    GetEngineRuntime().input->Console_SetInvertMouseY(settings.Controls().invertMouseY);
    GetEngineRuntime().input->Console_SetMouseDeadZone(settings.Controls().mouseDeadZone);
    GetEngineRuntime().input->Console_SetRawMouseInput(settings.Controls().rawMouseInput);
    GetEngineRuntime().input->Console_SetMouseAcceleration(settings.Controls().mouseAcceleration);

    // Console init is handled by InitConsole() in InitializeWindowedSubsystems()
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    return TRUE;
}

// ===================================================================================
//                          Window procedure
// ===================================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Game-mode ImGui overlay gets first look at input so HUD menus
    // (spawn screen, map) are clickable. Gameplay input still flows to
    // InputManager below; ImGui only "captures" when hovering its widgets.
    Spark::GameImGui::HandleWndProc(hWnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (GetEngineRuntime().input)
        {
            // While gameplay owns the mouse (FPS look mode) it keeps the
            // keyboard too; otherwise a focused ImGui text field eats keys.
            if (GetEngineRuntime().input->IsMouseCaptured() || !Spark::GameImGui::WantsKeyboard())
                GetEngineRuntime().input->HandleMessage(msg, wParam, lParam);
        }
        break;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (GetEngineRuntime().input)
        {
            // Clicks on HUD widgets must not fall through into gameplay
            // capture — but once gameplay has captured the mouse it keeps
            // receiving input even if ImGui windows sit under the hidden
            // cursor. Button-up always flows so gameplay never sees a
            // stuck-down button when ImGui grabs capture mid-press.
            const bool buttonUp = (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP);
            if (GetEngineRuntime().input->IsMouseCaptured() || buttonUp || !Spark::GameImGui::WantsMouse())
                GetEngineRuntime().input->HandleMessage(msg, wParam, lParam);
        }
        break;

    case WM_SIZE:
        if (GetEngineRuntime().graphics)
            GetEngineRuntime().graphics->OnResize(LOWORD(lParam), HIWORD(lParam));
        if (GetEngineRuntime().moduleManager)
            GetEngineRuntime().moduleManager->ResizeAll(LOWORD(lParam), HIWORD(lParam));
        if (GetEngineRuntime().eventBus)
            GetEngineRuntime().eventBus->Publish(Spark::WindowResizeEvent{LOWORD(lParam), HIWORD(lParam)});
        break;

    case WM_CLOSE:
        // Keep the window/resources alive until the main loop completes the
        // one non-destructive shutdown preflight. A vetoed WM_QUIT is consumed
        // there and the application continues normally.
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        if (hWnd == g_mainWindow)
            g_mainWindow = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    // The class is registered with RegisterClassExW and every WM_* string
    // payload must therefore reach the Unicode default procedure. Using the
    // encoding-neutral macro here compiled to DefWindowProcA in non-UNICODE
    // builds and truncated same-thread wide captions to their first letter.
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

INT_PTR CALLBACK About(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    if (msg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
    {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}

#endif // SPARK_PLATFORM_WINDOWS
