/**
 * @file TestENG200ScriptRuntimeReal.cpp
 * @brief ENG-200 regression coverage for the engine-owned ECS script system.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Core/EngineContext.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Engine/ECS/Components.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/Scripting/AngelScriptEngine.h"

#include <cmath>
#include <memory>

namespace
{
    struct ProductionLifecycleGuard
    {
        EngineContext& context;
        World& world;

        ~ProductionLifecycleGuard()
        {
            AngelScriptEngine::BindWorld(nullptr);
            Spark::Core::Lifecycle::ShutdownGameplaySystemsImpl();
            Spark::Core::Lifecycle::ShutdownDebugSystemsImpl();
            world.GetRegistry().clear();
            context.SetScriptEngine(nullptr);
            context.SetEventBus(nullptr);
            context.SetWorld(nullptr);
        }
    };
} // namespace

TEST(ScriptLifecycle_ENG200_EngineSystemOwnsStartAndUpdateOncePerFrame)
{
    EngineContext::SetOwned(std::make_unique<EngineContext>());
    EngineContext& context = *EngineContext::Get();
    static World world;
    static Spark::EventBus eventBus;
    context.SetWorld(&world);
    context.SetEventBus(&eventBus);
    ProductionLifecycleGuard cleanup{context, world};
    ASSERT_TRUE(Spark::Core::Lifecycle::InitializeDebugSystemsImpl());
    ASSERT_TRUE(Spark::Core::Lifecycle::InitializeGameplaySystemsImpl());
    AngelScriptEngine* scriptEngine = context.GetScriptEngine();
    ASSERT_TRUE(scriptEngine != nullptr);
    AngelScriptEngine::BindWorld(&world);

    const EntityID mover = world.CreateEntity("SystemMover");
    world.AddComponent<Transform>(mover);
    auto& script = world.AddComponent<Script>(mover);
    script.className = "SystemMoverScript";
    script.moduleName = "ENG200SystemRuntime";

    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_FALSE(script.started);

    constexpr const char* source = "class SystemMoverScript\n"
                                   "{\n"
                                   "    void Start()\n"
                                   "    {\n"
                                   "        EntityID self = getEntityByName(\"SystemMover\");\n"
                                   "        Vector3 p = getPosition(self);\n"
                                   "        p.x += 10.0f;\n"
                                   "        setPosition(self, p);\n"
                                   "    }\n"
                                   "    void Update(float dt)\n"
                                   "    {\n"
                                   "        EntityID self = getEntityByName(\"SystemMover\");\n"
                                   "        Vector3 p = getPosition(self);\n"
                                   "        p.x += dt;\n"
                                   "        setPosition(self, p);\n"
                                   "    }\n"
                                   "}\n";
    ASSERT_TRUE(scriptEngine->CompileScriptFromString(source, "ENG200SystemRuntime"));
    ASSERT_TRUE(scriptEngine->AttachScript(mover, script.className, script.moduleName));

    // A reload can happen while the world is paused, before the first lifecycle
    // tick, and while another world is temporarily bound by a host command. The
    // replacement instance must still receive exactly one Start().
    World unrelatedWorld;
    script.enabled = false;
    AngelScriptEngine::BindWorld(&unrelatedWorld);
    ASSERT_TRUE(scriptEngine->HotReloadModuleFromSource("ENG200SystemRuntime", source));
    AngelScriptEngine::BindWorld(&world);

    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_FALSE(script.started);
    EXPECT_NEAR(world.GetComponent<Transform>(mover)->position.x, 0.0f, 1e-5f);

    script.enabled = true;
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_NEAR(world.GetComponent<Transform>(mover)->position.x, 10.5f, 1e-5f);

    script.enabled = false;
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_NEAR(world.GetComponent<Transform>(mover)->position.x, 10.5f, 1e-5f);

    script.enabled = true;
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_NEAR(world.GetComponent<Transform>(mover)->position.x, 11.0f, 1e-5f);

    scriptEngine->DetachScript(mover);
    ASSERT_TRUE(scriptEngine->AttachScript(mover, script.className, script.moduleName));
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_NEAR(world.GetComponent<Transform>(mover)->position.x, 21.5f, 1e-5f);

    const EntityID unrelatedMover = unrelatedWorld.CreateEntity("UnrelatedMover");
    auto& unrelatedScript = unrelatedWorld.AddComponent<Script>(unrelatedMover);
    unrelatedScript.started = false;
    AngelScriptEngine::BindWorld(&unrelatedWorld);
    ASSERT_TRUE(scriptEngine->HotReloadModuleFromSource("ENG200SystemRuntime", source));
    EXPECT_FALSE(unrelatedScript.started);
    AngelScriptEngine::BindWorld(&world);
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_NEAR(world.GetComponent<Transform>(mover)->position.x, 22.0f, 1e-5f);

    world.DestroyEntity(mover);
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_EQ(scriptEngine->GetAttachedScriptCount(), static_cast<std::size_t>(0));

    // A real script event can synchronously destroy its own ECS component.
    // Start must not leave a dangling component reference or dispatch Update.
    ASSERT_TRUE(
        scriptEngine->CompileScriptFromString("class DestroyOnStart { void Start() { fireEvent(\"destroy-self\"); } "
                                              "void Update(float dt) { fireEvent(\"unexpected-update\"); } }",
                                              "ENG200DestroyOnStart"));
    const EntityID transient = world.CreateEntity("TransientScript");
    world.AddComponent<Script>(transient);
    int unexpectedUpdates = 0;
    auto subscription = eventBus.Subscribe<Spark::ScriptEvent>(
        [&](const Spark::ScriptEvent& event)
        {
            if (event.eventName == "destroy-self")
            {
                world.DestroyEntity(static_cast<EntityID>(event.sourceEntity));
            }
            else if (event.eventName == "unexpected-update")
            {
                ++unexpectedUpdates;
            }
        });
    ASSERT_TRUE(scriptEngine->AttachScript(transient, "DestroyOnStart", "ENG200DestroyOnStart"));
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_FALSE(world.GetRegistry().valid(transient));
    EXPECT_EQ(unexpectedUpdates, 0);
    Spark::Core::Lifecycle::UpdateGameplaySystemsImpl(0.5f);
    EXPECT_EQ(scriptEngine->GetAttachedScriptCount(), static_cast<std::size_t>(0));

    AngelScriptEngine::BindWorld(nullptr);
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
