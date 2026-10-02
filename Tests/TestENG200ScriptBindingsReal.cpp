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
 * - Contact dispatch: AngelScriptEngine::ConnectEventBus() delivers the
 *   CollisionEvent / TriggerEnterEvent / TriggerExitEvent that the real
 *   PhysicsSystem publishes after a step to OnCollision / OnTriggerEnter /
 *   OnTriggerExit on both participants' scripts, with no game-module glue and
 *   no physics trigger callback installed (ScriptLifecycle_ENG200_Contact*).
 *
 * - Entity id 0 (the physics "no entity" id) never receives contacts, and a
 *   body asleep inside a sensor does not exit it.
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

namespace
{
    /// Reports every contact callback as a ScriptEvent "<kind>:<other>" stamped with the receiving entity.
    const char* const kContactProbeScript =
        "class ContactProbe\n"
        "{\n"
        "    void OnCollision(EntityID other) { fireEvent(\"collision:\" + other); }\n"
        "    void OnTriggerEnter(EntityID other) { fireEvent(\"enter:\" + other); }\n"
        "    void OnTriggerExit(EntityID other) { fireEvent(\"exit:\" + other); }\n"
        "}\n";

    /// ScriptEvents published by contact probes, in order.
    struct ContactLog
    {
        std::vector<Spark::ScriptEvent> events;
        Spark::SubscriptionHandle subscription;

        explicit ContactLog(Spark::EventBus& bus)
            : subscription(
                  bus.Subscribe<Spark::ScriptEvent>([this](const Spark::ScriptEvent& e) { events.push_back(e); }))
        {
        }

        /// Number of events @p receiver's script fired with exactly @p name.
        size_t Count(EntityID receiver, const std::string& name) const
        {
            size_t count = 0;
            for (const auto& e : events)
            {
                if (e.sourceEntity == static_cast<uint32_t>(receiver) && e.eventName == name)
                {
                    ++count;
                }
            }
            return count;
        }

        /// Index of the first event @p receiver fired with @p name, or events.size().
        size_t IndexOf(EntityID receiver, const std::string& name) const
        {
            for (size_t i = 0; i < events.size(); ++i)
            {
                if (events[i].sourceEntity == static_cast<uint32_t>(receiver) && events[i].eventName == name)
                {
                    return i;
                }
            }
            return events.size();
        }
    };

    std::string ContactName(const char* kind, EntityID other)
    {
        return std::string(kind) + ":" + std::to_string(static_cast<uint32_t>(other));
    }
} // namespace

TEST(ScriptLifecycle_ENG200_ContactDispatchReachesBothScriptsAndDisconnects)
{
    Spark::EventBus bus;
    ContextEventBusScope busScope(&bus);
    ContactLog log(bus);

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kContactProbeScript, "ENG200ContactDirect"));

    // Entity 0 is the physics "no entity" id and never receives contacts (ScriptLifecycle_ENG200_ContactNoEntity*).
    (void)fx.world.CreateEntity("LevelRoot");
    const EntityID a = fx.world.CreateEntity("A");
    const EntityID b = fx.world.CreateEntity("B");
    const EntityID doomed = fx.world.CreateEntity("Doomed");
    const EntityID unscripted = fx.world.CreateEntity("Unscripted");
    EXPECT_TRUE(fx.engine.AttachScript(a, "ContactProbe", "ENG200ContactDirect"));
    EXPECT_TRUE(fx.engine.AttachScript(b, "ContactProbe", "ENG200ContactDirect"));
    EXPECT_TRUE(fx.engine.AttachScript(doomed, "ContactProbe", "ENG200ContactDirect"));
    fx.engine.ConnectEventBus(&bus);

    const auto id = [](EntityID e) { return static_cast<uint32_t>(e); };
    bus.Publish(Spark::CollisionEvent{id(a), id(b), 0.0f});
    bus.Publish(Spark::TriggerEnterEvent{id(a), id(b)});
    bus.Publish(Spark::TriggerExitEvent{id(a), id(b)});

    // Both participants, each told about the other, the first-named participant first.
    EXPECT_EQ(log.events.size(), static_cast<size_t>(6));
    EXPECT_EQ(log.Count(a, ContactName("collision", b)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(b, ContactName("collision", a)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(a, ContactName("enter", b)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(b, ContactName("enter", a)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(a, ContactName("exit", b)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(b, ContactName("exit", a)), static_cast<size_t>(1));
    EXPECT_TRUE(log.IndexOf(a, ContactName("collision", b)) < log.IndexOf(b, ContactName("collision", a)));

    // A destroyed entity's script and an unscripted participant are skipped; the live side still hears it.
    log.events.clear();
    fx.world.DestroyEntity(doomed);
    bus.Publish(Spark::CollisionEvent{id(doomed), id(a), 0.0f});
    bus.Publish(Spark::CollisionEvent{id(unscripted), id(b), 0.0f});
    EXPECT_EQ(log.events.size(), static_cast<size_t>(2));
    EXPECT_EQ(log.Count(a, ContactName("collision", doomed)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(b, ContactName("collision", unscripted)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(doomed, ContactName("collision", a)), static_cast<size_t>(0));

    // Disconnecting stops delivery; Shutdown() (fixture teardown) would too.
    log.events.clear();
    fx.engine.ConnectEventBus(nullptr);
    bus.Publish(Spark::CollisionEvent{id(a), id(b), 0.0f});
    bus.Publish(Spark::TriggerEnterEvent{id(a), id(b)});
    EXPECT_TRUE(log.events.empty());

    EXPECT_FALSE(fx.engine.IsScriptFaulted(a));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(b));
}

TEST(ScriptLifecycle_ENG200_ContactNoEntityIdNeverReachesEntityZero)
{
    Spark::EventBus bus;
    ContextEventBusScope busScope(&bus);
    ContactLog log(bus);

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kContactProbeScript, "ENG200ContactNoEntity"));

    // The first entity a World creates is entt::entity{0}, the same value physics uses for "no entity" (terrain,
    // props, ragdoll parts). Give it a probe script: contacts of unowned bodies must not reach it.
    const EntityID first = fx.world.CreateEntity("First");
    const EntityID player = fx.world.CreateEntity("Player");
    EXPECT_EQ(static_cast<uint32_t>(first), static_cast<uint32_t>(0));
    EXPECT_TRUE(fx.engine.AttachScript(first, "ContactProbe", "ENG200ContactNoEntity"));
    EXPECT_TRUE(fx.engine.AttachScript(player, "ContactProbe", "ENG200ContactNoEntity"));
    fx.engine.ConnectEventBus(&bus);

    // The player touches unowned bodies on either side of each event.
    const uint32_t playerId = static_cast<uint32_t>(player);
    bus.Publish(Spark::CollisionEvent{0, playerId, 0.0f});
    bus.Publish(Spark::CollisionEvent{playerId, 0, 0.0f});
    bus.Publish(Spark::TriggerEnterEvent{playerId, 0});
    bus.Publish(Spark::TriggerExitEvent{playerId, 0});
    // Two unowned bodies touching reach no script at all.
    bus.Publish(Spark::CollisionEvent{0, 0, 0.0f});

    // Only the player hears these, and it is told it touched the null entity, not entity 0.
    const EntityID none = entt::null;
    EXPECT_EQ(log.events.size(), static_cast<size_t>(4));
    EXPECT_EQ(log.Count(player, ContactName("collision", none)), static_cast<size_t>(2));
    EXPECT_EQ(log.Count(player, ContactName("enter", none)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(player, ContactName("exit", none)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(player, ContactName("collision", first)), static_cast<size_t>(0));
    for (const auto& e : log.events)
    {
        EXPECT_TRUE(e.sourceEntity != static_cast<uint32_t>(first));
    }

    fx.engine.ConnectEventBus(nullptr);
    EXPECT_FALSE(fx.engine.IsScriptFaulted(first));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(player));
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

namespace
{
    /// Box collider of the given half extents on @p entity.
    void AddBoxCollider(World& world, EntityID entity, const DirectX::XMFLOAT3& halfExtents)
    {
        ColliderComponent collider;
        collider.shape = ColliderComponent::Shape::Box;
        collider.halfExtents = halfExtents;
        world.AddComponent<ColliderComponent>(entity, collider);
    }
} // namespace

TEST(ScriptLifecycle_ENG200_ContactJoltCollisionReachesBothScripts)
{
    PhysicsRig rig;
    EXPECT_TRUE(rig.ready);
    if (!rig.ready)
    {
        return;
    }

    Spark::EventBus bus;
    ContextEventBusScope busScope(&bus);
    rig.physics->SetEventBus(&bus);
    ContactLog log(bus);

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kContactProbeScript, "ENG200ContactJolt"));
    fx.engine.ConnectEventBus(&bus);

    // A dynamic crate dropped 0.5 m onto a static floor. The level root takes entity 0 (the no-entity id).
    (void)fx.world.CreateEntity("LevelRoot");
    const EntityID floor = MakeBody(fx.world, "Floor", RigidBodyComponent::Type::Static);
    fx.world.GetComponent<Transform>(floor)->position = {0.0f, -0.5f, 0.0f};
    AddBoxCollider(fx.world, floor, {10.0f, 0.5f, 10.0f});
    const EntityID crate = MakeBody(fx.world, "Crate", RigidBodyComponent::Type::Dynamic);
    fx.world.GetComponent<Transform>(crate)->position = {0.0f, 1.0f, 0.0f};
    EXPECT_TRUE(fx.engine.AttachScript(floor, "ContactProbe", "ENG200ContactJolt"));
    EXPECT_TRUE(fx.engine.AttachScript(crate, "ContactProbe", "ENG200ContactJolt"));

    rig.sync->Update(fx.world, kTick);
    for (int i = 0; i < 120; ++i)
    {
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }

    std::printf("  collision events: crate=%zu floor=%zu crate y=%.3f\n",
                log.Count(crate, ContactName("collision", floor)), log.Count(floor, ContactName("collision", crate)),
                fx.world.GetComponent<Transform>(crate)->position.y);
    EXPECT_TRUE(log.Count(crate, ContactName("collision", floor)) >= 1);
    EXPECT_TRUE(log.Count(floor, ContactName("collision", crate)) >= 1);
    EXPECT_EQ(log.Count(crate, ContactName("collision", floor)), log.Count(floor, ContactName("collision", crate)));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(crate));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(floor));

    fx.engine.ConnectEventBus(nullptr);
    rig.physics->SetEventBus(nullptr);
}

TEST(ScriptLifecycle_ENG200_ContactJoltTriggerEnterExitReachesBothScripts)
{
    PhysicsRig rig;
    EXPECT_TRUE(rig.ready);
    if (!rig.ready)
    {
        return;
    }

    Spark::EventBus bus;
    ContextEventBusScope busScope(&bus);
    rig.physics->SetEventBus(&bus);
    ContactLog log(bus);
    std::vector<Spark::TriggerEnterEvent> enters;
    auto enterSubscription =
        bus.Subscribe<Spark::TriggerEnterEvent>([&enters](const Spark::TriggerEnterEvent& e) { enters.push_back(e); });

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kContactProbeScript, "ENG200ContactSensor"));
    fx.engine.ConnectEventBus(&bus);

    // A static sensor volume at the origin and a crate that free-falls through it. No trigger callback is
    // installed on PhysicsSystem: the EventBus alone must carry the overlap. The level root takes entity 0.
    (void)fx.world.CreateEntity("LevelRoot");
    const EntityID sensor = fx.world.CreateEntity("Sensor");
    fx.world.AddComponent<Transform>(sensor);
    RigidBodyComponent sensorBody;
    sensorBody.type = RigidBodyComponent::Type::Static;
    sensorBody.isTrigger = true;
    fx.world.AddComponent<RigidBodyComponent>(sensor, sensorBody);
    AddBoxCollider(fx.world, sensor, {1.0f, 1.0f, 1.0f});
    const EntityID crate = MakeBody(fx.world, "Crate", RigidBodyComponent::Type::Dynamic);
    fx.world.GetComponent<Transform>(crate)->position = {0.0f, 3.0f, 0.0f};
    EXPECT_TRUE(fx.engine.AttachScript(sensor, "ContactProbe", "ENG200ContactSensor"));
    EXPECT_TRUE(fx.engine.AttachScript(crate, "ContactProbe", "ENG200ContactSensor"));

    rig.sync->Update(fx.world, kTick);
    for (int i = 0; i < 180; ++i)
    {
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }

    const float crateY = fx.world.GetComponent<Transform>(crate)->position.y;
    std::printf("  trigger events: %zu script events, crate y=%.3f\n", log.events.size(), crateY);
    EXPECT_TRUE(crateY < -2.0f); // fell all the way through: the sensor applied no contact force

    // Exactly one enter and one exit per participant, enter first, each naming the other entity.
    EXPECT_EQ(log.Count(crate, ContactName("enter", sensor)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(sensor, ContactName("enter", crate)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(crate, ContactName("exit", sensor)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(sensor, ContactName("exit", crate)), static_cast<size_t>(1));
    EXPECT_TRUE(log.IndexOf(crate, ContactName("enter", sensor)) < log.IndexOf(crate, ContactName("exit", sensor)));
    EXPECT_EQ(log.Count(crate, ContactName("collision", sensor)), static_cast<size_t>(0));

    // The published roles follow the sensor, not Jolt's pair order.
    EXPECT_EQ(enters.size(), static_cast<size_t>(1));
    if (enters.size() == 1)
    {
        EXPECT_EQ(enters[0].entityId, static_cast<uint32_t>(crate));
        EXPECT_EQ(enters[0].triggerId, static_cast<uint32_t>(sensor));
    }
    EXPECT_FALSE(fx.engine.IsScriptFaulted(crate));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(sensor));

    fx.engine.ConnectEventBus(nullptr);
    rig.physics->SetEventBus(nullptr);
}

TEST(ScriptLifecycle_ENG200_ContactJoltSleepingBodyStaysInsideTrigger)
{
    PhysicsRig rig;
    EXPECT_TRUE(rig.ready);
    if (!rig.ready)
    {
        return;
    }

    Spark::EventBus bus;
    ContextEventBusScope busScope(&bus);
    rig.physics->SetEventBus(&bus);
    ContactLog log(bus);

    ScriptBindingFixture fx;
    EXPECT_TRUE(fx.ready);
    EXPECT_TRUE(fx.Compile(kContactProbeScript, "ENG200ContactSleep"));
    fx.engine.ConnectEventBus(&bus);

    // A crate dropped onto a static floor inside a large static sensor. Jolt stops reporting contacts once the
    // crate sleeps; the overlap must survive that instead of exiting while the crate is still inside.
    (void)fx.world.CreateEntity("LevelRoot");
    const EntityID floor = MakeBody(fx.world, "Floor", RigidBodyComponent::Type::Static);
    fx.world.GetComponent<Transform>(floor)->position = {0.0f, -0.5f, 0.0f};
    AddBoxCollider(fx.world, floor, {10.0f, 0.5f, 10.0f});
    const EntityID sensor = fx.world.CreateEntity("Sensor");
    fx.world.AddComponent<Transform>(sensor).position = {0.0f, 1.0f, 0.0f};
    RigidBodyComponent sensorBody;
    sensorBody.type = RigidBodyComponent::Type::Static;
    sensorBody.isTrigger = true;
    fx.world.AddComponent<RigidBodyComponent>(sensor, sensorBody);
    AddBoxCollider(fx.world, sensor, {3.0f, 2.0f, 3.0f});
    const EntityID crate = MakeBody(fx.world, "Crate", RigidBodyComponent::Type::Dynamic);
    fx.world.GetComponent<Transform>(crate)->position = {0.0f, 1.0f, 0.0f};
    EXPECT_TRUE(fx.engine.AttachScript(sensor, "ContactProbe", "ENG200ContactSleep"));
    EXPECT_TRUE(fx.engine.AttachScript(crate, "ContactProbe", "ENG200ContactSleep"));

    // Four seconds: the crate lands within the first second and Jolt's sleep threshold is 0.5 s at rest.
    rig.sync->Update(fx.world, kTick);
    for (int i = 0; i < 240; ++i)
    {
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }

    auto* crateBody = fx.world.GetComponent<RigidBodyComponent>(crate)->physicsBodyHandle.As<PhysicsBody>();
    EXPECT_TRUE(crateBody != nullptr);
    if (!crateBody)
    {
        return;
    }
    std::printf("  sleeping crate: active=%d y=%.3f, %zu script events\n", crateBody->IsActive() ? 1 : 0,
                fx.world.GetComponent<Transform>(crate)->position.y, log.events.size());
    EXPECT_FALSE(crateBody->IsActive()); // the scenario under test: the crate is asleep inside the sensor
    EXPECT_EQ(log.Count(crate, ContactName("enter", sensor)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(sensor, ContactName("enter", crate)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(crate, ContactName("exit", sensor)), static_cast<size_t>(0));
    EXPECT_EQ(log.Count(sensor, ContactName("exit", crate)), static_cast<size_t>(0));

    // Waking the crate in place (a small upward nudge) re-reports the overlap: still no exit and no second enter.
    crateBody->SetLinearVelocity({0.0f, 1.0f, 0.0f});
    for (int i = 0; i < 5; ++i)
    {
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }
    EXPECT_TRUE(crateBody->IsActive());
    EXPECT_EQ(log.Count(crate, ContactName("enter", sensor)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(crate, ContactName("exit", sensor)), static_cast<size_t>(0));

    // Moving the crate (which wakes it) out of the sensor ends the overlap exactly once, with no second enter.
    crateBody->SetPosition({20.0f, 1.0f, 0.0f});
    crateBody->SetLinearVelocity({0.0f, 0.0f, 0.0f});
    for (int i = 0; i < 10; ++i)
    {
        rig.physics->StepFixed(1);
        rig.sync->Update(fx.world, kTick);
    }
    EXPECT_EQ(log.Count(crate, ContactName("enter", sensor)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(crate, ContactName("exit", sensor)), static_cast<size_t>(1));
    EXPECT_EQ(log.Count(sensor, ContactName("exit", crate)), static_cast<size_t>(1));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(crate));
    EXPECT_FALSE(fx.engine.IsScriptFaulted(sensor));

    fx.engine.ConnectEventBus(nullptr);
    rig.physics->SetEventBus(nullptr);
}

#endif // SPARK_TEST_HAS_PHYSICS

#endif // SPARK_ANGELSCRIPT_SUPPORT
