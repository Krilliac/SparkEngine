/**
 * @file EventResponseSystem.cpp
 * @brief Implementation of the data-driven event response rule engine
 */

#include "EventResponseSystem.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Spark::Gameplay
{

    EventResponseSystem& EventResponseSystem::GetInstance()
    {
        static EventResponseSystem instance;
        return instance;
    }

    void EventResponseSystem::Initialize()
    {
        if (m_initialized)
        {
            return;
        }

        SubscribeToEvents();
        m_initialized = true;

        auto& console = SimpleConsole::GetInstance();
        console.LogInfo("[EventResponseSystem] Initialized");
    }

    void EventResponseSystem::Shutdown()
    {
        UnsubscribeFromEvents();
        m_rules.clear();
        m_delayedActions.clear();
        m_timers.clear();
        m_totalFired = 0;
        m_initialized = false;
    }

    void EventResponseSystem::Update(float deltaTime)
    {
        // Process timer-based rules
        for (auto& [ruleName, timer] : m_timers)
        {
            timer.elapsed += deltaTime;
            if (timer.elapsed >= timer.interval)
            {
                timer.elapsed -= timer.interval;
                EvaluateRules(EventTriggerType::OnTimer, ruleName, 0);
            }
        }

        // Process delayed actions
        for (auto it = m_delayedActions.begin(); it != m_delayedActions.end();)
        {
            it->remainingSeconds -= deltaTime;
            if (it->remainingSeconds <= 0.0f)
            {
                ExecuteActions(it->actions, it->contextEntity);
                it = m_delayedActions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // ========================================================================
    // Rule Management
    // ========================================================================

    void EventResponseSystem::AddRule(EventResponseRule rule)
    {
        // Set up timer state for OnTimer rules
        if (rule.trigger == EventTriggerType::OnTimer)
        {
            float interval = 1.0f;
            try
            {
                interval = std::stof(rule.triggerParam);
            }
            catch (...)
            {
                interval = 1.0f;
            }
            m_timers[rule.name] = TimerState{interval, 0.0f};
        }

        // Fire OnStart rules immediately
        if (rule.trigger == EventTriggerType::OnStart && rule.enabled)
        {
            ExecuteActions(rule.actions, rule.sourceEntityId);
            ++m_totalFired;
            if (rule.oneShot)
            {
                return; // Don't store one-shot OnStart rules after firing
            }
        }

        m_rules.push_back(std::move(rule));
    }

    void EventResponseSystem::RemoveRule(const std::string& name)
    {
        std::erase_if(m_rules, [&name](const EventResponseRule& r) { return r.name == name; });
        m_timers.erase(name);
    }

    void EventResponseSystem::SetRuleEnabled(const std::string& name, bool enabled)
    {
        for (auto& rule : m_rules)
        {
            if (rule.name == name)
            {
                rule.enabled = enabled;
                break;
            }
        }
    }

    void EventResponseSystem::ClearRules()
    {
        m_rules.clear();
        m_timers.clear();
        m_delayedActions.clear();
    }

    // ========================================================================
    // Event Subscription
    // ========================================================================

    void EventResponseSystem::SubscribeToEvents()
    {
        auto& bus = EventBus::Global();

        m_subscriptions.push_back(bus.Subscribe<TriggerEnterEvent>(
            [this](const TriggerEnterEvent& e)
            { EvaluateRules(EventTriggerType::OnTriggerEnter, std::to_string(e.triggerId), e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<TriggerExitEvent>(
            [this](const TriggerExitEvent& e)
            { EvaluateRules(EventTriggerType::OnTriggerExit, std::to_string(e.triggerId), e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<EntityDamagedEvent>(
            [this](const EntityDamagedEvent& e)
            { EvaluateRules(EventTriggerType::OnDamaged, e.damageSource, e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<EntityKilledEvent>(
            [this](const EntityKilledEvent& e) { EvaluateRules(EventTriggerType::OnKilled, e.cause, e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<ItemPickedUpEvent>(
            [this](const ItemPickedUpEvent& e)
            { EvaluateRules(EventTriggerType::OnItemPickup, std::to_string(e.itemDefId), e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<InputActionEvent>(
            [this](const InputActionEvent& e)
            {
                auto triggerType = e.pressed ? EventTriggerType::OnKeyPress : EventTriggerType::OnKeyRelease;
                EvaluateRules(triggerType, e.actionName, 0);
            }));

        m_subscriptions.push_back(bus.Subscribe<CollisionEvent>(
            [this](const CollisionEvent& e)
            { EvaluateRules(EventTriggerType::OnCollision, std::to_string(e.entityB), e.entityA); }));

        m_subscriptions.push_back(bus.Subscribe<QuestCompletedEvent>(
            [this](const QuestCompletedEvent& e)
            { EvaluateRules(EventTriggerType::OnQuestComplete, e.questName, e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<WeatherChangedEvent>(
            [this](const WeatherChangedEvent& e)
            { EvaluateRules(EventTriggerType::OnWeatherChange, std::to_string(e.newType), 0); }));

        m_subscriptions.push_back(bus.Subscribe<TimeOfDayChangedEvent>(
            [this](const TimeOfDayChangedEvent& e)
            { EvaluateRules(EventTriggerType::OnTimeOfDay, std::to_string(static_cast<int>(e.currentHour)), 0); }));

        m_subscriptions.push_back(bus.Subscribe<EntityCreatedEvent>(
            [this](const EntityCreatedEvent& e) { EvaluateRules(EventTriggerType::OnEntityCreated, "", e.entityId); }));

        m_subscriptions.push_back(bus.Subscribe<EntityDestroyedEvent>(
            [this](const EntityDestroyedEvent& e)
            { EvaluateRules(EventTriggerType::OnEntityDestroyed, "", e.entityId); }));
    }

    void EventResponseSystem::UnsubscribeFromEvents()
    {
        m_subscriptions.clear(); // RAII handles release via SubscriptionHandle
    }

    // ========================================================================
    // Rule Evaluation
    // ========================================================================

    void EventResponseSystem::EvaluateRules(EventTriggerType trigger, const std::string& triggerParam,
                                            uint32_t contextEntity)
    {
        // Collect rules to disable after firing (one-shot) to avoid mutation during iteration
        std::vector<std::string> toDisable;

        for (auto& rule : m_rules)
        {
            if (!rule.enabled || rule.trigger != trigger)
            {
                continue;
            }

            // For entity-specific rules, check the source entity matches
            if (rule.sourceEntityId != 0 && rule.sourceEntityId != contextEntity)
            {
                continue;
            }

            // For OnTimer, match by rule name (triggerParam == rule name from timer map)
            if (trigger == EventTriggerType::OnTimer && rule.name != triggerParam)
            {
                continue;
            }

            // For keyed triggers, match the trigger parameter
            if (trigger == EventTriggerType::OnKeyPress || trigger == EventTriggerType::OnKeyRelease ||
                trigger == EventTriggerType::OnCustom)
            {
                if (!rule.triggerParam.empty() && rule.triggerParam != triggerParam)
                {
                    continue;
                }
            }

            // Evaluate conditions (skip if no conditions defined)
            // Note: Full condition evaluation requires an ECS World reference.
            // When conditions are present but no World is available, the rule
            // fires unconditionally. In practice, the AngelScriptEngine binds
            // the active World; a future enhancement can pass it through here.
            if (!rule.conditions.IsEmpty())
            {
                // Condition evaluation requires a World& — not available here
                // without coupling to the ECS. For now, non-empty conditions
                // are evaluated by the ConditionSystem only when a World
                // pointer is provided via a future SetWorld() method.
                // TODO: Wire ConditionSystem evaluation when World is available.
            }

            // All checks passed — fire the rule
            ExecuteActions(rule.actions, contextEntity);
            ++m_totalFired;

            if (rule.oneShot)
            {
                toDisable.push_back(rule.name);
            }
        }

        for (const auto& name : toDisable)
        {
            SetRuleEnabled(name, false);
        }
    }

    // ========================================================================
    // Action Execution
    // ========================================================================

    void EventResponseSystem::ExecuteActions(const std::vector<GameplayAction>& actions, uint32_t contextEntity)
    {
        for (size_t i = 0; i < actions.size(); ++i)
        {
            if (!ExecuteSingleAction(actions[i], contextEntity, actions, i))
            {
                break; // Delay action defers remaining actions
            }
        }
    }

    bool EventResponseSystem::ExecuteSingleAction(const GameplayAction& action, uint32_t contextEntity,
                                                  const std::vector<GameplayAction>& allActions, size_t actionIndex)
    {
        auto& console = SimpleConsole::GetInstance();
        auto getStr = [](const ActionParam& p) -> std::string
        {
            if (auto* s = std::get_if<std::string>(&p))
                return *s;
            return "";
        };
        auto getDbl = [](const ActionParam& p) -> double
        {
            if (auto* d = std::get_if<double>(&p))
                return *d;
            if (auto* i = std::get_if<int64_t>(&p))
                return static_cast<double>(*i);
            return 0.0;
        };
        auto getInt = [](const ActionParam& p) -> int64_t
        {
            if (auto* i = std::get_if<int64_t>(&p))
                return *i;
            if (auto* d = std::get_if<double>(&p))
                return static_cast<int64_t>(*d);
            return 0;
        };

        switch (action.type)
        {
        case ActionType::ShowMessage:
        {
            std::string msg = action.params.empty() ? "(empty)" : getStr(action.params[0]);
            console.LogInfo("[EventResponse] Message: " + msg);
            break;
        }
        case ActionType::PlaySound:
        {
            std::string sound = action.params.empty() ? "" : getStr(action.params[0]);
            console.LogInfo("[EventResponse] PlaySound: " + sound);
            break;
        }
        case ActionType::StopSound:
        {
            std::string sound = action.params.empty() ? "" : getStr(action.params[0]);
            console.LogInfo("[EventResponse] StopSound: " + sound);
            break;
        }
        case ActionType::PlayAnimation:
        {
            std::string anim = action.params.empty() ? "" : getStr(action.params[0]);
            console.LogInfo("[EventResponse] PlayAnimation: " + anim);
            break;
        }
        case ActionType::SpawnEntity:
        {
            std::string name = action.params.empty() ? "Entity" : getStr(action.params[0]);
            console.LogInfo("[EventResponse] SpawnEntity: " + name);
            break;
        }
        case ActionType::DestroyEntity:
        {
            uint32_t eid = action.params.empty() ? contextEntity : static_cast<uint32_t>(getInt(action.params[0]));
            if (eid == 0)
                eid = contextEntity;
            console.LogInfo("[EventResponse] DestroyEntity: " + std::to_string(eid));
            break;
        }
        case ActionType::EnableEntity:
        case ActionType::DisableEntity:
        {
            uint32_t eid = action.params.empty() ? contextEntity : static_cast<uint32_t>(getInt(action.params[0]));
            std::string op = (action.type == ActionType::EnableEntity) ? "Enable" : "Disable";
            console.LogInfo("[EventResponse] " + op + "Entity: " + std::to_string(eid));
            break;
        }
        case ActionType::SetPosition:
        {
            double x = action.params.size() > 0 ? getDbl(action.params[0]) : 0.0;
            double y = action.params.size() > 1 ? getDbl(action.params[1]) : 0.0;
            double z = action.params.size() > 2 ? getDbl(action.params[2]) : 0.0;
            console.LogInfo("[EventResponse] SetPosition: (" + std::to_string(x) + ", " + std::to_string(y) + ", " +
                            std::to_string(z) + ")");
            break;
        }
        case ActionType::MoveToward:
        case ActionType::TeleportEntity:
        case ActionType::RotateEntity:
        case ActionType::ApplyForce:
        case ActionType::ApplyImpulse:
        {
            console.LogInfo("[EventResponse] Transform/Physics action type " +
                            std::to_string(static_cast<int>(action.type)));
            break;
        }
        case ActionType::SetHealth:
        case ActionType::DealDamage:
        case ActionType::HealEntity:
        {
            double amount = action.params.empty() ? 0.0 : getDbl(action.params[0]);
            std::string op = (action.type == ActionType::SetHealth)    ? "SetHealth"
                             : (action.type == ActionType::DealDamage) ? "DealDamage"
                                                                       : "HealEntity";
            console.LogInfo("[EventResponse] " + op + ": " + std::to_string(amount));
            break;
        }
        case ActionType::ShowDialogue:
        {
            std::string dlg = action.params.empty() ? "" : getStr(action.params[0]);
            console.LogInfo("[EventResponse] ShowDialogue: " + dlg);
            break;
        }
        case ActionType::SetWeather:
        {
            int64_t weatherType = action.params.empty() ? 0 : getInt(action.params[0]);
            console.LogInfo("[EventResponse] SetWeather: " + std::to_string(weatherType));
            break;
        }
        case ActionType::SetTimeOfDay:
        {
            double hour = action.params.empty() ? 12.0 : getDbl(action.params[0]);
            console.LogInfo("[EventResponse] SetTimeOfDay: " + std::to_string(hour));
            break;
        }
        case ActionType::SetWorldVariable:
        {
            std::string varName = action.params.size() > 0 ? getStr(action.params[0]) : "";
            int64_t value = action.params.size() > 1 ? getInt(action.params[1]) : 0;
            ConditionSystem::GetInstance().SetVariable(varName, value);
            console.LogInfo("[EventResponse] SetWorldVariable: " + varName + " = " + std::to_string(value));
            break;
        }
        case ActionType::SetWorldFlag:
        {
            std::string flagName = action.params.size() > 0 ? getStr(action.params[0]) : "";
            bool value = action.params.size() > 1 ? (getInt(action.params[1]) != 0) : true;
            ConditionSystem::GetInstance().SetFlag(flagName, value);
            console.LogInfo("[EventResponse] SetWorldFlag: " + flagName + " = " + (value ? "true" : "false"));
            break;
        }
        case ActionType::Delay:
        {
            double seconds = action.params.empty() ? 1.0 : getDbl(action.params[0]);
            // Defer remaining actions
            if (actionIndex + 1 < allActions.size())
            {
                DelayedActionBatch batch;
                batch.remainingSeconds = static_cast<float>(seconds);
                batch.contextEntity = contextEntity;
                batch.actions.assign(allActions.begin() + static_cast<ptrdiff_t>(actionIndex) + 1, allActions.end());
                m_delayedActions.push_back(std::move(batch));
            }
            return false; // Stop processing further actions in this batch
        }
        case ActionType::FireCustomEvent:
        {
            std::string evtName = action.params.empty() ? "" : getStr(action.params[0]);
            FireCustomEvent(evtName, contextEntity);
            break;
        }
        default:
            break;
        }

        return true; // Continue to next action
    }

    void EventResponseSystem::FireCustomEvent(const std::string& eventName, uint32_t sourceEntity)
    {
        EvaluateRules(EventTriggerType::OnCustom, eventName, sourceEntity);
    }

    // ========================================================================
    // JSON Serialization
    // ========================================================================

    static std::string TriggerTypeToString(EventTriggerType t)
    {
        switch (t)
        {
        case EventTriggerType::OnTriggerEnter:
            return "OnTriggerEnter";
        case EventTriggerType::OnTriggerExit:
            return "OnTriggerExit";
        case EventTriggerType::OnDamaged:
            return "OnDamaged";
        case EventTriggerType::OnKilled:
            return "OnKilled";
        case EventTriggerType::OnItemPickup:
            return "OnItemPickup";
        case EventTriggerType::OnKeyPress:
            return "OnKeyPress";
        case EventTriggerType::OnKeyRelease:
            return "OnKeyRelease";
        case EventTriggerType::OnCollision:
            return "OnCollision";
        case EventTriggerType::OnQuestComplete:
            return "OnQuestComplete";
        case EventTriggerType::OnTimer:
            return "OnTimer";
        case EventTriggerType::OnStart:
            return "OnStart";
        case EventTriggerType::OnWeatherChange:
            return "OnWeatherChange";
        case EventTriggerType::OnTimeOfDay:
            return "OnTimeOfDay";
        case EventTriggerType::OnEntityCreated:
            return "OnEntityCreated";
        case EventTriggerType::OnEntityDestroyed:
            return "OnEntityDestroyed";
        case EventTriggerType::OnCustom:
            return "OnCustom";
        default:
            return "Unknown";
        }
    }

    static EventTriggerType StringToTriggerType(const std::string& s)
    {
        if (s == "OnTriggerEnter")
            return EventTriggerType::OnTriggerEnter;
        if (s == "OnTriggerExit")
            return EventTriggerType::OnTriggerExit;
        if (s == "OnDamaged")
            return EventTriggerType::OnDamaged;
        if (s == "OnKilled")
            return EventTriggerType::OnKilled;
        if (s == "OnItemPickup")
            return EventTriggerType::OnItemPickup;
        if (s == "OnKeyPress")
            return EventTriggerType::OnKeyPress;
        if (s == "OnKeyRelease")
            return EventTriggerType::OnKeyRelease;
        if (s == "OnCollision")
            return EventTriggerType::OnCollision;
        if (s == "OnQuestComplete")
            return EventTriggerType::OnQuestComplete;
        if (s == "OnTimer")
            return EventTriggerType::OnTimer;
        if (s == "OnStart")
            return EventTriggerType::OnStart;
        if (s == "OnWeatherChange")
            return EventTriggerType::OnWeatherChange;
        if (s == "OnTimeOfDay")
            return EventTriggerType::OnTimeOfDay;
        if (s == "OnEntityCreated")
            return EventTriggerType::OnEntityCreated;
        if (s == "OnEntityDestroyed")
            return EventTriggerType::OnEntityDestroyed;
        if (s == "OnCustom")
            return EventTriggerType::OnCustom;
        return EventTriggerType::OnStart;
    }

    static std::string ActionTypeToString(ActionType t)
    {
        switch (t)
        {
        case ActionType::SpawnEntity:
            return "SpawnEntity";
        case ActionType::DestroyEntity:
            return "DestroyEntity";
        case ActionType::EnableEntity:
            return "EnableEntity";
        case ActionType::DisableEntity:
            return "DisableEntity";
        case ActionType::SetPosition:
            return "SetPosition";
        case ActionType::MoveToward:
            return "MoveToward";
        case ActionType::TeleportEntity:
            return "TeleportEntity";
        case ActionType::RotateEntity:
            return "RotateEntity";
        case ActionType::SetHealth:
            return "SetHealth";
        case ActionType::DealDamage:
            return "DealDamage";
        case ActionType::HealEntity:
            return "HealEntity";
        case ActionType::PlaySound:
            return "PlaySound";
        case ActionType::StopSound:
            return "StopSound";
        case ActionType::PlayAnimation:
            return "PlayAnimation";
        case ActionType::ApplyForce:
            return "ApplyForce";
        case ActionType::ApplyImpulse:
            return "ApplyImpulse";
        case ActionType::ShowDialogue:
            return "ShowDialogue";
        case ActionType::ShowMessage:
            return "ShowMessage";
        case ActionType::SetWeather:
            return "SetWeather";
        case ActionType::SetTimeOfDay:
            return "SetTimeOfDay";
        case ActionType::SetWorldVariable:
            return "SetWorldVariable";
        case ActionType::SetWorldFlag:
            return "SetWorldFlag";
        case ActionType::Delay:
            return "Delay";
        case ActionType::FireCustomEvent:
            return "FireCustomEvent";
        default:
            return "Unknown";
        }
    }

    static ActionType StringToActionType(const std::string& s)
    {
        if (s == "SpawnEntity")
            return ActionType::SpawnEntity;
        if (s == "DestroyEntity")
            return ActionType::DestroyEntity;
        if (s == "EnableEntity")
            return ActionType::EnableEntity;
        if (s == "DisableEntity")
            return ActionType::DisableEntity;
        if (s == "SetPosition")
            return ActionType::SetPosition;
        if (s == "MoveToward")
            return ActionType::MoveToward;
        if (s == "TeleportEntity")
            return ActionType::TeleportEntity;
        if (s == "RotateEntity")
            return ActionType::RotateEntity;
        if (s == "SetHealth")
            return ActionType::SetHealth;
        if (s == "DealDamage")
            return ActionType::DealDamage;
        if (s == "HealEntity")
            return ActionType::HealEntity;
        if (s == "PlaySound")
            return ActionType::PlaySound;
        if (s == "StopSound")
            return ActionType::StopSound;
        if (s == "PlayAnimation")
            return ActionType::PlayAnimation;
        if (s == "ApplyForce")
            return ActionType::ApplyForce;
        if (s == "ApplyImpulse")
            return ActionType::ApplyImpulse;
        if (s == "ShowDialogue")
            return ActionType::ShowDialogue;
        if (s == "ShowMessage")
            return ActionType::ShowMessage;
        if (s == "SetWeather")
            return ActionType::SetWeather;
        if (s == "SetTimeOfDay")
            return ActionType::SetTimeOfDay;
        if (s == "SetWorldVariable")
            return ActionType::SetWorldVariable;
        if (s == "SetWorldFlag")
            return ActionType::SetWorldFlag;
        if (s == "Delay")
            return ActionType::Delay;
        if (s == "FireCustomEvent")
            return ActionType::FireCustomEvent;
        return ActionType::ShowMessage;
    }

    // Minimal JSON writer — avoids external dependency
    static void WriteJsonString(std::ostream& out, const std::string& s)
    {
        out << '"';
        for (char c : s)
        {
            if (c == '"')
                out << "\\\"";
            else if (c == '\\')
                out << "\\\\";
            else if (c == '\n')
                out << "\\n";
            else
                out << c;
        }
        out << '"';
    }

    bool EventResponseSystem::SaveToJson(const std::string& path) const
    {
        std::ofstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        file << "{\n  \"rules\": [\n";
        for (size_t r = 0; r < m_rules.size(); ++r)
        {
            const auto& rule = m_rules[r];
            file << "    {\n";
            file << "      \"name\": ";
            WriteJsonString(file, rule.name);
            file << ",\n";
            file << "      \"sourceEntityId\": " << rule.sourceEntityId << ",\n";
            file << "      \"trigger\": ";
            WriteJsonString(file, TriggerTypeToString(rule.trigger));
            file << ",\n";
            file << "      \"triggerParam\": ";
            WriteJsonString(file, rule.triggerParam);
            file << ",\n";
            file << "      \"enabled\": " << (rule.enabled ? "true" : "false") << ",\n";
            file << "      \"oneShot\": " << (rule.oneShot ? "true" : "false") << ",\n";

            // Actions
            file << "      \"actions\": [\n";
            for (size_t a = 0; a < rule.actions.size(); ++a)
            {
                const auto& act = rule.actions[a];
                file << "        { \"type\": ";
                WriteJsonString(file, ActionTypeToString(act.type));
                file << ", \"params\": [";
                for (size_t p = 0; p < act.params.size(); ++p)
                {
                    if (p > 0)
                        file << ", ";
                    std::visit(
                        [&file](auto&& val)
                        {
                            using T = std::decay_t<decltype(val)>;
                            if constexpr (std::is_same_v<T, std::monostate>)
                                file << "null";
                            else if constexpr (std::is_same_v<T, std::string>)
                                WriteJsonString(file, val);
                            else if constexpr (std::is_same_v<T, int64_t>)
                                file << val;
                            else if constexpr (std::is_same_v<T, double>)
                                file << val;
                            else if constexpr (std::is_same_v<T, uint32_t>)
                                file << val;
                        },
                        act.params[p]);
                }
                file << "] }";
                if (a + 1 < rule.actions.size())
                    file << ",";
                file << "\n";
            }
            file << "      ]\n";

            file << "    }";
            if (r + 1 < m_rules.size())
                file << ",";
            file << "\n";
        }
        file << "  ]\n}\n";
        return true;
    }

    bool EventResponseSystem::LoadFromJson(const std::string& /*path*/)
    {
        // JSON parsing would require a JSON library or manual parser.
        // For now, rules are created programmatically or via the editor.
        // Full JSON loading can be added when a JSON library (nlohmann/json) is integrated.
        return false;
    }

} // namespace Spark::Gameplay
