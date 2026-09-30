/**
 * @file IStateValidation.h
 * @brief ECS state-invariant rules game modules register through IEngineContext::GetStateValidation()
 *
 * The host runs one invalid-state detector (the engine's InvalidStateDetector)
 * that periodically scans the ECS world for impossible component combinations,
 * such as a dead entity that still has a target. A module adds its own
 * domain rules through the context it received in OnLoad instead of including
 * the engine-private Utils/InvalidStateDetector.h.
 *
 * ## Usage
 * @code
 *   #include <Spark/IStateValidation.h>
 *
 *   bool MyModule::OnLoad(Spark::IEngineContext* context)
 *   {
 *       if (Spark::IStateValidation* rules = context->GetStateValidation())
 *       {
 *           rules->AddRule("MyModule.DeadButMoving", "MyModule", Spark::StateViolationSeverity::Warning,
 *                          [](World& world, std::vector<Spark::StateViolation>& out) { ... });
 *       }
 *       return true;
 *   }
 *
 *   void MyModule::OnUnload()
 *   {
 *       if (Spark::IStateValidation* rules = m_context ? m_context->GetStateValidation() : nullptr)
 *           rules->RemoveRulesByCategory("MyModule");
 *   }
 * @endcode
 *
 * Contract:
 * - Thread affinity: game thread. Add and remove rules from OnLoad/OnUnload;
 *   the host runs every rule on the game thread during its periodic scan.
 * - Ownership: host-owned; the pointer stays valid until after the module's
 *   OnUnload returns. Do not cache it past OnUnload.
 * - Rule checks are std::functions whose code lives in the module image. A
 *   module must remove every category it added in OnUnload, before its image is
 *   unmapped. The host also removes a module's rules by owner on unload as a
 *   backstop, but that does not excuse the module.
 * - While the host runs a module's OnLoad/OnUnload it attributes rule
 *   registrations and category removals to that module, so RemoveRulesByCategory
 *   never removes a rule another module (or the engine) registered under the same
 *   category name. Rule names are not required to be unique: during a
 *   transactional hot reload the replacement image registers its rules before the
 *   outgoing image removes its own.
 * - A check receives the host's live ECS world. Reading components needs the ECS
 *   headers; the SDK only forward-declares ::World.
 * - ABI: std::function, std::string and std::vector cross the boundary. That is
 *   sound because the SDK ABI is exact-match (IsSDKCompatible) with the same
 *   toolchain and CRT on both sides (OD-02).
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class World;

namespace Spark
{

    /// Severity of a detected state violation.
    enum class StateViolationSeverity : uint8_t
    {
        Warning, ///< Suspicious but possibly transient (e.g. one-frame desync).
        Error,   ///< Likely bug — state should not persist.
        Critical ///< Definitely wrong — immediate investigation needed.
    };

    /// A single detected state violation.
    struct StateViolation
    {
        std::string ruleName;  ///< Which rule was violated.
        uint32_t entityId = 0; ///< Entity with the invalid state.
        std::string details;   ///< Human-readable description.
        StateViolationSeverity severity = StateViolationSeverity::Error;
    };

    /// Callback signature for a validation rule check.
    /// The rule iterates the World for its target components and appends any violations found.
    using StateCheckFn = std::function<void(::World&, std::vector<StateViolation>&)>;

    /**
     * @brief Abstract registry of ECS state-invariant rules for game modules
     *
     * Implemented by the host; obtained from IEngineContext::GetStateValidation().
     */
    class IStateValidation
    {
      public:
        virtual ~IStateValidation() = default;

        /**
         * @brief Register an enabled validation rule with the host detector
         * @param name     Rule identifier reported with each violation, e.g. "RTS.DeadUnitMoving"
         * @param category Grouping the module removes its rules by, usually the module name
         * @param severity Severity the host reports the rule's violations with
         * @param check    Scans the world and appends a StateViolation per offending entity
         * @return true when the rule is registered; false when the name, category or
         *         check is empty, or the host detector is not running
         */
        virtual bool AddRule(std::string_view name, std::string_view category, StateViolationSeverity severity,
                             StateCheckFn check) = 0;

        /**
         * @brief Remove this module's rules in @p category; unknown categories are ignored
         *
         * Call in OnUnload for every category the module added. While the host
         * runs a module's OnLoad/OnUnload, only that module's rules are removed.
         */
        virtual void RemoveRulesByCategory(std::string_view category) = 0;
    };

} // namespace Spark
