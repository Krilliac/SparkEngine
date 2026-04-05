/**
 * @file InvalidStateDetector.cpp
 * @brief Runtime detection of impossible/contradictory ECS component state combinations
 */

#include "InvalidStateDetector.h"
#include "LogMacros.h"
#include "SparkConsole.h"

#include "../Engine/ECS/Components.h"
#include "../Engine/ECS/Components/AIComponents.h"
#include "../Engine/ECS/Components/AdvancedPlacementComponents.h"
#include "../Engine/ECS/Components/FPSComponents.h"
#include "../Engine/ECS/Components/GameplayComponents.h"
#include "../Engine/ECS/Components/NetworkComponents.h"
#include "../Engine/ECS/Components/PhysicsComponents.h"
#include "../Engine/ECS/Components/PlacementComponents.h"

#include <algorithm>
#include <format>
#include <sstream>
#include <unordered_set>

namespace Spark
{

    InvalidStateDetector& InvalidStateDetector::GetInstance()
    {
        static InvalidStateDetector instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void InvalidStateDetector::Initialize()
    {
        if (m_initialized)
            return;

        m_rules.clear();
        m_violations.clear();
        m_totalChecks = 0;
        m_totalViolations = 0;
        m_timeSinceLastCheck = 0.0f;

        RegisterDefaultRules();
        m_initialized = true;

        SPARK_LOG_INFO(Spark::LogCategory::Core, "InvalidStateDetector initialized with %u rules",
                       static_cast<uint32_t>(m_rules.size()));
    }

    void InvalidStateDetector::Update(float dt)
    {
        if (!m_initialized || !m_config.enabled || !m_world)
            return;

        m_timeSinceLastCheck += dt;
        if (m_timeSinceLastCheck < m_config.checkIntervalSec)
            return;

        m_timeSinceLastCheck = 0.0f;
        RunChecks();
    }

    void InvalidStateDetector::Shutdown()
    {
        if (!m_initialized)
            return;

        if (m_totalViolations > 0)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core,
                           "InvalidStateDetector: %u total violations detected across %u checks", m_totalViolations,
                           m_totalChecks);
        }

        m_world = nullptr;
        m_rules.clear();
        m_violations.clear();
        m_initialized = false;
    }

    // =========================================================================
    // Rule Management
    // =========================================================================

    void InvalidStateDetector::AddRule(StateValidationRule rule)
    {
        m_rules.push_back(std::move(rule));
    }

    void InvalidStateDetector::SetRuleEnabled(const std::string& name, bool enabled)
    {
        for (auto& rule : m_rules)
        {
            if (rule.name == name)
            {
                rule.enabled = enabled;
                return;
            }
        }
    }

    // =========================================================================
    // Checks
    // =========================================================================

    void InvalidStateDetector::RunChecks()
    {
        ++m_totalChecks;

        std::vector<StateViolation> newViolations;
        for (const auto& rule : m_rules)
        {
            if (!rule.enabled)
                continue;
            rule.checkFn(*m_world, newViolations);
        }

        for (auto& v : newViolations)
        {
            ++m_totalViolations;

            if (m_config.logOnDetection)
            {
                if (v.severity == StateViolationSeverity::Critical)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "[StateViolation] %s entity=%u: %s", v.ruleName.c_str(),
                                    v.entityId, v.details.c_str());
                }
                else
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "[StateViolation] %s entity=%u: %s", v.ruleName.c_str(),
                                   v.entityId, v.details.c_str());
                }
            }

            // Ring buffer: drop oldest if full
            if (m_violations.size() >= m_config.maxViolationHistory)
            {
                m_violations.erase(m_violations.begin());
            }
            m_violations.push_back(std::move(v));
        }
    }

    // =========================================================================
    // Query
    // =========================================================================

    InvalidStateDetectorStatus InvalidStateDetector::GetStatus() const
    {
        InvalidStateDetectorStatus status;
        status.totalChecks = m_totalChecks;
        status.totalViolations = m_totalViolations;
        status.totalRules = static_cast<uint32_t>(m_rules.size());
        status.activeRules = static_cast<uint32_t>(
            std::count_if(m_rules.begin(), m_rules.end(), [](const auto& r) { return r.enabled; }));
        status.recentViolations = m_violations;
        return status;
    }

    // =========================================================================
    // Console Commands
    // =========================================================================

    void InvalidStateDetector::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "state.status",
            [](const std::vector<std::string>&) -> std::string
            {
                auto& det = GetInstance();
                auto status = det.GetStatus();
                std::stringstream ss;
                ss << "=== Invalid State Detector ===\n";
                ss << "  Enabled:     " << (det.m_config.enabled ? "yes" : "no") << "\n";
                ss << "  World:       " << (det.m_world ? "set" : "NOT SET") << "\n";
                ss << "  Rules:       " << status.activeRules << "/" << status.totalRules << " active\n";
                ss << "  Checks run:  " << status.totalChecks << "\n";
                ss << "  Violations:  " << status.totalViolations << " total\n";
                ss << "  Interval:    " << det.m_config.checkIntervalSec << "s\n";
                ss << "  Recent:      " << status.recentViolations.size() << " in buffer\n";
                return ss.str();
            },
            "Show invalid state detector status", "Diagnostics");

        console.RegisterCommand(
            "state.rules",
            [](const std::vector<std::string>& args) -> std::string
            {
                auto& det = GetInstance();

                // Handle enable/disable: state.rules <name> enable|disable
                if (args.size() >= 2)
                {
                    const std::string& name = args[0];
                    const std::string& action = args[1];
                    bool enable = (action == "enable");
                    det.SetRuleEnabled(name, enable);
                    return "Rule '" + name + "' " + (enable ? "enabled" : "disabled");
                }

                std::stringstream ss;
                ss << "=== Validation Rules ===\n";
                std::string lastCategory;
                for (const auto& rule : det.m_rules)
                {
                    if (rule.category != lastCategory)
                    {
                        ss << "\n  [" << rule.category << "]\n";
                        lastCategory = rule.category;
                    }
                    const char* sevStr = rule.severity == StateViolationSeverity::Critical ? "CRIT"
                                         : rule.severity == StateViolationSeverity::Error  ? "ERR "
                                                                                           : "WARN";
                    ss << "    " << (rule.enabled ? "[ON] " : "[OFF]") << " " << sevStr << " " << rule.name << "\n";
                }
                ss << "\nUsage: state.rules <name> enable|disable\n";
                return ss.str();
            },
            "List rules or toggle: state.rules [name enable|disable]", "Diagnostics");

        console.RegisterCommand(
            "state.violations",
            [](const std::vector<std::string>&) -> std::string
            {
                auto& det = GetInstance();
                if (det.m_violations.empty())
                    return std::string("No recent violations.");

                std::stringstream ss;
                ss << "=== Recent Violations (" << det.m_violations.size() << ") ===\n";
                for (const auto& v : det.m_violations)
                {
                    const char* sevStr = v.severity == StateViolationSeverity::Critical ? "CRIT"
                                         : v.severity == StateViolationSeverity::Error  ? "ERR "
                                                                                        : "WARN";
                    ss << "  [" << sevStr << "] " << v.ruleName << " entity=" << v.entityId << ": " << v.details
                       << "\n";
                }
                return ss.str();
            },
            "Show recent state violations", "Diagnostics");

        console.RegisterCommand(
            "state.config",
            [](const std::vector<std::string>&) -> std::string
            {
                auto& det = GetInstance();
                std::stringstream ss;
                ss << "=== State Detector Config ===\n";
                ss << "  enabled:           " << (det.m_config.enabled ? "true" : "false") << "\n";
                ss << "  checkIntervalSec:  " << det.m_config.checkIntervalSec << "\n";
                ss << "  maxViolationHistory: " << det.m_config.maxViolationHistory << "\n";
                ss << "  logOnDetection:    " << (det.m_config.logOnDetection ? "true" : "false") << "\n";
                return ss.str();
            },
            "Show state detector configuration", "Diagnostics");
    }

    // =========================================================================
    // Default Rules
    // =========================================================================

    void InvalidStateDetector::RegisterDefaultRules()
    {
        // -- Health rules --

        AddRule({"Health.DeadButPositive", "Health", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<HealthComponent>())
                     {
                         auto* h = w.GetComponent<HealthComponent>(entity);
                         if (h && h->isDead && h->health > 0.0f)
                         {
                             out.push_back({"Health.DeadButPositive", static_cast<uint32_t>(entity),
                                            "isDead=true but health=" + std::to_string(h->health),
                                            StateViolationSeverity::Error});
                         }
                     }
                 }});

        AddRule({"Health.AliveButZero", "Health", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<HealthComponent>())
                     {
                         auto* h = w.GetComponent<HealthComponent>(entity);
                         if (h && !h->isDead && h->health <= 0.0f)
                         {
                             out.push_back({"Health.AliveButZero", static_cast<uint32_t>(entity),
                                            "isDead=false but health=" + std::to_string(h->health),
                                            StateViolationSeverity::Error});
                         }
                     }
                 }});

        AddRule({"Health.DeathProcessedButAlive", "Health", StateViolationSeverity::Critical, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<HealthComponent>())
                     {
                         auto* h = w.GetComponent<HealthComponent>(entity);
                         if (h && h->deathProcessed && !h->isDead)
                         {
                             out.push_back({"Health.DeathProcessedButAlive", static_cast<uint32_t>(entity),
                                            "deathProcessed=true but isDead=false", StateViolationSeverity::Critical});
                         }
                     }
                 }});

        AddRule({"Health.DeadAINotInDeadState", "Health", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<HealthComponent, AIComponent>())
                     {
                         auto* h = w.GetComponent<HealthComponent>(entity);
                         auto* ai = w.GetComponent<AIComponent>(entity);
                         if (h && ai && h->isDead && ai->state != AIComponent::State::Dead)
                         {
                             out.push_back({"Health.DeadAINotInDeadState", static_cast<uint32_t>(entity),
                                            "HealthComponent.isDead=true but AI state is not Dead",
                                            StateViolationSeverity::Error});
                         }
                     }
                 }});

        // -- Physics rules --

        AddRule({"Physics.StaticWithVelocity", "Physics", StateViolationSeverity::Warning, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<RigidBodyComponent>())
                     {
                         auto* rb = w.GetComponent<RigidBodyComponent>(entity);
                         if (!rb || rb->type != RigidBodyComponent::Type::Static)
                             continue;
                         float speed = rb->linearVelocity.x * rb->linearVelocity.x +
                                       rb->linearVelocity.y * rb->linearVelocity.y +
                                       rb->linearVelocity.z * rb->linearVelocity.z;
                         if (speed > 0.01f)
                         {
                             out.push_back({"Physics.StaticWithVelocity", static_cast<uint32_t>(entity),
                                            "Static body has nonzero velocity", StateViolationSeverity::Warning});
                         }
                     }
                 }});

        AddRule({"Physics.DynamicZeroMass", "Physics", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<RigidBodyComponent>())
                     {
                         auto* rb = w.GetComponent<RigidBodyComponent>(entity);
                         if (rb && rb->type == RigidBodyComponent::Type::Dynamic && rb->mass <= 0.0f)
                         {
                             out.push_back({"Physics.DynamicZeroMass", static_cast<uint32_t>(entity),
                                            "Dynamic body has mass=" + std::to_string(rb->mass),
                                            StateViolationSeverity::Error});
                         }
                     }
                 }});

        // -- AI rules --

        AddRule({"AI.CombatNoTarget", "AI", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<AIComponent>())
                     {
                         auto* ai = w.GetComponent<AIComponent>(entity);
                         if (ai && ai->state == AIComponent::State::Combat && ai->targetEntity == entt::null)
                         {
                             out.push_back({"AI.CombatNoTarget", static_cast<uint32_t>(entity),
                                            "AI in Combat state with no target entity", StateViolationSeverity::Error});
                         }
                     }
                 }});

        AddRule({"AI.DeadWithTarget", "AI", StateViolationSeverity::Warning, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<AIComponent>())
                     {
                         auto* ai = w.GetComponent<AIComponent>(entity);
                         if (ai && ai->state == AIComponent::State::Dead && ai->targetEntity != entt::null)
                         {
                             out.push_back({"AI.DeadWithTarget", static_cast<uint32_t>(entity),
                                            "Dead AI still has targetEntity set", StateViolationSeverity::Warning});
                         }
                     }
                 }});

        AddRule({"AI.FleeingNoTarget", "AI", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<AIComponent>())
                     {
                         auto* ai = w.GetComponent<AIComponent>(entity);
                         if (ai && ai->state == AIComponent::State::Fleeing && ai->targetEntity == entt::null)
                         {
                             out.push_back({"AI.FleeingNoTarget", static_cast<uint32_t>(entity),
                                            "AI is Fleeing with no target entity", StateViolationSeverity::Error});
                         }
                     }
                 }});

        // -- Interaction rules --

        AddRule({"Interaction.DestroyedWithUses", "Interaction", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<InteractionComponent>())
                     {
                         auto* ic = w.GetComponent<InteractionComponent>(entity);
                         if (ic && ic->state == InteractionComponent::State::Destroyed && ic->usesRemaining > 0)
                         {
                             out.push_back(
                                 {"Interaction.DestroyedWithUses", static_cast<uint32_t>(entity),
                                  "Destroyed interaction has usesRemaining=" + std::to_string(ic->usesRemaining),
                                  StateViolationSeverity::Error});
                         }
                     }
                 }});

        AddRule({"Interaction.CooldownExpired", "Interaction", StateViolationSeverity::Warning, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<InteractionComponent>())
                     {
                         auto* ic = w.GetComponent<InteractionComponent>(entity);
                         if (ic && ic->state == InteractionComponent::State::Cooldown && ic->cooldownRemaining <= 0.0f)
                         {
                             out.push_back({"Interaction.CooldownExpired", static_cast<uint32_t>(entity),
                                            "In Cooldown state but cooldownRemaining<=0",
                                            StateViolationSeverity::Warning});
                         }
                     }
                 }});

        AddRule({"Interaction.HoldProgressOnNonHold", "Interaction", StateViolationSeverity::Warning, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<InteractionComponent>())
                     {
                         auto* ic = w.GetComponent<InteractionComponent>(entity);
                         if (ic && ic->type != InteractionComponent::InteractionType::Hold && ic->holdProgress > 0.0f)
                         {
                             out.push_back({"Interaction.HoldProgressOnNonHold", static_cast<uint32_t>(entity),
                                            "holdProgress>0 on non-Hold interaction type",
                                            StateViolationSeverity::Warning});
                         }
                     }
                 }});

        // -- Network rules --

        AddRule({"Network.DuplicateIDs", "Network", StateViolationSeverity::Critical, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     std::unordered_set<uint32_t> seen;
                     for (auto entity : w.GetEntitiesWith<NetworkIdentity>())
                     {
                         auto* ni = w.GetComponent<NetworkIdentity>(entity);
                         if (!ni || ni->networkID == 0)
                             continue;
                         if (!seen.insert(ni->networkID).second)
                         {
                             out.push_back({"Network.DuplicateIDs", static_cast<uint32_t>(entity),
                                            "Duplicate networkID=" + std::to_string(ni->networkID),
                                            StateViolationSeverity::Critical});
                         }
                     }
                 }});

        // -- Ragdoll rules --

        AddRule({"Ragdoll.AnimatedWithBlend", "Ragdoll", StateViolationSeverity::Warning, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<RagdollComponent>())
                     {
                         auto* rc = w.GetComponent<RagdollComponent>(entity);
                         if (rc && rc->mode == RagdollComponent::Mode::Animated && rc->blendWeight > 0.01f)
                         {
                             out.push_back({"Ragdoll.AnimatedWithBlend", static_cast<uint32_t>(entity),
                                            "Animated ragdoll has blendWeight=" + std::to_string(rc->blendWeight),
                                            StateViolationSeverity::Warning});
                         }
                     }
                 }});

        // -- Destructible rules --

        AddRule({"Destructible.HealthStageDesync", "Destructible", StateViolationSeverity::Error, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<DestructibleComponent>())
                     {
                         auto* dc = w.GetComponent<DestructibleComponent>(entity);
                         if (dc && dc->health > 0.0f && dc->currentStage >= dc->damageStages)
                         {
                             out.push_back({"Destructible.HealthStageDesync", static_cast<uint32_t>(entity),
                                            "health>0 but currentStage=" + std::to_string(dc->currentStage) +
                                                " >= damageStages=" + std::to_string(dc->damageStages),
                                            StateViolationSeverity::Error});
                         }
                     }
                 }});

        AddRule({"CoverPoint.OverOccupied", "Destructible", StateViolationSeverity::Warning, true,
                 [](World& w, std::vector<StateViolation>& out)
                 {
                     for (auto entity : w.GetEntitiesWith<CoverPointComponent>())
                     {
                         auto* cp = w.GetComponent<CoverPointComponent>(entity);
                         if (cp && cp->currentOccupants > cp->maxOccupants)
                         {
                             out.push_back({"CoverPoint.OverOccupied", static_cast<uint32_t>(entity),
                                            "currentOccupants=" + std::to_string(cp->currentOccupants) +
                                                " > maxOccupants=" + std::to_string(cp->maxOccupants),
                                            StateViolationSeverity::Warning});
                         }
                     }
                 }});
    }

} // namespace Spark
