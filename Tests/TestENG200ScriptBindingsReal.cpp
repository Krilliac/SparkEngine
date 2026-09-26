/**
 * @file TestENG200ScriptBindingsReal.cpp
 * @brief ENG-200: real physics and event bindings exposed to AngelScript.
 *
 * Drives the production AngelScriptEngine against production subsystems:
 *
 * - applyForce()/getSpeed() go through the entity's RigidBodyComponent to the
 *   Jolt body PhysicsUpdateSystem created, and the world is advanced with
 *   PhysicsSystem::StepFixed(), so a script-issued force must move the entity.
 * - fireEvent() publishes Spark::ScriptEvent on the EngineContext EventBus,
 *   stamped with the entity whose script fired it.
 *
 * Both bindings were previously no-op or log-only; every assertion here fails
 * against those versions.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "Core/EngineContext.h"
#include "Engine/ECS/Systems/ECSystems.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/Scripting/AngelScriptEngine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
    /// Last debugTrace() output, used to read values computed inside scripts.
    std::string g_lastTraceOutput;

    void CaptureTrace(uint32_t /*nodeId*/, const char* /*nodeName*/, const char* output)
    {
        g_lastTraceOutput = output ? output : "";
    }

    /// A real, initialized AngelScriptEngine bound to a fresh World.
    struct ScriptBindingFixture
    {
        AngelScriptEngine engine;
        World world;
        bool ready = false;

        ScriptBindingFixture()
        {
            g_lastTraceOutput.clear();
            ASSetDebugTraceCallback(&CaptureTrace);
            ready = engine.Initialize();
            AngelScriptEngine::BindWorld(&world);
        }

        ~ScriptBindingFixture()
        {
            AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
            ASSetDebugTraceCallback(nullptr);
        }

        ScriptBindingFixture(const ScriptBindingFixture&) = delete;
        ScriptBindingFixture& operator=(const ScriptBindingFixture&) = delete;

        bool Compile(const char* source, const char* moduleName)
        {
            const bool compiled = engine.CompileScriptFromString(source, moduleName);
            if (!compiled)
            {
                std::printf("  compile diagnostic: %s\n", engine.GetLastError().c_str());
            }
            return compiled;
        }
    };

    /**
     * Registers @p bus as the EngineContext EventBus for the fixture's lifetime
     * and restores whatever was registered before (other tests share the
     * process-wide context).
     */
    struct ContextEventBusScope
    {
        EngineContext* context = nullptr;
        Spark::EventBus* previous = nullptr;

        explicit ContextEventBusScope(Spark::EventBus* bus)
        {
            if (!EngineContext::Get())
            {
                EngineContext::SetOwned(std::make_unique<EngineContext>());
            }
            context = EngineContext::Get();
            if (context)
            {
                previous = context->GetEventBus();
                context->SetEventBus(bus);
            }
        }

        ~ContextEventBusScope()
        {
            if (context)
            {
                context->SetEventBus(previous);
            }
        }

        ContextEventBusScope(const ContextEventBusScope&) = delete;
        ContextEventBusScope& operator=(const ContextEventBusScope&) = delete;
    };

    // Fires from the constructor, Start() and Update() so each dispatch path
    // is checked for the source entity stamp.
    const char* const kEventScript = "class Emitter\n"
                                     "{\n"
                                     "    Emitter() { fireEvent(\"Constructed\"); }\n"
                                     "    void Start() { fireEvent(\"DoorOpened\"); }\n"
                                     "    void Update(float dt) { fireEvent(\"Tick\"); }\n"
                                     "}\n";
} // namespace

TEST(ScriptBindings_ENG200_FireEventPublishesScriptEventWithSourceEntity)
{
    Spark::EventBus bus;
    ContextEventBusScope busScope(&bus);
    EXPECT_TRUE(busScope.context != nullptr);

    std::vector<Spark::ScriptEvent> received;
    auto subscription =
        bus.Subscribe<Spark::ScriptEvent>([&received](const Spark::ScriptEvent& e) { received.push_back(e); });

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kEventScript, "ENG200Events"));

    const EntityID first = fx.world.CreateEntity("EmitterA");
    const EntityID second = fx.world.CreateEntity("EmitterB");
    EXPECT_TRUE(fx.engine.AttachScript(first, "Emitter", "ENG200Events"));
    EXPECT_TRUE(fx.engine.AttachScript(second, "Emitter", "ENG200Events"));
    fx.engine.CallStart(first);
    fx.engine.CallUpdate(second, 0.016f);

    EXPECT_EQ(received.size(), static_cast<size_t>(4));
    if (received.size() == 4)
    {
        EXPECT_TRUE(received[0].eventName == "Constructed");
        EXPECT_EQ(received[0].sourceEntity, static_cast<uint32_t>(first));
        EXPECT_TRUE(received[1].eventName == "Constructed");
        EXPECT_EQ(received[1].sourceEntity, static_cast<uint32_t>(second));
        EXPECT_TRUE(received[2].eventName == "DoorOpened");
        EXPECT_EQ(received[2].sourceEntity, static_cast<uint32_t>(first));
        EXPECT_TRUE(received[3].eventName == "Tick");
        EXPECT_EQ(received[3].sourceEntity, static_cast<uint32_t>(second));
    }

    EXPECT_FALSE(fx.engine.IsScriptFaulted(first));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(second));

    // Outside any script callback there is no executing entity.
    EXPECT_TRUE(AngelScriptEngine::GetExecutingEntity() == entt::null);

    fx.engine.DetachScript(first);
    fx.engine.DetachScript(second);
}

TEST(ScriptBindings_ENG200_FireEventWithoutEventBusDropsSafely)
{
    ContextEventBusScope busScope(nullptr);

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kEventScript, "ENG200EventsNoBus"));

    const EntityID entity = fx.world.CreateEntity("Emitter");
    EXPECT_TRUE(fx.engine.AttachScript(entity, "Emitter", "ENG200EventsNoBus"));
    fx.engine.CallStart(entity);
    fx.engine.CallUpdate(entity, 0.016f);

    // A missing bus is a configuration problem, not a script fault.
    EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
    fx.engine.DetachScript(entity);
}

#ifdef SPARK_TEST_HAS_PHYSICS

#include "Physics/PhysicsBody.h"
#include "Physics/PhysicsSystem.h"

namespace
{
    constexpr float kTick = 1.0f / 60.0f;

    /**
     * A real Jolt PhysicsSystem registered on EngineContext (PhysicsBody resolves
     * its world there) plus the production PhysicsUpdateSystem that creates
     * bodies from RigidBodyComponent. Heap-allocated with explicit teardown, as
     * in TestMOD380VehiclePhysicsReal.
     */
    struct PhysicsRig
    {
        std::unique_ptr<PhysicsSystem> physics = std::make_unique<PhysicsSystem>();
        std::unique_ptr<Spark::ECS::PhysicsUpdateSystem> sync;
        EngineContext* context = nullptr;
        PhysicsSystem* previousPhysics = nullptr;
        bool ready = false;

        PhysicsRig()
        {
            if (!EngineContext::Get())
            {
                EngineContext::SetOwned(std::make_unique<EngineContext>());
            }
            context = EngineContext::Get();
            if (!context || FAILED(physics->Initialize()))
            {
                return;
            }
            previousPhysics = context->GetPhysics();
            context->SetPhysics(physics.get());
            physics->SetDeterministicSimulation(true);
            sync = std::make_unique<Spark::ECS::PhysicsUpdateSystem>(physics.get(), kTick);
            ready = true;
        }

        ~PhysicsRig()
        {
            sync.reset();
            physics->Shutdown();
            if (context)
            {
                context->SetPhysics(previousPhysics);
            }
        }

        PhysicsRig(const PhysicsRig&) = delete;
        PhysicsRig& operator=(const PhysicsRig&) = delete;
    };

    /// Adds a Transform and a zero-damping RigidBodyComponent of @p type.
    EntityID MakeBody(World& world, const char* name, RigidBodyComponent::Type type)
    {
        const EntityID entity = world.CreateEntity(name);
        world.AddComponent<Transform>(entity);
        RigidBodyComponent rb;
        rb.type = type;
        rb.mass = 2.0f;
        rb.linearDamping = 0.0f;
        world.AddComponent<RigidBodyComponent>(entity, rb);
        return entity;
    }

    /// Pushes 20 N along +X on the entity named "Pusher" every Update.
    const char* const kPushScript = "class Pusher\n"
                                    "{\n"
                                    "    void Update(float dt)\n"
                                    "    {\n"
                                    "        Vector3 f;\n"
                                    "        f.x = 20.0f;\n"
                                    "        f.y = 0.0f;\n"
                                    "        f.z = 0.0f;\n"
                                    "        applyForce(getEntityByName(\"Pusher\"), f);\n"
                                    "    }\n"
                                    "}\n";
} // namespace

TEST(ScriptBindings_ENG200_ApplyForceMovesDynamicJoltBody)
{
    PhysicsRig rig;
    EXPECT_TRUE(rig.ready);
    if (!rig.ready)
    {
        return;
    }

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    const EntityID pusher = MakeBody(fx.world, "Pusher", RigidBodyComponent::Type::Dynamic);
    // Identical body with no script: isolates the script's force from gravity.
    const EntityID control = MakeBody(fx.world, "Control", RigidBodyComponent::Type::Dynamic);
    fx.world.GetComponent<Transform>(control)->position = {0.0f, 0.0f, 10.0f};

    EXPECT_TRUE(fx.Compile(kPushScript, "ENG200Push"));
    EXPECT_TRUE(fx.engine.AttachScript(pusher, "Pusher", "ENG200Push"));

    // First sync creates the Jolt bodies from the components.
    rig.sync->Update(fx.world, kTick);
    constexpr int kTicks = 60;
    for (int i = 0; i < kTicks; ++i)
    {
        fx.engine.CallUpdate(pusher, kTick);
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }

    EXPECT_FALSE(fx.engine.IsScriptFaulted(pusher));

    // F = 20 N on 2 kg for 1 s: v = 10 m/s, x = 5 m (semi-implicit Euler lands slightly above).
    const auto& rb = *fx.world.GetComponent<RigidBodyComponent>(pusher);
    const float x = fx.world.GetComponent<Transform>(pusher)->position.x;
    std::printf("  pusher vx=%.3f x=%.3f\n", rb.linearVelocity.x, x);
    EXPECT_TRUE(std::fabs(rb.linearVelocity.x - 10.0f) < 0.5f);
    EXPECT_TRUE(x > 4.5f && x < 5.5f);

    EXPECT_TRUE(std::fabs(fx.world.GetComponent<Transform>(control)->position.x) < 1e-4f);
    fx.engine.DetachScript(pusher);
}

TEST(ScriptBindings_ENG200_ApplyForceIgnoresStaticBodiesAndBadInput)
{
    PhysicsRig rig;
    EXPECT_TRUE(rig.ready);
    if (!rig.ready)
    {
        return;
    }

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    const EntityID wall = MakeBody(fx.world, "Wall", RigidBodyComponent::Type::Static);
    const EntityID mover = MakeBody(fx.world, "Mover", RigidBodyComponent::Type::Dynamic);
    // Keep the two bodies apart so contact resolution cannot move the mover.
    fx.world.GetComponent<Transform>(wall)->position = {0.0f, 0.0f, -10.0f};
    const EntityID bare = fx.world.CreateEntity("Bare");
    fx.world.AddComponent<Transform>(bare);

    // A static body, an entity with no RigidBodyComponent, a stale id, and a
    // non-finite force on a dynamic body: all are ignored, none faults.
    const char* const script = "class Abuser\n"
                               "{\n"
                               "    void Update(float dt)\n"
                               "    {\n"
                               "        Vector3 f;\n"
                               "        f.x = 1000.0f;\n"
                               "        f.y = 0.0f;\n"
                               "        f.z = 0.0f;\n"
                               "        applyForce(getEntityByName(\"Wall\"), f);\n"
                               "        applyForce(getEntityByName(\"Bare\"), f);\n"
                               "        applyForce(123456, f);\n"
                               "        float big = 3.0e38f;\n"
                               "        Vector3 bad;\n"
                               "        bad.x = big * big;\n"
                               "        bad.y = 0.0f;\n"
                               "        bad.z = 0.0f;\n"
                               "        applyForce(getEntityByName(\"Mover\"), bad);\n"
                               "    }\n"
                               "}\n";
    EXPECT_TRUE(fx.Compile(script, "ENG200Abuse"));
    EXPECT_TRUE(fx.engine.AttachScript(bare, "Abuser", "ENG200Abuse"));

    rig.sync->Update(fx.world, kTick);
    for (int i = 0; i < 30; ++i)
    {
        fx.engine.CallUpdate(bare, kTick);
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }

    EXPECT_FALSE(fx.engine.IsScriptFaulted(bare));
    EXPECT_TRUE(std::fabs(fx.world.GetComponent<Transform>(wall)->position.x) < 1e-6f);
    EXPECT_TRUE(std::fabs(fx.world.GetComponent<Transform>(wall)->position.z + 10.0f) < 1e-6f);
    const float moverX = fx.world.GetComponent<Transform>(mover)->position.x;
    EXPECT_TRUE(std::isfinite(moverX) && std::fabs(moverX) < 1e-4f);
    fx.engine.DetachScript(bare);
}

TEST(ScriptBindings_ENG200_GetSpeedReadsLiveBodyVelocity)
{
    PhysicsRig rig;
    EXPECT_TRUE(rig.ready);
    if (!rig.ready)
    {
        return;
    }

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    const EntityID runner = MakeBody(fx.world, "Runner", RigidBodyComponent::Type::Dynamic);
    const EntityID bare = fx.world.CreateEntity("Bare");

    const char* const script =
        "class Speedometer\n"
        "{\n"
        "    void Update(float dt)\n"
        "    {\n"
        "        debugTrace(1, \"speed\", \"\" + getSpeed(getEntityByName(\"Runner\")) + \"|\" +\n"
        "                   getSpeed(getEntityByName(\"Bare\")));\n"
        "    }\n"
        "}\n";
    EXPECT_TRUE(fx.Compile(script, "ENG200Speed"));
    EXPECT_TRUE(fx.engine.AttachScript(bare, "Speedometer", "ENG200Speed"));

    rig.sync->Update(fx.world, kTick); // creates the body
    auto* body = fx.world.GetComponent<RigidBodyComponent>(runner)->physicsBodyHandle.As<PhysicsBody>();
    EXPECT_TRUE(body != nullptr);
    if (!body)
    {
        return;
    }
    // Set on the live body only: the component cache still reads zero, so a
    // 5 m/s answer proves the binding reads the simulation.
    body->SetLinearVelocity({3.0f, 4.0f, 0.0f});
    fx.engine.CallUpdate(bare, kTick);

    const std::string& out = g_lastTraceOutput;
    const auto bar = out.find('|');
    EXPECT_TRUE(bar != std::string::npos);
    if (bar != std::string::npos)
    {
        EXPECT_TRUE(std::fabs(std::stof(out.substr(0, bar)) - 5.0f) < 1e-3f);
        EXPECT_TRUE(std::fabs(std::stof(out.substr(bar + 1))) < 1e-6f);
    }
    EXPECT_FALSE(fx.engine.IsScriptFaulted(bare));
    fx.engine.DetachScript(bare);
}

#endif // SPARK_TEST_HAS_PHYSICS

#endif // SPARK_ANGELSCRIPT_SUPPORT
