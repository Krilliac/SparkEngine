/**
 * @file RacingPhysicsTestWorld.h
 * @brief Test support: a real Jolt PhysicsSystem published through the process EngineContext for Racing tests.
 *
 * SparkGameRacing vehicles and track colliders live in the engine's shared PhysicsSystem, which the module
 * reaches through Spark::IEngineContext::GetPhysics(), and PhysicsBody resolves its world through
 * EngineContext. This fixture initializes a PhysicsSystem, registers it in the process-wide EngineContext for
 * its lifetime (restoring the previous one afterwards), and hands that context to the Racing systems.
 * Declare it before the systems that use it so it is destroyed after them.
 */

#pragma once

#include "Core/EngineContext.h"
#include "Physics/PhysicsSystem.h"

#include <memory>

struct RacingPhysicsTestWorld
{
    std::unique_ptr<PhysicsSystem> physics = std::make_unique<PhysicsSystem>();
    EngineContext* context = nullptr;
    PhysicsSystem* previousPhysics = nullptr;
    bool ready = false;

    RacingPhysicsTestWorld()
    {
        // A standalone test run has no engine-owned context yet; create the process-wide one on demand.
        if (!EngineContext::Get())
            EngineContext::SetOwned(std::make_unique<EngineContext>());
        context = EngineContext::Get();
        if (!context || FAILED(physics->Initialize()))
            return;
        previousPhysics = context->GetPhysics();
        context->SetPhysics(physics.get());
        physics->SetDeterministicSimulation(true);
        ready = true;
    }

    ~RacingPhysicsTestWorld()
    {
        physics->Shutdown();
        if (context && ready)
            context->SetPhysics(previousPhysics);
    }

    RacingPhysicsTestWorld(const RacingPhysicsTestWorld&) = delete;
    RacingPhysicsTestWorld& operator=(const RacingPhysicsTestWorld&) = delete;

    /// The engine context the Racing systems bind to (its GetPhysics() is this world).
    Spark::IEngineContext* Context() { return context; }
};
