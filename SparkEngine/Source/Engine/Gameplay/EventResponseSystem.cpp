/**
 * @file EventResponseSystem.cpp
 * @brief Implementation of the data-driven event response rule engine
 */

#include "EventResponseSystem.h"
#include "Core/EngineContext.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"
#include "Engine/Modding/HeldHandles.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spark::Gameplay
{
    namespace
    {
        std::optional<std::string> ReadRulesFile(const std::string& path, size_t maxBytes)
        {
#ifdef _WIN32
            Spark::HeldHandles::ScopedHandle handle(
                ::CreateFileW(std::filesystem::path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!handle.IsValid())
            {
                return std::nullopt;
            }

            BY_HANDLE_FILE_INFORMATION info{};
            LARGE_INTEGER size{};
            if (::GetFileType(handle.Get()) != FILE_TYPE_DISK || !::GetFileInformationByHandle(handle.Get(), &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
                !::GetFileSizeEx(handle.Get(), &size) || size.QuadPart < 0 ||
                static_cast<unsigned long long>(size.QuadPart) > maxBytes)
            {
                return std::nullopt;
            }

            std::string text(static_cast<size_t>(size.QuadPart), '\0');
            size_t offset = 0;
            while (offset < text.size())
            {
                const DWORD request = static_cast<DWORD>(std::min<size_t>(text.size() - offset, 64 * 1024));
                DWORD read = 0;
                if (!::ReadFile(handle.Get(), text.data() + offset, request, &read, nullptr) || read == 0)
                {
                    return std::nullopt;
                }
                offset += read;
            }

            char extra = 0;
            DWORD extraRead = 0;
            if (!::ReadFile(handle.Get(), &extra, 1, &extraRead, nullptr))
            {
                if (::GetLastError() != ERROR_HANDLE_EOF)
                {
                    return std::nullopt;
                }
            }
            else if (extraRead != 0)
            {
                return std::nullopt;
            }

            BY_HANDLE_FILE_INFORMATION after{};
            if (!::GetFileInformationByHandle(handle.Get(), &after) ||
                after.dwVolumeSerialNumber != info.dwVolumeSerialNumber ||
                after.nFileIndexHigh != info.nFileIndexHigh || after.nFileIndexLow != info.nFileIndexLow ||
                after.nFileSizeHigh != info.nFileSizeHigh || after.nFileSizeLow != info.nFileSizeLow ||
                ::CompareFileTime(&after.ftLastWriteTime, &info.ftLastWriteTime) != 0)
            {
                return std::nullopt;
            }
            return text;
#else
            Spark::HeldHandles::ScopedFd handle(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
            if (handle.Get() < 0)
            {
                return std::nullopt;
            }

            struct stat info
            {
            };
            if (::fstat(handle.Get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
                static_cast<unsigned long long>(info.st_size) > maxBytes)
            {
                return std::nullopt;
            }

            std::string text(static_cast<size_t>(info.st_size), '\0');
            size_t offset = 0;
            while (offset < text.size())
            {
                const ssize_t read = ::read(handle.Get(), text.data() + offset, text.size() - offset);
                if (read < 0 && errno == EINTR)
                {
                    continue;
                }
                if (read <= 0)
                {
                    return std::nullopt;
                }
                offset += static_cast<size_t>(read);
            }

            char extra = 0;
            for (;;)
            {
                const ssize_t read = ::read(handle.Get(), &extra, 1);
                if (read < 0 && errno == EINTR)
                {
                    continue;
                }
                if (read > 0)
                {
                    return std::nullopt;
                }
                if (read < 0)
                {
                    return std::nullopt;
                }
                break;
            }

            struct stat after
            {
            };
            if (::fstat(handle.Get(), &after) != 0 || after.st_dev != info.st_dev || after.st_ino != info.st_ino ||
                after.st_size != info.st_size ||
#if defined(__APPLE__)
                after.st_mtimespec.tv_sec != info.st_mtimespec.tv_sec ||
                after.st_mtimespec.tv_nsec != info.st_mtimespec.tv_nsec
#else
                after.st_mtim.tv_sec != info.st_mtim.tv_sec || after.st_mtim.tv_nsec != info.st_mtim.tv_nsec
#endif
            )
            {
                return std::nullopt;
            }
            return text;
#endif
        }
    } // namespace

    EventResponseSystem& EventResponseSystem::GetInstance()
    {
        static EventResponseSystem instance;
        return instance;
    }

    void EventResponseSystem::Initialize()
    {
        if (m_initialized)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "EventResponseSystem::Initialize called while already initialized");
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "EventResponseSystem::Initialize — subscribing to events");
        SubscribeToEvents();
        m_initialized = true;

        auto& console = SimpleConsole::GetInstance();
        console.LogInfo("[EventResponseSystem] Initialized");
    }

    void EventResponseSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "EventResponseSystem::Shutdown — %zu rules, %zu pending delayed actions", m_rules.size(),
                       m_delayedActions.size());
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
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "EventResponseSystem::AddRule '%s' trigger=%d", rule.name.c_str(),
                        static_cast<int>(rule.trigger));
        SPARK_WARN_IF(Spark::LogCategory::Game, rule.name.empty(), "Adding rule with empty name");

        // Set up timer state for OnTimer rules
        if (rule.trigger == EventTriggerType::OnTimer)
        {
            float interval = 1.0f;
            try
            {
                interval = std::stof(rule.triggerParam);
            }
            catch (const std::exception& e)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "EventResponseSystem: invalid timer interval '%s' for rule '%s': %s",
                               rule.triggerParam.c_str(), rule.name.c_str(), e.what());
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
            if (!rule.conditions.IsEmpty())
            {
                auto* ctx = EngineContext::Get();
                World* world = ctx ? ctx->GetWorld() : nullptr;
                if (world)
                {
                    bool conditionsPassed =
                        ConditionSystem::GetInstance().Evaluate(rule.conditions, contextEntity, *world);
                    if (!conditionsPassed)
                    {
                        continue;
                    }
                }
                // If no World available, skip condition check (fire unconditionally)
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
        // The parameter readers live with the rule file format (EventResponseRules.cpp). They
        // read uint32 parameters (the documented entity-id kind; the old lambdas returned 0
        // for them) and give 0 for a double outside int64 instead of an undefined conversion.
        const auto getStr = &ActionParamToString;
        const auto getDbl = &ActionParamToDouble;
        const auto getInt = &ActionParamToInt64;

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
        // A rule action can fire a custom event that fires the same rule again; a rules file
        // that closes that loop recursed until the stack overflowed.
        if (m_customEventDepth >= kMaxCustomEventDepth)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "EventResponseSystem: dropped custom event '%s' nested %u events deep", eventName.c_str(),
                           m_customEventDepth);
            return;
        }

        struct DepthScope
        {
            uint32_t& depth;
            explicit DepthScope(uint32_t& counter) : depth(counter) { ++depth; }
            ~DepthScope() { --depth; }
            DepthScope(const DepthScope&) = delete;
            DepthScope& operator=(const DepthScope&) = delete;
        };
        const DepthScope scope(m_customEventDepth);
        EvaluateRules(EventTriggerType::OnCustom, eventName, sourceEntity);
    }

    // ========================================================================
    // JSON Serialization (the format lives in EventResponseRules.cpp)
    // ========================================================================

    bool EventResponseSystem::SaveToJson(const std::string& path) const
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "EventResponseSystem::SaveToJson — saving %zu rules to '%s'",
                       m_rules.size(), path.c_str());
        std::ofstream file(path);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "SaveToJson: failed to open file '%s'", path.c_str());
            return false;
        }

        WriteEventResponseRules(file, m_rules);
        return true;
    }

    bool EventResponseSystem::LoadFromJson(const std::string& path)
    {
        const size_t maxBytes = Spark::Json::JsonLimits{}.maxBytes;
        const auto text = ReadRulesFile(path, maxBytes);
        if (!text)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "LoadFromJson: failed to open or read regular file '%s'",
                            path.c_str());
            return false;
        }

        std::vector<EventResponseRule> rules;
        if (!ParseEventResponseRules(*text, rules, path))
        {
            return false;
        }

        m_rules.clear();
        m_timers.clear();
        m_delayedActions.clear();
        for (auto& rule : rules)
        {
            AddRule(std::move(rule));
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "EventResponseSystem::LoadFromJson — loaded %zu rules from '%s'",
                       m_rules.size(), path.c_str());
        return true;
    }

} // namespace Spark::Gameplay
