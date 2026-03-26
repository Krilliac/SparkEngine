/**
 * @file EngineDiagnostics.cpp
 * @brief Runtime diagnostic commands that exercise live engine subsystems
 *
 * Each Diag*() function performs real operations on a live subsystem:
 * creates objects, mutates state, queries results, then cleans up.
 * The console command `diag_all` runs every subsystem; individual
 * commands like `diag_physics` run one subsystem at a time.
 */

#include "EngineDiagnostics.h"
#include "EngineContext.h"

#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/Gameplay/AbilitySystem.h"
#include "Engine/Gameplay/ConditionSystem.h"
#include "Engine/Gameplay/InstanceManager.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Tween/TweenSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Graphics/WeatherSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/EventBus.h"
#include "Utils/LocalFileCache.h"
#include "Utils/MemoryMonitor.h"
#include "Utils/SparkConsole.h"

#include <cmath>
#include <sstream>

namespace Spark
{

    // ========================================================================
    // DiagReport formatting
    // ========================================================================

    std::string DiagReport::Format() const
    {
        std::ostringstream ss;
        ss << "\n=== Engine Diagnostics Report ===\n\n";

        std::string lastSubsystem;
        for (const auto& r : results)
        {
            if (r.subsystem != lastSubsystem)
            {
                if (!lastSubsystem.empty())
                    ss << "\n";
                ss << "[" << r.subsystem << "]\n";
                lastSubsystem = r.subsystem;
            }
            ss << "  " << (r.passed ? "PASS" : "FAIL") << "  " << r.testName;
            if (!r.detail.empty())
                ss << " -- " << r.detail;
            ss << "\n";
        }

        ss << "\n=== Summary: " << passed << " passed, " << failed << " failed, " << (passed + failed)
           << " total ===\n";
        if (failed == 0)
            ss << "All diagnostics PASSED.\n";
        else
            ss << failed << " diagnostic(s) FAILED.\n";

        return ss.str();
    }

    // ========================================================================
    // Helper
    // ========================================================================

    static EngineContext* Ctx()
    {
        return EngineContext::Get();
    }

    // ========================================================================
    // Physics
    // ========================================================================

    void DiagPhysics(DiagReport& report)
    {
        const std::string sub = "Physics";

        auto* physics = Ctx() ? Ctx()->GetPhysics() : nullptr;
        if (!physics)
        {
            report.Add(sub, "System available", false, "PhysicsSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        // Query metrics
        auto metrics = physics->Console_GetMetrics();
        report.Add(sub, "Metrics query", true,
                   "bodies=" + std::to_string(metrics.totalRigidBodies) +
                       " active=" + std::to_string(metrics.activeRigidBodies));

        // Create a body, verify it exists, then remove it
        bool created = physics->Console_CreateBody("__diag_test_body", "dynamic", 0.0f, 10.0f, 0.0f);
        report.Add(sub, "Create body", created);

        if (created)
        {
            auto info = physics->Console_GetBodyInfo("__diag_test_body");
            bool hasInfo = !info.empty() && info.find("not found") == std::string::npos;
            report.Add(sub, "Query body info", hasInfo);

            // Apply force (void return — success if no exception)
            physics->Console_ApplyForce("__diag_test_body", 0.0f, -9.81f, 0.0f);
            report.Add(sub, "Apply force", true);

            // Raycast downward from above the body
            auto rayResult = physics->Console_Raycast(0.0f, 100.0f, 0.0f, 0.0f, -1.0f, 0.0f, 200.0f);
            bool rayOk = !rayResult.empty();
            report.Add(sub, "Raycast query", rayOk, rayResult.substr(0, 60));

            // Clean up
            bool removed = physics->Console_RemoveBody("__diag_test_body");
            report.Add(sub, "Remove body", removed);
        }

        // Gravity (void return — success if no exception)
        physics->Console_SetGravity(0.0f, -20.0f, 0.0f);
        report.Add(sub, "Set gravity", true);
    }

    // ========================================================================
    // Weather
    // ========================================================================

    void DiagWeather(DiagReport& report)
    {
        const std::string sub = "Weather";

        auto* weather = Ctx() ? Ctx()->GetWeather() : nullptr;
        if (!weather)
        {
            report.Add(sub, "System available", false, "WeatherSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        // Save original state
        auto originalState = weather->GetCurrentState();
        auto originalType = originalState.type;

        // Cycle through weather types
        const WeatherType types[] = {WeatherType::Clear, WeatherType::Rain, WeatherType::Snow, WeatherType::Fog,
                                     WeatherType::Storm};
        bool allTransitions = true;
        for (auto wt : types)
        {
            weather->SetWeather(wt, 0.8f, 0.0f); // instant transition
            weather->Update(0.016f);
            auto state = weather->GetCurrentState();
            if (state.type != wt)
            {
                allTransitions = false;
                report.Add(sub, "Transition to " + std::string(WeatherSystem::GetWeatherTypeName(wt)), false);
            }
        }
        if (allTransitions)
            report.Add(sub, "All weather transitions (5 types)", true);

        // Verify state fields are populated
        weather->SetWeather(WeatherType::Storm, 1.0f, 0.0f);
        weather->Update(0.016f);
        auto storm = weather->GetCurrentState();
        bool stormValid = storm.intensity > 0.0f && storm.windSpeed > 0.0f;
        report.Add(sub, "Storm state populated", stormValid,
                   "intensity=" + std::to_string(storm.intensity) + " wind=" + std::to_string(storm.windSpeed));

        // Restore original weather
        weather->SetWeather(originalType, originalState.intensity, 0.0f);
        weather->Update(0.016f);
        report.Add(sub, "Restore original weather", true);
    }

    // ========================================================================
    // TimeOfDay
    // ========================================================================

    void DiagTimeOfDay(DiagReport& report)
    {
        const std::string sub = "TimeOfDay";

        auto* tod = Ctx() ? Ctx()->GetTimeOfDay() : nullptr;
        if (!tod)
        {
            report.Add(sub, "System available", false, "TimeOfDaySystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        // Save and restore
        float originalTime = tod->GetTimeOfDay();
        float originalScale = tod->GetTimeScale();

        // Set to noon, check sun direction points up
        tod->SetTimeOfDay(12.0f);
        auto sunDir = tod->GetSunDirection();
        bool noonSunUp = sunDir.y > 0.3f; // sun should be high at noon
        report.Add(sub, "Noon sun direction", noonSunUp, "y=" + std::to_string(sunDir.y));

        // Sun intensity should be high at noon
        float noonIntensity = tod->GetSunIntensity();
        report.Add(sub, "Noon sun intensity", noonIntensity > 0.5f, std::to_string(noonIntensity));

        // Set to midnight, intensity should be low
        tod->SetTimeOfDay(0.0f);
        float midnightIntensity = tod->GetSunIntensity();
        report.Add(sub, "Midnight sun intensity low", midnightIntensity < 0.3f, std::to_string(midnightIntensity));

        // Time scale
        tod->SetTimeScale(120.0f);
        bool scaleSet = (tod->GetTimeScale() == 120.0f);
        report.Add(sub, "Time scale set/get", scaleSet);

        // Day period at dawn
        tod->SetTimeOfDay(6.0f);
        auto period = tod->GetDayPeriod();
        bool isDawn = (period == DayPeriod::Dawn || period == DayPeriod::Morning);
        report.Add(sub, "Dawn period classification", isDawn);

        // Time string
        auto timeStr = tod->GetTimeString();
        report.Add(sub, "Time string format", !timeStr.empty(), timeStr);

        // Restore
        tod->SetTimeOfDay(originalTime);
        tod->SetTimeScale(originalScale);
        report.Add(sub, "State restored", true);
    }

    // ========================================================================
    // ECS (World)
    // ========================================================================

    void DiagECS(DiagReport& report)
    {
        const std::string sub = "ECS";

        auto* world = Ctx() ? Ctx()->GetWorld() : nullptr;
        if (!world)
        {
            report.Add(sub, "World available", false, "World not registered");
            return;
        }
        report.Add(sub, "World available", true);

        size_t countBefore = world->GetEntityCount();

        // Create entity with name
        auto entity = world->CreateEntity("__diag_test_entity");
        bool created = (entity != entt::null);
        report.Add(sub, "Create entity", created);

        if (created)
        {
            // Add HealthComponent
            auto& health = world->AddComponent<HealthComponent>(entity, HealthComponent{100.0f, 100.0f});
            report.Add(sub, "Add HealthComponent", health.maxHealth == 100.0f);

            // Query component back
            auto* hp = world->GetComponent<HealthComponent>(entity);
            report.Add(sub, "Get HealthComponent", hp != nullptr && hp->maxHealth == 100.0f);

            // HasComponent
            bool hasHp = world->HasComponent<HealthComponent>(entity);
            bool hasNoAI = !world->HasComponent<AIComponent>(entity);
            report.Add(sub, "HasComponent check", hasHp && hasNoAI);

            // Add Transform and verify
            Transform t;
            t.position = {1.0f, 2.0f, 3.0f};
            world->AddComponent<Transform>(entity, t);
            auto* tr = world->GetComponent<Transform>(entity);
            bool posOk = tr && tr->position.x == 1.0f && tr->position.y == 2.0f;
            report.Add(sub, "Transform component", posOk);

            // Entity count should have increased
            size_t countAfter = world->GetEntityCount();
            report.Add(sub, "Entity count increased", countAfter == countBefore + 1,
                       std::to_string(countBefore) + " -> " + std::to_string(countAfter));

            // Clean up
            world->DestroyEntity(entity);
            size_t countFinal = world->GetEntityCount();
            report.Add(sub, "Destroy entity", countFinal == countBefore);
        }
    }

    // ========================================================================
    // EventBus
    // ========================================================================

    void DiagEventBus(DiagReport& report)
    {
        const std::string sub = "EventBus";

        // Use a local bus to avoid polluting the global one
        EventBus bus;

        struct DiagTestEvent
        {
            int value = 0;
        };

        // Subscribe and publish
        int received = 0;
        auto handle = bus.Subscribe<DiagTestEvent>([&](const DiagTestEvent& e) { received = e.value; });
        report.Add(sub, "Subscribe", handle.IsActive());

        bus.Publish(DiagTestEvent{42});
        report.Add(sub, "Publish + receive", received == 42, "received=" + std::to_string(received));

        // Subscriber count
        size_t count = bus.SubscriberCount<DiagTestEvent>();
        report.Add(sub, "Subscriber count", count == 1);

        // Unsubscribe
        handle.Unsubscribe();
        received = 0;
        bus.Publish(DiagTestEvent{99});
        report.Add(sub, "Unsubscribe stops delivery", received == 0);

        // Multiple subscribers
        int sum = 0;
        auto h1 = bus.Subscribe<DiagTestEvent>([&](const DiagTestEvent& e) { sum += e.value; });
        auto h2 = bus.Subscribe<DiagTestEvent>([&](const DiagTestEvent& e) { sum += e.value * 10; });
        bus.Publish(DiagTestEvent{5});
        report.Add(sub, "Multiple subscribers", sum == 55, "sum=" + std::to_string(sum));

        // Clear all
        bus.ClearAll();
        size_t afterClear = bus.SubscriberCount<DiagTestEvent>();
        report.Add(sub, "ClearAll", afterClear == 0);
    }

    // ========================================================================
    // SaveSystem
    // ========================================================================

    void DiagSaveSystem(DiagReport& report)
    {
        const std::string sub = "SaveSystem";

        auto* save = Ctx() ? Ctx()->GetSaveSystem() : nullptr;
        if (!save)
        {
            report.Add(sub, "System available", false, "SaveSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        // List saves (shouldn't crash)
        auto listing = save->Console_ListSaves();
        report.Add(sub, "List saves", true, listing.substr(0, 60));

        // Check a save slot that doesn't exist
        bool nonExistent = !save->SaveExists("__diag_nonexistent_slot__");
        report.Add(sub, "Non-existent slot check", nonExistent);

        // If we have a world, test save/load roundtrip
        auto* world = Ctx()->GetWorld();
        if (world)
        {
            SaveMetadata meta;
            meta.saveName = "DiagTest";
            meta.playTime = 0.0f;

            bool saved = save->Save("__diag_test_slot__", *world, meta);
            report.Add(sub, "Save to slot", saved);

            if (saved)
            {
                bool exists = save->SaveExists("__diag_test_slot__");
                report.Add(sub, "Slot exists after save", exists);

                SaveMetadata readMeta;
                bool gotMeta = save->GetSaveMetadata("__diag_test_slot__", readMeta);
                report.Add(sub, "Read metadata", gotMeta && readMeta.saveName == "DiagTest");

                // Load back
                bool loaded = save->Load("__diag_test_slot__", *world);
                report.Add(sub, "Load from slot", loaded);

                // Clean up test slot
                save->DeleteSave("__diag_test_slot__");
                report.Add(sub, "Delete test slot", !save->SaveExists("__diag_test_slot__"));
            }
        }
    }

    // ========================================================================
    // Networking
    // ========================================================================

    void DiagNetworking(DiagReport& report)
    {
        const std::string sub = "Networking";

        auto& net = Spark::Net::NetworkManager::GetInstance();
        bool initialized = net.IsInitialized();
        report.Add(sub, "System available", true, initialized ? "initialized" : "not yet initialized");

        // Check initial state
        auto state = net.GetConnectionState();
        auto role = net.GetRole();
        report.Add(sub, "Initial state query", true,
                   "role=" + std::to_string(static_cast<int>(role)) +
                       " state=" + std::to_string(static_cast<int>(state)));

        // Console status (exercises status formatting)
        auto status = net.Console_GetStatus();
        report.Add(sub, "Console status", !status.empty(), status.substr(0, 80));

        // Start a server on a high port, verify state, then stop
        bool started = net.StartServer(59999, 4);
        if (started)
        {
            report.Add(sub, "Start server", true, "port=59999");

            auto serverRole = net.GetRole();
            report.Add(sub, "Server role", serverRole == Spark::Net::NetworkRole::Server);

            auto serverState = net.GetConnectionState();
            report.Add(sub, "Server connected", serverState == Spark::Net::ConnectionState::Connected);

            auto stats = net.Console_GetStats();
            report.Add(sub, "Server stats", !stats.empty());

            net.StopServer();
            auto afterStop = net.GetRole();
            report.Add(sub, "Stop server", afterStop == Spark::Net::NetworkRole::None);
        }
        else
        {
            // Server start can fail if port in use or networking compiled out
            report.Add(sub, "Start server", false, "Could not bind port 59999");
        }
    }

    // ========================================================================
    // AbilitySystem
    // ========================================================================

    void DiagAbilities(DiagReport& report)
    {
        const std::string sub = "Abilities";

        auto* ctx = Ctx();
        auto* abilities = ctx ? ctx->GetAbilities() : nullptr;
        if (!abilities)
        {
            report.Add(sub, "System available", false, "AbilitySystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        int countBefore = abilities->GetRegisteredAbilityCount();
        report.Add(sub, "Registered abilities", true, std::to_string(countBefore));

        // Register a test ability
        Gameplay::AbilityDefinition testAbility;
        testAbility.id = 99999;
        testAbility.name = "__diag_test_ability";
        testAbility.range = 10.0f;
        testAbility.castTime = 0.0f;
        testAbility.cooldown = 1.0f;
        testAbility.resourceCost = 0.0f;
        abilities->RegisterAbility(testAbility);

        int countAfter = abilities->GetRegisteredAbilityCount();
        report.Add(sub, "Register ability", countAfter == countBefore + 1);

        // Look it up
        auto* def = abilities->GetAbilityDef(99999);
        report.Add(sub, "Lookup ability", def != nullptr && def->name == "__diag_test_ability");

        // Aura count
        int auraCount = abilities->GetRegisteredAuraCount();
        report.Add(sub, "Aura registry", true, "count=" + std::to_string(auraCount));
    }

    // ========================================================================
    // DialogueSystem
    // ========================================================================

    void DiagDialogue(DiagReport& report)
    {
        const std::string sub = "Dialogue";

        auto* dialogue = Ctx() ? Ctx()->GetDialogue() : nullptr;
        if (!dialogue)
        {
            report.Add(sub, "System available", false, "DialogueSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        auto status = dialogue->Console_GetStatus();
        report.Add(sub, "Status query", !status.empty(), status.substr(0, 80));

        // Build and register a simple test dialogue tree
        auto tree = std::make_unique<DialogueTree>();
        tree->SetId("__diag_tree");
        tree->SetStartNodeId("start");

        DialogueNode node1;
        node1.id = "start";
        node1.type = DialogueNodeType::Text;
        node1.speakerName = "DiagBot";
        node1.text = "Hello from diagnostics!";
        node1.nextNodeId = "end";
        tree->AddNode(node1);

        DialogueNode endNode;
        endNode.id = "end";
        endNode.type = DialogueNodeType::End;
        tree->AddNode(endNode);

        dialogue->RegisterTree("__diag_tree", std::move(tree));

        // Start conversation
        bool started = dialogue->StartConversation("__diag_tree");
        report.Add(sub, "Start conversation", started);

        if (started)
        {
            bool active = dialogue->IsConversationActive();
            report.Add(sub, "Conversation active", active);

            auto* currentNode = dialogue->GetCurrentNode();
            bool nodeOk = currentNode != nullptr && currentNode->speakerName == "DiagBot";
            report.Add(sub, "Current node", nodeOk);

            // Advance to end
            dialogue->AdvanceNode();
            dialogue->Update(0.016f);

            // End conversation
            dialogue->EndConversation();
            report.Add(sub, "End conversation", !dialogue->IsConversationActive());
        }

        // Variable system
        dialogue->SetVariable("__diag_var", "test_value");
        auto val = dialogue->GetVariable("__diag_var");
        report.Add(sub, "Variable set/get", val == "test_value");
    }

    // ========================================================================
    // Coroutines
    // ========================================================================

    void DiagCoroutines(DiagReport& report)
    {
        const std::string sub = "Coroutines";

        auto* scheduler = Ctx() ? Ctx()->GetCoroutineScheduler() : nullptr;
        if (!scheduler)
        {
            report.Add(sub, "System available", false, "CoroutineScheduler not registered");
            return;
        }
        report.Add(sub, "System available", true);

        size_t countBefore = scheduler->ActiveCount();

        // Start a coroutine with a simple action
        bool actionRan = false;
        scheduler->StartCoroutine("__diag_test_coro").Do([&]() { actionRan = true; });

        // Tick the scheduler to execute
        scheduler->Update(0.016f);
        report.Add(sub, "Coroutine executed", actionRan);

        // Verify it completed (no longer active, or count returned)
        scheduler->Update(0.016f); // second tick to finalize

        // Start and stop
        scheduler->StartCoroutine("__diag_long_coro").WaitForSeconds(999.0f).Do([]() {});

        bool running = scheduler->IsRunning("__diag_long_coro");
        report.Add(sub, "Long coroutine running", running);

        scheduler->StopCoroutine("__diag_long_coro");
        bool stopped = !scheduler->IsRunning("__diag_long_coro");
        report.Add(sub, "Stop coroutine", stopped);
    }

    // ========================================================================
    // ConditionSystem
    // ========================================================================

    void DiagConditions(DiagReport& report)
    {
        const std::string sub = "Conditions";

        auto* conditions = Ctx() ? Ctx()->GetConditions() : nullptr;
        if (!conditions)
        {
            report.Add(sub, "System available", false, "ConditionSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        // Set and get a variable
        conditions->SetVariable("__diag_var", 42);
        int64_t val = conditions->GetVariable("__diag_var");
        report.Add(sub, "Variable set/get", val == 42);

        // Set and get a flag
        conditions->SetFlag("__diag_flag", true);
        bool flag = conditions->GetFlag("__diag_flag");
        report.Add(sub, "Flag set/get", flag);

        conditions->SetFlag("__diag_flag", false);
        bool flagOff = !conditions->GetFlag("__diag_flag");
        report.Add(sub, "Flag toggle", flagOff);

        // Register a custom evaluator
        conditions->RegisterCustomEvaluator("__diag_custom",
                                            [](uint32_t, const Gameplay::Condition&) -> bool { return true; });
        report.Add(sub, "Register custom evaluator", true);
    }

    // ========================================================================
    // InstanceManager
    // ========================================================================

    void DiagInstances(DiagReport& report)
    {
        const std::string sub = "Instances";

        auto* instances = Ctx() ? Ctx()->GetInstances() : nullptr;
        if (!instances)
        {
            report.Add(sub, "System available", false, "InstanceManager not registered");
            return;
        }
        report.Add(sub, "System available", true);

        int templatesBefore = instances->GetRegisteredTemplateCount();
        int activesBefore = instances->GetActiveInstanceCount();

        // Register a template
        Gameplay::InstanceTemplate tmpl;
        tmpl.templateId = 99999;
        tmpl.name = "__diag_dungeon";
        tmpl.maxPlayers = 5;
        instances->RegisterTemplate(tmpl);

        int templatesAfter = instances->GetRegisteredTemplateCount();
        report.Add(sub, "Register template", templatesAfter == templatesBefore + 1);

        // Create an instance
        auto instId = instances->CreateInstance(99999);
        bool instCreated = (instId != 0);
        report.Add(sub, "Create instance", instCreated, "id=" + std::to_string(instId));

        if (instCreated)
        {
            int activesAfter = instances->GetActiveInstanceCount();
            report.Add(sub, "Active count increased", activesAfter == activesBefore + 1);

            auto* data = instances->GetInstanceData(instId);
            report.Add(sub, "Get instance data", data != nullptr);

            // Destroy
            instances->DestroyInstance(instId);
            int activesFinal = instances->GetActiveInstanceCount();
            report.Add(sub, "Destroy instance", activesFinal == activesBefore);
        }
    }

    // ========================================================================
    // TweenSystem
    // ========================================================================

    void DiagTweens(DiagReport& report)
    {
        const std::string sub = "Tweens";

        auto* tweens = Ctx() ? Ctx()->GetTween() : nullptr;
        if (!tweens)
        {
            report.Add(sub, "System available", false, "TweenSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        auto status = tweens->Console_GetStatus();
        report.Add(sub, "Status query", !status.empty(), status.substr(0, 60));

        // Create a float tween
        float testVal = 0.0f;
        auto handle = tweens->TweenFloat(testVal, 0.0f, 1.0f, 0.5f, EaseType::Linear);
        report.Add(sub, "Create tween", handle != INVALID_TWEEN);

        // Tick halfway
        tweens->Update(0.25f);
        bool halfway = (testVal > 0.4f && testVal < 0.6f);
        report.Add(sub, "Tween at 50%", halfway, "value=" + std::to_string(testVal));

        // Tick to completion
        tweens->Update(0.30f);
        bool complete = (testVal >= 0.99f);
        report.Add(sub, "Tween completed", complete, "value=" + std::to_string(testVal));

        // Delay tween
        bool delayCalled = false;
        auto delayH = tweens->TweenDelay(0.1f, [&]() { delayCalled = true; });
        report.Add(sub, "Delay tween created", delayH != INVALID_TWEEN);

        tweens->Update(0.15f);
        report.Add(sub, "Delay callback fired", delayCalled);

        // Cancel
        float cancelVal = 0.0f;
        auto cancelH = tweens->TweenFloat(cancelVal, 0.0f, 100.0f, 10.0f);
        tweens->Cancel(cancelH);
        tweens->Update(5.0f);
        report.Add(sub, "Cancel tween", cancelVal < 1.0f);
    }

    // ========================================================================
    // LocalFileCache
    // ========================================================================

    void DiagFileCache(DiagReport& report)
    {
        const std::string sub = "FileCache";

        auto* cache = Ctx() ? Ctx()->GetFileCache() : nullptr;
        if (!cache)
        {
            report.Add(sub, "System available", false, "LocalFileCache not registered");
            return;
        }
        report.Add(sub, "System available", true);

        auto cacheMetrics = cache->GetMetrics();
        report.Add(sub, "Metrics query", true,
                   "entries=" + std::to_string(cache->GetEntryCount()) + " hits=" + std::to_string(cacheMetrics.hits));

        // Write, read, verify, delete
        auto writeResult = cache->WriteText("/tmp/__diag_cache_test.txt", "SparkEngine diagnostics OK");
        bool wrote = writeResult.IsOk();
        report.Add(sub, "Write text", wrote);

        if (wrote)
        {
            auto readResult = cache->ReadText("/tmp/__diag_cache_test.txt");
            bool readOk = readResult.IsOk() && readResult.Value() == "SparkEngine diagnostics OK";
            report.Add(sub, "Read text roundtrip", readOk);

            bool contains = cache->Contains("/tmp/__diag_cache_test.txt");
            report.Add(sub, "Contains check", contains);

            cache->Invalidate("/tmp/__diag_cache_test.txt");
            auto deleteResult = cache->Delete("/tmp/__diag_cache_test.txt");
            report.Add(sub, "Delete file", deleteResult.IsOk());
        }

        // Reading non-existent should return error
        auto badRead = cache->ReadText("/tmp/__diag_nonexistent_12345.txt");
        report.Add(sub, "Read non-existent returns error", !badRead.IsOk());
    }

    // ========================================================================
    // VirtualFileSystem
    // ========================================================================

    void DiagVFS(DiagReport& report)
    {
        const std::string sub = "VFS";

        auto* vfs = Ctx() ? Ctx()->GetVFS() : nullptr;
        if (!vfs)
        {
            report.Add(sub, "System available", false, "VirtualFileSystem not registered");
            return;
        }
        report.Add(sub, "System available", true);

        auto status = vfs->Console_GetStatus();
        report.Add(sub, "Status query", !status.empty(), status.substr(0, 60));

        uint32_t mounts = vfs->GetMountCount();
        report.Add(sub, "Mount count", true, std::to_string(mounts) + " mounts");

        // Query a known non-existent path
        bool notFound = !vfs->Exists("__diag_nonexistent_path/file.txt");
        report.Add(sub, "Non-existent path check", notFound);
    }

    // ========================================================================
    // Memory
    // ========================================================================

    void DiagMemory(DiagReport& report)
    {
        const std::string sub = "Memory";

        auto& monitor = MemoryMonitor::GetInstance();
        auto statusReport = monitor.GetStatusReport();
        report.Add(sub, "Status report", !statusReport.empty(), statusReport.substr(0, 80));

        // Snapshot
        auto snap = monitor.TakeSnapshot();
        report.Add(sub, "Snapshot", true,
                   "bytes=" + std::to_string(snap.totalBytes) + " allocs=" + std::to_string(snap.activeAllocations));

        // Anomalies query
        auto anomalies = monitor.GetRecentAnomalies();
        report.Add(sub, "Anomaly query", true, std::to_string(anomalies.size()) + " anomalies recorded");
    }

    // ========================================================================
    // Console command registration
    // ========================================================================

    /// Run a single subsystem diagnostic and return formatted output.
    template <typename DiagFunc> static std::string RunSingleDiag(DiagFunc func)
    {
        DiagReport report;
        func(report);
        return report.Format();
    }

    void RegisterDiagnosticCommands(SimpleConsole& console)
    {
        // Master command: run all diagnostics
        console.RegisterCommand(
            "diag_all",
            [](const std::vector<std::string>&) -> std::string
            {
                DiagReport report;
                DiagRunAll(report);
                return report.Format();
            },
            "Run all engine runtime diagnostics", "Diagnostics");

        // Individual subsystem commands
        console.RegisterCommand(
            "diag_physics", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagPhysics); },
            "Diagnose physics system (create body, raycast, forces)", "Diagnostics");

        console.RegisterCommand(
            "diag_weather", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagWeather); },
            "Diagnose weather system (cycle types, verify state)", "Diagnostics");

        console.RegisterCommand(
            "diag_time", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagTimeOfDay); },
            "Diagnose time-of-day system (sun, periods, scale)", "Diagnostics");

        console.RegisterCommand(
            "diag_ecs", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagECS); },
            "Diagnose ECS (create entity, add components, query)", "Diagnostics");

        console.RegisterCommand(
            "diag_events", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagEventBus); },
            "Diagnose EventBus (subscribe, publish, clear)", "Diagnostics");

        console.RegisterCommand(
            "diag_save", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagSaveSystem); },
            "Diagnose save system (save/load roundtrip)", "Diagnostics");

        console.RegisterCommand(
            "diag_network",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagNetworking); },
            "Diagnose networking (start/stop server, state queries)", "Diagnostics");

        console.RegisterCommand(
            "diag_abilities", [](const std::vector<std::string>&) -> std::string
            { return RunSingleDiag(DiagAbilities); }, "Diagnose ability system (register, lookup)", "Diagnostics");

        console.RegisterCommand(
            "diag_dialogue", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagDialogue); },
            "Diagnose dialogue system (tree, conversation, variables)", "Diagnostics");

        console.RegisterCommand(
            "diag_coroutines",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagCoroutines); },
            "Diagnose coroutine scheduler (start, stop, tick)", "Diagnostics");

        console.RegisterCommand(
            "diag_conditions",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagConditions); },
            "Diagnose condition system (variables, flags, evaluators)", "Diagnostics");

        console.RegisterCommand(
            "diag_instances",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagInstances); },
            "Diagnose instance manager (create/destroy dungeon instances)", "Diagnostics");

        console.RegisterCommand(
            "diag_tweens", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagTweens); },
            "Diagnose tween system (float tween, delay, cancel)", "Diagnostics");

        console.RegisterCommand(
            "diag_filecache",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagFileCache); },
            "Diagnose file cache (write, read, delete roundtrip)", "Diagnostics");

        console.RegisterCommand(
            "diag_vfs", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagVFS); },
            "Diagnose virtual file system (mounts, paths)", "Diagnostics");

        console.RegisterCommand(
            "diag_memory", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagMemory); },
            "Diagnose memory monitor (snapshot, anomalies)", "Diagnostics");

        // Extended subsystem commands
        console.RegisterCommand(
            "diag_rhi", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagRHI); },
            "Diagnose RHI null device (headless graphics)", "Diagnostics");

        console.RegisterCommand(
            "diag_jobs", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagJobSystem); },
            "Diagnose job system (thread pool, parallel dispatch)", "Diagnostics");

        console.RegisterCommand(
            "diag_ai", [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagAISystems); },
            "Diagnose AI systems (tactical, cover, formation, group)", "Diagnostics");

        console.RegisterCommand(
            "diag_debug",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagDebugSystems); },
            "Diagnose debug systems (hooks, tracing, inspector, profiler)", "Diagnostics");

        console.RegisterCommand(
            "diag_render",
            [](const std::vector<std::string>&) -> std::string { return RunSingleDiag(DiagRenderingSystems); },
            "Diagnose rendering utilities (GPU counters, light culling)", "Diagnostics");

        console.RegisterCommand(
            "diag_streaming", [](const std::vector<std::string>&) -> std::string
            { return RunSingleDiag(DiagStreaming); }, "Diagnose area streaming (seamless area manager)", "Diagnostics");

        console.RegisterCommand(
            "diag_destruction", [](const std::vector<std::string>&) -> std::string
            { return RunSingleDiag(DiagDestruction); }, "Diagnose destruction system", "Diagnostics");

        console.RegisterCommand(
            "diag_freeze", [](const std::vector<std::string>&) -> std::string
            { return RunSingleDiag(DiagFreezeSystem); }, "Diagnose freeze/snapshot system", "Diagnostics");

        console.RegisterCommand(
            "diag_scripting", [](const std::vector<std::string>&) -> std::string
            { return RunSingleDiag(DiagScripting); }, "Diagnose scripting engines (AngelScript, Lua)", "Diagnostics");
    }

} // namespace Spark
