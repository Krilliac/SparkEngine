/**
 * @file EngineSdkStateValidation.h
 * @brief The engine's implementation of the public Spark::IStateValidation SDK interface.
 *
 * Game modules reach it through IEngineContext::GetStateValidation() instead of
 * including the private Utils/InvalidStateDetector.h. The calls run in the host
 * image, so they reach the host's InvalidStateDetector, the one the gameplay
 * lifecycle initializes and ticks.
 *
 * Contract:
 * - Thread affinity: game thread (module OnLoad/OnUnload); the detector is
 *   not synchronized and runs its rules on the game thread.
 * - Ownership: a member of EngineContext; stateless, lives as long as it.
 * - Allocation: copies the name and category into the detector's rule record.
 * - Registration owner: the call carries no owner token, so the detector
 *   attributes it to the module whose lifecycle callback is running
 *   (ModuleManager's ScopedRegistrationOwner), exactly like a module calling the
 *   detector directly. RemoveRulesByCategory is scoped to that owner the same way.
 */

#pragma once

#include <Spark/IStateValidation.h>

class EngineSdkStateValidation final : public Spark::IStateValidation
{
  public:
    bool AddRule(std::string_view name, std::string_view category, Spark::StateViolationSeverity severity,
                 Spark::StateCheckFn check) override;
    void RemoveRulesByCategory(std::string_view category) override;
};
