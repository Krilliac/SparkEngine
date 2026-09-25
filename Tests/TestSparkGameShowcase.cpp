/**
 * @file TestSparkGameShowcase.cpp
 * @brief MOD-300: the SparkGame showcase, loaded as the real module image, drives its coroutine sequence
 *        through IEngineContext::GetCoroutineScheduler() and restores its exact state on quickload.
 *
 * Each test loads the built libSparkGame image through ModuleManager with a host context that supplies a real
 * World, EventBus and the engine CoroutineScheduler, then steps the scheduler with an exactly representable
 * delta (1/64 s) so every step boundary is deterministic.
 */

#include "TestFramework.h"

#if !defined(_WIN32) && defined(SPARK_TEST_SPARK_GAME_MODULE_PATH)

#include "Core/ModuleManager.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/ECS/Components.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/EventBus.h"
#include "Utils/SparkConsole.h"
#include <Spark/IEngineContext.h>
#include <Spark/Version.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

namespace
{
    constexpr float STEP_SECONDS = 1.0f / 64.0f;
    // Update 1 runs the spawn step and the first wait's first tick; the 3 s wait
    // is satisfied on update 192 (192/64 = 3.0 exactly), which then runs the damage
    // step and the 2 s wait's first tick, satisfied 127 updates later.
    constexpr int DAMAGE_UPDATE = 192;
    constexpr int HEAL_UPDATE = DAMAGE_UPDATE + 127;
    constexpr const char* MODULE_NAME = "Spark Default - Engine Showcase";
    constexpr const char* COROUTINE_NAME = "SparkGame.ShowcaseLifecycle";

    /// Host context exposing only the capabilities the coroutine sequence needs.
    class ShowcaseHostContext final : public Spark::IEngineContext
    {
      public:
        ShowcaseHostContext(World* world, Spark::EventBus* eventBus, Spark::CoroutineScheduler* scheduler,
                            Spark::SaveSystem* saveSystem = nullptr)
            : m_world(world), m_eventBus(eventBus), m_scheduler(scheduler), m_saveSystem(saveSystem)
        {
        }

        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return m_eventBus; }
        const Spark::EventBus* GetEventBus() const override { return m_eventBus; }
        AudioEngine* GetAudio() override { return nullptr; }
        const AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        ::World* GetWorld() override { return m_world; }
        const ::World* GetWorld() const override { return m_world; }
        Spark::CoroutineScheduler* GetCoroutineScheduler() override { return m_scheduler; }
        const Spark::CoroutineScheduler* GetCoroutineScheduler() const override { return m_scheduler; }
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }

      private:
        World* m_world;
        Spark::EventBus* m_eventBus;
        Spark::CoroutineScheduler* m_scheduler;
        Spark::SaveSystem* m_saveSystem;
    };

    /// Initializes the shared console for the test (module commands need it) and restores it afterwards.
    struct ConsoleScope final
    {
        Spark::SimpleConsole& console = Spark::SimpleConsole::GetInstance();
        bool restoreUninitialized = !console.IsInitialized();
        ConsoleScope()
        {
            if (restoreUninitialized)
                console.Initialize();
        }
        ~ConsoleScope()
        {
            if (restoreUninitialized)
                console.Shutdown();
        }
    };

    /// Loads the real SparkGame image and guarantees shutdown-then-unload on every exit path.
    struct LoadedShowcase final
    {
        ModuleManager manager;
        ~LoadedShowcase()
        {
            manager.ShutdownAllAfterPreflight();
            manager.UnloadAll();
        }
    };

    /// Starts from an empty scheduler: other tests share the engine singleton.
    Spark::CoroutineScheduler& CleanScheduler()
    {
        auto& scheduler = Spark::CoroutineScheduler::GetInstance();
        scheduler.StopAll();
        return scheduler;
    }

    std::optional<EntityID> FindNamedEntity(World& world, const std::string& name)
    {
        for (EntityID entity : world.GetEntitiesWith<NameComponent>())
        {
            const auto* nameComponent = world.GetComponent<NameComponent>(entity);
            if (nameComponent && nameComponent->name == name)
                return entity;
        }
        return std::nullopt;
    }

    /// Runs showcase_status through the console and returns its line starting with @p label.
    std::string StatusLine(Spark::SimpleConsole& console, const std::string& label)
    {
        if (!console.ExecuteCommand("showcase_status"))
            return "<showcase_status failed>";
        const auto history = console.GetLogHistory();
        for (auto it = history.rbegin(); it != history.rend(); ++it)
        {
            const auto begin = it->message.find(label);
            if (begin == std::string::npos)
                continue;
            const auto end = it->message.find('\n', begin);
            return it->message.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        }
        return "<no '" + label + "' status line>";
    }

    /// Runs showcase_status through the console and returns its "Coroutine sequence:" line.
    std::string CoroutineStatusLine(Spark::SimpleConsole& console)
    {
        return StatusLine(console, "Coroutine sequence: ");
    }

    /// Runs a console command and returns the result line it logged.
    std::string CommandResult(Spark::SimpleConsole& console, const std::string& commandLine)
    {
        if (!console.ExecuteCommand(commandLine))
            return "<" + commandLine + " failed>";
        const auto history = console.GetLogHistory();
        return history.empty() ? std::string("<no output>") : history.back().message;
    }

    /// Everything the save round-trips for one showcase entity, in a totally ordered form.
    struct ShowcaseEntityState
    {
        std::string name;
        float position[3];
        float rotation[3];
        float scale[3];
        float health;
        float maxHealth;
        bool isDead;
        std::vector<std::string> tags; // sorted

        auto Key() const
        {
            return std::tie(name, position[0], position[1], position[2], rotation[0], rotation[1], rotation[2],
                            scale[0], scale[1], scale[2], health, maxHealth, isDead, tags);
        }
        bool operator==(const ShowcaseEntityState& other) const { return Key() == other.Key(); }
        bool operator<(const ShowcaseEntityState& other) const { return Key() < other.Key(); }
    };

    /// Snapshot of every entity carrying the "showcase" tag, sorted so two snapshots compare as multisets.
    std::vector<ShowcaseEntityState> SnapshotShowcase(World& world)
    {
        std::vector<ShowcaseEntityState> snapshot;
        for (EntityID entity : world.GetEntitiesWith<TagComponent>())
        {
            const auto* tag = world.GetComponent<TagComponent>(entity);
            if (!tag || !tag->HasTag("showcase"))
                continue;

            const auto* name = world.GetComponent<NameComponent>(entity);
            const auto* transform = world.GetComponent<Transform>(entity);
            const auto* health = world.GetComponent<HealthComponent>(entity);
            ShowcaseEntityState state{};
            state.name = name ? name->name : "<no NameComponent>";
            if (transform)
            {
                state.position[0] = transform->position.x;
                state.position[1] = transform->position.y;
                state.position[2] = transform->position.z;
                state.rotation[0] = transform->rotation.x;
                state.rotation[1] = transform->rotation.y;
                state.rotation[2] = transform->rotation.z;
                state.scale[0] = transform->scale.x;
                state.scale[1] = transform->scale.y;
                state.scale[2] = transform->scale.z;
            }
            else
            {
                state.name += " <no Transform>";
            }
            if (health)
            {
                state.health = health->health;
                state.maxHealth = health->maxHealth;
                state.isDead = health->isDead;
            }
            else
            {
                state.name += " <no HealthComponent>";
            }
            state.tags.assign(tag->tags.begin(), tag->tags.end());
            std::sort(state.tags.begin(), state.tags.end());
            snapshot.push_back(std::move(state));
        }
        std::sort(snapshot.begin(), snapshot.end());
        return snapshot;
    }

    /// Points the SaveSystem singleton at a private temporary directory and restores it afterwards.
    class ScopedShowcaseSaveDirectory final
    {
      public:
        ScopedShowcaseSaveDirectory()
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() /
                          ("spark_mod300_showcase_" + std::to_string(static_cast<long long>(::getpid()))))
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            m_saveSystem.SetFileCache(nullptr);
            m_initialized = m_saveSystem.Initialize(m_directory.string());
        }

        ~ScopedShowcaseSaveDirectory()
        {
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        ScopedShowcaseSaveDirectory(const ScopedShowcaseSaveDirectory&) = delete;
        ScopedShowcaseSaveDirectory& operator=(const ScopedShowcaseSaveDirectory&) = delete;

        bool IsInitialized() const { return m_initialized; }
        Spark::SaveSystem& System() { return m_saveSystem; }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        bool m_initialized = false;
    };
} // namespace

TEST(SparkGameShowcase_CoroutineSequence)
{
    ConsoleScope consoleScope;
    auto& scheduler = CleanScheduler();
    World world;
    Spark::EventBus eventBus;
    std::vector<Spark::EntityDamagedEvent> damageEvents;
    auto damageSubscription = eventBus.Subscribe<Spark::EntityDamagedEvent>(
        [&damageEvents](const Spark::EntityDamagedEvent& e) { damageEvents.push_back(e); });
    ShowcaseHostContext context(&world, &eventBus, &scheduler);

    LoadedShowcase showcase;
    ASSERT_TRUE(showcase.manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
    showcase.manager.InitializeAll(&context);
    ASSERT_TRUE(showcase.manager.GetModule(MODULE_NAME) != nullptr);

    // Scheduled on the host scheduler during load, but no step runs until it ticks.
    EXPECT_TRUE(scheduler.IsRunning(COROUTINE_NAME));
    EXPECT_EQ(scheduler.ActiveCount(), size_t{1});
    EXPECT_FALSE(FindNamedEntity(world, "CoroutineTarget").has_value());
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console), std::string("Coroutine sequence: scheduled"));

    scheduler.Update(STEP_SECONDS);
    const auto target = FindNamedEntity(world, "CoroutineTarget");
    ASSERT_TRUE(target.has_value());
    const auto* health = world.GetComponent<HealthComponent>(*target);
    ASSERT_TRUE(health != nullptr);
    EXPECT_EQ(health->health, 100.0f);
    EXPECT_EQ(damageEvents.size(), size_t{0});
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console), std::string("Coroutine sequence: spawned target"));

    for (int update = 2; update < DAMAGE_UPDATE; ++update)
        scheduler.Update(STEP_SECONDS);
    EXPECT_EQ(health->health, 100.0f);
    EXPECT_EQ(damageEvents.size(), size_t{0});

    scheduler.Update(STEP_SECONDS); // DAMAGE_UPDATE
    EXPECT_EQ(health->health, 75.0f);
    ASSERT_EQ(damageEvents.size(), size_t{1});
    EXPECT_EQ(damageEvents[0].entityId, static_cast<uint32_t>(*target));
    EXPECT_EQ(damageEvents[0].damage, 25.0f);
    EXPECT_EQ(damageEvents[0].damageSource, std::string("ShowcaseCoroutine"));
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console), std::string("Coroutine sequence: damaged target"));

    for (int update = DAMAGE_UPDATE + 1; update < HEAL_UPDATE; ++update)
        scheduler.Update(STEP_SECONDS);
    EXPECT_EQ(health->health, 75.0f);
    EXPECT_TRUE(scheduler.IsRunning(COROUTINE_NAME));

    scheduler.Update(STEP_SECONDS); // HEAL_UPDATE
    EXPECT_EQ(health->health, 100.0f);
    EXPECT_EQ(damageEvents.size(), size_t{1});
    EXPECT_FALSE(scheduler.IsRunning(COROUTINE_NAME));
    EXPECT_EQ(scheduler.ActiveCount(), size_t{0});
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console), std::string("Coroutine sequence: complete"));

    // Nothing further happens once the sequence has finished.
    for (int update = 0; update < 256; ++update)
        scheduler.Update(STEP_SECONDS);
    EXPECT_EQ(health->health, 100.0f);
    EXPECT_EQ(damageEvents.size(), size_t{1});
}

TEST(SparkGameShowcase_CoroutineStoppedBeforeUnload)
{
    ConsoleScope consoleScope;
    auto& scheduler = CleanScheduler();
    World world;
    Spark::EventBus eventBus;
    size_t damageEvents = 0;
    auto damageSubscription = eventBus.Subscribe<Spark::EntityDamagedEvent>(
        [&damageEvents](const Spark::EntityDamagedEvent&) { ++damageEvents; });
    ShowcaseHostContext context(&world, &eventBus, &scheduler);

    {
        LoadedShowcase showcase;
        ASSERT_TRUE(showcase.manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
        showcase.manager.InitializeAll(&context);
        for (int update = 1; update <= DAMAGE_UPDATE; ++update)
            scheduler.Update(STEP_SECONDS);
        EXPECT_EQ(damageEvents, size_t{1});
        ASSERT_TRUE(scheduler.IsRunning(COROUTINE_NAME));

        // Shutdown mid-sequence must stop *and destroy* the coroutine before the image unloads:
        // its step callables live in the module image.
        showcase.manager.ShutdownAllAfterPreflight();
        EXPECT_FALSE(scheduler.IsRunning(COROUTINE_NAME));
        EXPECT_EQ(scheduler.ActiveCount(), size_t{0});
        EXPECT_FALSE(FindNamedEntity(world, "CoroutineTarget").has_value());

        showcase.manager.UnloadAll();
        EXPECT_EQ(showcase.manager.GetModuleCount(), size_t{0});
    }

    // The host keeps ticking after the image is gone; nothing of the showcase remains to run.
    for (int update = 0; update < 256; ++update)
        scheduler.Update(STEP_SECONDS);
    EXPECT_EQ(scheduler.ActiveCount(), size_t{0});
    EXPECT_EQ(damageEvents, size_t{1});
}

TEST(SparkGameShowcase_CoroutineAbortsOnLostTargetAndKeepsRootCause)
{
    ConsoleScope consoleScope;
    auto& scheduler = CleanScheduler();
    World world;
    Spark::EventBus eventBus;
    size_t damageEvents = 0;
    auto damageSubscription = eventBus.Subscribe<Spark::EntityDamagedEvent>(
        [&damageEvents](const Spark::EntityDamagedEvent&) { ++damageEvents; });
    ShowcaseHostContext context(&world, &eventBus, &scheduler);

    LoadedShowcase showcase;
    ASSERT_TRUE(showcase.manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
    showcase.manager.InitializeAll(&context);

    scheduler.Update(STEP_SECONDS);
    const auto target = FindNamedEntity(world, "CoroutineTarget");
    ASSERT_TRUE(target.has_value());
    world.DestroyEntity(*target);

    for (int update = 2; update <= DAMAGE_UPDATE; ++update)
        scheduler.Update(STEP_SECONDS);

    // The failing step cancels the rest of the sequence: the heal step never runs, so the first
    // failure stays the reported root cause and the scheduler holds nothing of the showcase.
    EXPECT_EQ(damageEvents, size_t{0});
    EXPECT_FALSE(scheduler.IsRunning(COROUTINE_NAME));
    EXPECT_EQ(scheduler.ActiveCount(), size_t{0});
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console),
              std::string("Coroutine sequence: failed: target lost before damage"));

    for (int update = DAMAGE_UPDATE + 1; update <= HEAL_UPDATE + 64; ++update)
        scheduler.Update(STEP_SECONDS);
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console),
              std::string("Coroutine sequence: failed: target lost before damage"));
}

TEST(SparkGameShowcase_CoroutineWithoutSchedulerFailsVisibly)
{
    ConsoleScope consoleScope;
    auto& scheduler = CleanScheduler();
    World world;
    Spark::EventBus eventBus;
    ShowcaseHostContext context(&world, &eventBus, nullptr);

    LoadedShowcase showcase;
    ASSERT_TRUE(showcase.manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
    showcase.manager.InitializeAll(&context);
    ASSERT_TRUE(showcase.manager.GetModule(MODULE_NAME) != nullptr);

    EXPECT_EQ(scheduler.ActiveCount(), size_t{0});
    EXPECT_EQ(CoroutineStatusLine(consoleScope.console),
              std::string("Coroutine sequence: unavailable (host exposes no CoroutineScheduler)"));
    EXPECT_FALSE(FindNamedEntity(world, "CoroutineTarget").has_value());
}


TEST(SparkGameShowcase_QuickLoadRestoresExactState)
{
    ConsoleScope consoleScope;
    auto& console = consoleScope.console;
    ScopedShowcaseSaveDirectory saveDirectory;
    ASSERT_TRUE(saveDirectory.IsInitialized());
    auto& scheduler = CleanScheduler();
    World world;
    Spark::EventBus eventBus;
    size_t damageEvents = 0;
    auto damageSubscription = eventBus.Subscribe<Spark::EntityDamagedEvent>(
        [&damageEvents](const Spark::EntityDamagedEvent&) { ++damageEvents; });
    ShowcaseHostContext context(&world, &eventBus, &scheduler, &saveDirectory.System());

    LoadedShowcase showcase;
    ASSERT_TRUE(showcase.manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
    showcase.manager.InitializeAll(&context);
    ASSERT_TRUE(showcase.manager.GetModule(MODULE_NAME) != nullptr);

    // Player, Enemy_Alpha, Enemy_Bravo from Initialize, the coroutine target from the first tick,
    // one console-spawned entity, and damage on two of them.
    scheduler.Update(STEP_SECONDS);
    ASSERT_TRUE(FindNamedEntity(world, "CoroutineTarget").has_value());
    const std::string spawnedScout = CommandResult(console, "showcase_spawn Scout");
    const auto alpha = FindNamedEntity(world, "Enemy_Alpha");
    const auto scout = FindNamedEntity(world, "Scout");
    ASSERT_TRUE(alpha.has_value() && scout.has_value());
    EXPECT_EQ(spawnedScout, "Spawned 'Scout' (id=" + std::to_string(static_cast<uint32_t>(*scout)) + ", total=5)");
    world.GetComponent<HealthComponent>(*alpha)->TakeDamage(40.0f);
    world.GetComponent<HealthComponent>(*scout)->TakeDamage(100.0f);
    world.GetComponent<Transform>(*scout)->rotation = {0.0f, 90.0f, 0.0f};
    world.GetComponent<TagComponent>(*scout)->AddTag("elite");

    const auto saved = SnapshotShowcase(world);
    ASSERT_EQ(saved.size(), size_t{5});
    EXPECT_EQ(StatusLine(console, "Spawned entities: "), std::string("Spawned entities: 5"));
    EXPECT_EQ(CommandResult(console, "showcase_save"), std::string("QuickSave successful"));

    // Diverge from the snapshot: more damage, a destroyed entity, and a new spawn.
    world.GetComponent<HealthComponent>(*alpha)->TakeDamage(30.0f);
    const auto bravo = FindNamedEntity(world, "Enemy_Bravo");
    ASSERT_TRUE(bravo.has_value());
    world.DestroyEntity(*bravo);
    EXPECT_TRUE(CommandResult(console, "showcase_spawn Intruder").find("Spawned 'Intruder'") == 0);
    ASSERT_TRUE(SnapshotShowcase(world) != saved);

    EXPECT_EQ(CommandResult(console, "showcase_load"),
              std::string("QuickLoad successful (5 showcase entities restored)"));
    EXPECT_TRUE(SnapshotShowcase(world) == saved);
    EXPECT_FALSE(FindNamedEntity(world, "Intruder").has_value());
    EXPECT_EQ(StatusLine(console, "Spawned entities: "), std::string("Spawned entities: 5"));

    // The in-flight coroutine is cancelled, not resumed against a renumbered target: ticking past
    // its damage and heal points changes nothing in the restored world.
    EXPECT_FALSE(scheduler.IsRunning(COROUTINE_NAME));
    EXPECT_EQ(scheduler.ActiveCount(), size_t{0});
    EXPECT_EQ(CoroutineStatusLine(console), std::string("Coroutine sequence: stopped by quickload"));
    for (int update = 2; update <= HEAL_UPDATE + 64; ++update)
        scheduler.Update(STEP_SECONDS);
    EXPECT_EQ(damageEvents, size_t{0});
    EXPECT_TRUE(SnapshotShowcase(world) == saved);

    // The restored entities are tracked again: the next spawn continues the grid after them.
    const std::string next = CommandResult(console, "showcase_spawn");
    EXPECT_TRUE(next.find(", total=6)") != std::string::npos);
    const auto nextEntity = FindNamedEntity(world, "ShowcaseEntity");
    ASSERT_TRUE(nextEntity.has_value());
    EXPECT_EQ(world.GetComponent<Transform>(*nextEntity)->position.x, 15.0f);

    // Shutdown destroys every tracked showcase entity, including the restored ones.
    showcase.manager.ShutdownAllAfterPreflight();
    EXPECT_EQ(SnapshotShowcase(world).size(), size_t{0});
}

#endif
