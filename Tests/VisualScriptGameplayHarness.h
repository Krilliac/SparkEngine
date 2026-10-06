/**
 * @file VisualScriptGameplayHarness.h
 * @brief Headless player for the SparkGameVisualScript demo, shared by the MOD-390 and ENG-200 gameplay tests.
 *
 * Builds the module's real demo world (VisualScriptDemoWorld.cpp, the code
 * OnLoad and vs_restart run) from a caller-chosen list of script roots against
 * a real World, the production AngelScriptEngine and a real InputManager
 * installed in an injected EngineContext, exactly where the script getKey /
 * getKeyDown bindings read it. The harness only plays the role of the human at
 * the keyboard: each frame it presses W/A/S/D towards a target, then ticks
 * the production engine-owned ScriptRuntimeSystem at a fixed 1/60 s delta
 * (in the product the engine Timer caps the delta). It does not reimplement
 * script lifecycle dispatch, so scripts run in ECS storage order, not the
 * order the demo spawned them.
 *
 * All gameplay is decided by the scripts and observed only through
 * script-visible state: Transform and HealthComponent values the scripts
 * write, the components their media bindings record, and what they print().
 */
#pragma once

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoRuntime.h"
#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoWorld.h"
#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Systems/ECSystems.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Input/InputManager.h"
#include "ScopedLoggerBaseline.h"
#include "Utils/Logger.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VisualScriptGameplayHarness
{
    namespace fs = std::filesystem;

    inline constexpr float kFrameDelta = 1.0f / 60.0f;

    /// Horizontal distance at which the scripted "player" stops pressing keys towards a target.
    inline constexpr float kArrivalTolerance = 0.2f;

    /// Script print() output is logged as "[Script] <message>"; this is the exact prefix.
    inline constexpr std::string_view kScriptPrintPrefix = "[Script] ";

    /// Coin visiting order (VS_Coin_<index>) that walks the player past every pickup without backtracking.
    inline constexpr std::array<int, 5> kCoinRoute = {2, 1, 0, 3, 4};

    /// The module's checked-in generated scripts in the source tree.
    inline fs::path SourceScriptRoot()
    {
        return fs::path(SPARK_TEST_SOURCE_DIR) / "GameModules/SparkGameVisualScript/Assets/Scripts/Generated";
    }

    /// Restores the process-wide injected EngineContext on every exit path.
    struct ScopedInjectedContext
    {
        explicit ScopedInjectedContext(EngineContext* context) { EngineContext::SetInjected(context); }
        ~ScopedInjectedContext() { EngineContext::SetInjected(nullptr); }
        ScopedInjectedContext(const ScopedInjectedContext&) = delete;
        ScopedInjectedContext& operator=(const ScopedInjectedContext&) = delete;
    };

    /// The demo, running on a real World / AngelScriptEngine / InputManager.
    struct GameplayFixture
    {
        ScopedLoggerBaseline loggerBaseline; // restored last, after the capture sink is gone
        std::vector<std::string> scriptOutput;
        World world;
        InputManager input;
        EngineContext context;
        std::optional<ScopedInjectedContext> injected;
        AngelScriptEngine engine;
        Spark::ECS::ScriptRuntimeSystem scriptRuntime{&engine};
        std::optional<Spark::VisualScriptDemo::DemoWorld> demo; // destroyed first (declared last)
        bool ready = false;

        /// Load the scripts from the first complete root in @p searchPaths (DemoWorld::LoadScripts), then spawn.
        explicit GameplayFixture(std::span<const fs::path> searchPaths)
        {
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
                [this](const Spark::LogMessage& message)
                {
                    if (message.message.starts_with(kScriptPrintPrefix))
                        scriptOutput.push_back(message.message.substr(kScriptPrintPrefix.size()));
                }));

            context.SetWorld(&world);
            context.SetInput(&input);
            injected.emplace(&context);

            demo.emplace(world, engine);
            ready = engine.Initialize() && demo->LoadScripts(searchPaths) && demo->Spawn();
            if (ready)
            {
                scriptRuntime.Update(world, 0.0f);
            }
        }

        /// The shipped scripts from the module's source tree.
        GameplayFixture() : GameplayFixture(std::array<fs::path, 1>{SourceScriptRoot()}) {}

        ~GameplayFixture()
        {
            demo.reset();
            if (AngelScriptEngine::GetBoundWorld() == &world)
                AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
            injected.reset();
            Spark::Logger::Get().ClearSinks(); // drop the sink capturing `this` before it dangles
        }

        GameplayFixture(const GameplayFixture&) = delete;
        GameplayFixture& operator=(const GameplayFixture&) = delete;

        EntityID Find(const std::string& name)
        {
            for (auto entity : world.GetEntitiesWith<NameComponent>())
            {
                const auto* component = world.GetComponent<NameComponent>(entity);
                if (component && component->name == name)
                    return entity;
            }
            return entt::null;
        }

        DirectX::XMFLOAT3 Position(EntityID entity) { return world.GetComponent<Transform>(entity)->position; }
        float Health(EntityID entity) { return world.GetComponent<HealthComponent>(entity)->health; }

        /// Hold or release one key through the same message path the platform hosts use.
        void SetKey(int virtualKey, bool down) { input.HandleMessage(down ? WM_KEYDOWN : WM_KEYUP, virtualKey, 0); }

        /// Press the WASD keys that move the player towards (x, z); release all of them once there.
        void SteerTowards(float x, float z)
        {
            const auto player = Position(Find("VS_Player"));
            const float dx = x - player.x;
            const float dz = z - player.z;
            SetKey('D', dx > kArrivalTolerance);
            SetKey('A', dx < -kArrivalTolerance);
            SetKey('W', dz > kArrivalTolerance);
            SetKey('S', dz < -kArrivalTolerance);
        }

        void ReleaseAllKeys() { SteerTowards(Position(Find("VS_Player")).x, Position(Find("VS_Player")).z); }

        /// One host frame: input edge update, then the production script runtime.
        void Tick()
        {
#ifndef _WIN32
            // The Windows Update() requires the HWND from Initialize() and aborts without one. getKey reads
            // IsKeyDown, which the injected WM_KEYDOWN/WM_KEYUP messages drive on every platform, so the
            // Windows frame skips the edge update; none of these scenarios uses the edge-triggered getKeyDown.
            input.Update();
#endif
            const float scriptDelta = Spark::VisualScriptDemo::SanitizeDeltaTime(kFrameDelta);
            scriptRuntime.Update(world, scriptDelta);
        }

        /**
         * Walk the player over every coin's spawn X/Z in kCoinRoute order; the coin scripts decide each pickup
         * (Collectible hides a collected coin at y = -100). Returns the frames used, or @p frameBudget when the
         * budget ran out first.
         */
        int CollectAllCoins(int frameBudget)
        {
            int frame = 0;
            for (int coinIndex : kCoinRoute)
            {
                const EntityID coin = Find("VS_Coin_" + std::to_string(coinIndex));
                if (coin == entt::null)
                    return frameBudget;
                const auto target = Position(coin);
                while (Position(coin).y > -50.0f)
                {
                    if (frame >= frameBudget)
                        return frameBudget;
                    SteerTowards(target.x, target.z);
                    Tick();
                    ++frame;
                }
            }
            // ECS iteration does not promise the manager runs after the last
            // collectible. Give it one ordinary frame to observe the final score.
            if (frame < frameBudget)
            {
                Tick();
                ++frame;
            }
            return frame;
        }

        size_t CountOutput(std::string_view text) const
        {
            size_t count = 0;
            for (const auto& line : scriptOutput)
            {
                if (line == text)
                    ++count;
            }
            return count;
        }

        bool AnyScriptFaulted()
        {
            for (EntityID entity : demo->GetEntities())
            {
                if (engine.IsScriptFaulted(entity))
                    return true;
            }
            return false;
        }
    };
} // namespace VisualScriptGameplayHarness

#endif // SPARK_ANGELSCRIPT_SUPPORT
