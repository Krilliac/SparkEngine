/**
 * @file EventResponseRules.cpp
 * @brief Event-response rule files: the JSON reader and writer and the action-parameter readers
 *
 * EventResponseSystem::LoadFromJson reads a rules file and hands its text here; SaveToJson
 * writes through WriteEventResponseRules. Keeping the format in its own translation unit lets
 * the SEC-120 fuzz target (FuzzerTests/FuzzEventResponse.cpp) link the shipped reader without
 * the EventBus subscriptions, the console or the condition system the rule engine runs on.
 */

#include "EventResponseSystem.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <ostream>
#include <string_view>
#include <system_error>
#include <utility>

namespace Spark::Gameplay
{
    namespace
    {
        std::string TriggerTypeToString(EventTriggerType t)
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

        /// Inverse of TriggerTypeToString; an unrecognised name reads as OnStart.
        EventTriggerType StringToTriggerType(const std::string& s)
        {
            for (size_t i = 0; i < static_cast<size_t>(EventTriggerType::Count); ++i)
            {
                const auto type = static_cast<EventTriggerType>(i);
                if (TriggerTypeToString(type) == s)
                {
                    return type;
                }
            }
            return EventTriggerType::OnStart;
        }

        std::string ActionTypeToString(ActionType t)
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

        /// Inverse of ActionTypeToString; an unrecognised name reads as ShowMessage.
        ActionType StringToActionType(const std::string& s)
        {
            for (size_t i = 0; i < static_cast<size_t>(ActionType::Count); ++i)
            {
                const auto type = static_cast<ActionType>(i);
                if (ActionTypeToString(type) == s)
                {
                    return type;
                }
            }
            return ActionType::ShowMessage;
        }

        /// JSON string literal for @p s. Every control character is escaped: a raw one is not
        /// valid JSON, so a strict reader (the nlohmann backend Json::Parse uses in engine
        /// builds) refused the whole file SaveToJson had just written.
        void WriteJsonString(std::ostream& out, const std::string& s)
        {
            out << '"';
            for (const char c : s)
            {
                switch (c)
                {
                case '"':
                    out << "\\\"";
                    break;
                case '\\':
                    out << "\\\\";
                    break;
                case '\b':
                    out << "\\b";
                    break;
                case '\f':
                    out << "\\f";
                    break;
                case '\n':
                    out << "\\n";
                    break;
                case '\r':
                    out << "\\r";
                    break;
                case '\t':
                    out << "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        constexpr char kHex[] = "0123456789abcdef";
                        out << "\\u00" << kHex[static_cast<unsigned char>(c) >> 4] << kHex[c & 0x0f];
                    }
                    else
                    {
                        out << c;
                    }
                    break;
                }
            }
            out << '"';
        }

        /// Shortest text that reads back as exactly @p value. A stream's default six significant
        /// digits turned 1234567.0 into 1.23457e+06. NaN and infinity have no JSON spelling, so
        /// they are written as null (which reads back as an empty parameter). Zero is written
        /// as "0": a JSON reader takes "-0" for the integer zero, so its sign cannot survive.
        /// Any other integral-looking text gets ".0": Json::Parse reads a token without '.' or
        /// an exponent as a 64-bit integer, which cannot hold a double such as 2^64.
        void WriteJsonNumber(std::ostream& out, double value)
        {
            if (!std::isfinite(value))
            {
                out << "null";
                return;
            }
            if (value == 0.0)
            {
                out << '0';
                return;
            }
            char buffer[32];
            const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (error != std::errc{})
            {
                out << "null";
                return;
            }
            const std::string_view text(buffer, static_cast<size_t>(end - buffer));
            out << text;
            if (text.find_first_of(".e") == std::string_view::npos)
            {
                out << ".0";
            }
        }

        /// sourceEntityId as WriteEventResponseRules writes it: an exact integer that fits uint32.
        std::optional<uint32_t> ReadEntityId(const Spark::Json::Value& node)
        {
            if (!node.IsNumber())
            {
                return std::nullopt;
            }
            const double value = node.AsNumber();
            if (!std::isfinite(value) || value < 0.0 || value > 4294967295.0 || value != std::trunc(value))
            {
                return std::nullopt;
            }
            return static_cast<uint32_t>(value);
        }
    } // namespace

    bool ParseEventResponseRules(std::string_view json, std::vector<EventResponseRule>& rules,
                                 std::string_view sourceName)
    {
        const std::string source(sourceName);
        const auto root = Spark::Json::Parse(json);
        if (!root.IsObject() || !root.HasKey("rules"))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "LoadFromJson: invalid root object in '%s'", source.c_str());
            return false;
        }

        const auto& rulesNode = root["rules"];
        if (!rulesNode.IsArray())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "LoadFromJson: 'rules' is not an array in '%s'", source.c_str());
            return false;
        }

        std::vector<EventResponseRule> parsed;
        for (size_t r = 0; r < rulesNode.Size(); ++r)
        {
            const auto& ruleNode = rulesNode[r];
            if (!ruleNode.IsObject())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "LoadFromJson: skipping non-object rule at index %zu", r);
                continue;
            }

            EventResponseRule rule;
            rule.name = ruleNode.HasKey("name") ? ruleNode["name"].AsString() : "";
            // The entity scope must be read exactly: AsInt() turned an id above INT_MAX into 0,
            // which made an entity-scoped rule global, and a negative value wrapped around.
            if (ruleNode.HasKey("sourceEntityId"))
            {
                const auto entityId = ReadEntityId(ruleNode["sourceEntityId"]);
                if (!entityId)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "LoadFromJson: skipping rule at index %zu: sourceEntityId is not a uint32", r);
                    continue;
                }
                rule.sourceEntityId = *entityId;
            }
            rule.trigger = ruleNode.HasKey("trigger") ? StringToTriggerType(ruleNode["trigger"].AsString())
                                                      : EventTriggerType::OnStart;
            rule.triggerParam = ruleNode.HasKey("triggerParam") ? ruleNode["triggerParam"].AsString() : "";
            rule.enabled = !ruleNode.HasKey("enabled") || ruleNode["enabled"].AsBool();
            rule.oneShot = ruleNode.HasKey("oneShot") && ruleNode["oneShot"].AsBool();

            if (ruleNode.HasKey("actions") && ruleNode["actions"].IsArray())
            {
                const auto& actions = ruleNode["actions"];
                for (size_t a = 0; a < actions.Size(); ++a)
                {
                    const auto& actionNode = actions[a];
                    if (!actionNode.IsObject())
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Game,
                                       "LoadFromJson: skipping non-object action at rule %zu action %zu", r, a);
                        continue;
                    }

                    GameplayAction action;
                    action.type = actionNode.HasKey("type") ? StringToActionType(actionNode["type"].AsString())
                                                            : ActionType::ShowMessage;

                    if (actionNode.HasKey("params") && actionNode["params"].IsArray())
                    {
                        const auto& params = actionNode["params"];
                        action.params.reserve(params.Size());
                        for (size_t p = 0; p < params.Size(); ++p)
                        {
                            const auto& paramNode = params[p];
                            if (paramNode.IsNull())
                            {
                                action.params.emplace_back(std::monostate{});
                            }
                            else if (paramNode.IsString())
                            {
                                action.params.emplace_back(paramNode.AsString());
                            }
                            else if (paramNode.IsBool())
                            {
                                action.params.emplace_back(static_cast<int64_t>(paramNode.AsBool() ? 1 : 0));
                            }
                            else if (paramNode.IsNumber() && std::isfinite(paramNode.AsNumber()))
                            {
                                action.params.emplace_back(paramNode.AsNumber());
                            }
                            else
                            {
                                SPARK_LOG_WARN(Spark::LogCategory::Game,
                                               "LoadFromJson: unsupported param type at rule %zu action %zu param %zu; "
                                               "storing null",
                                               r, a, p);
                                action.params.emplace_back(std::monostate{});
                            }
                        }
                    }

                    rule.actions.push_back(std::move(action));
                }
            }

            parsed.push_back(std::move(rule));
        }

        rules = std::move(parsed);
        return true;
    }

    void WriteEventResponseRules(std::ostream& out, const std::vector<EventResponseRule>& rules)
    {
        out << "{\n  \"rules\": [\n";
        for (size_t r = 0; r < rules.size(); ++r)
        {
            const auto& rule = rules[r];
            out << "    {\n";
            out << "      \"name\": ";
            WriteJsonString(out, rule.name);
            out << ",\n";
            out << "      \"sourceEntityId\": " << rule.sourceEntityId << ",\n";
            out << "      \"trigger\": ";
            WriteJsonString(out, TriggerTypeToString(rule.trigger));
            out << ",\n";
            out << "      \"triggerParam\": ";
            WriteJsonString(out, rule.triggerParam);
            out << ",\n";
            out << "      \"enabled\": " << (rule.enabled ? "true" : "false") << ",\n";
            out << "      \"oneShot\": " << (rule.oneShot ? "true" : "false") << ",\n";

            // Actions
            out << "      \"actions\": [\n";
            for (size_t a = 0; a < rule.actions.size(); ++a)
            {
                const auto& act = rule.actions[a];
                out << "        { \"type\": ";
                WriteJsonString(out, ActionTypeToString(act.type));
                out << ", \"params\": [";
                for (size_t p = 0; p < act.params.size(); ++p)
                {
                    if (p > 0)
                    {
                        out << ", ";
                    }
                    std::visit(
                        [&out](auto&& val)
                        {
                            using T = std::decay_t<decltype(val)>;
                            if constexpr (std::is_same_v<T, std::monostate>)
                            {
                                out << "null";
                            }
                            else if constexpr (std::is_same_v<T, std::string>)
                            {
                                WriteJsonString(out, val);
                            }
                            else if constexpr (std::is_same_v<T, double>)
                            {
                                WriteJsonNumber(out, val);
                            }
                            else
                            {
                                out << val; // int64_t and uint32_t
                            }
                        },
                        act.params[p]);
                }
                out << "] }";
                if (a + 1 < rule.actions.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "      ]\n";

            out << "    }";
            if (r + 1 < rules.size())
            {
                out << ",";
            }
            out << "\n";
        }
        out << "  ]\n}\n";
    }

    int64_t ActionParamToInt64(const ActionParam& param)
    {
        if (const auto* i = std::get_if<int64_t>(&param))
        {
            return *i;
        }
        if (const auto* u = std::get_if<uint32_t>(&param))
        {
            return static_cast<int64_t>(*u);
        }
        if (const auto* d = std::get_if<double>(&param))
        {
            // Converting a double outside int64 (or NaN) is undefined behaviour, and the rule
            // file chooses the number, so one that does not fit reads as 0 like a missing one.
            if (std::isfinite(*d) && *d >= -9223372036854775808.0 && *d < 9223372036854775808.0)
            {
                return static_cast<int64_t>(*d);
            }
        }
        return 0;
    }

    double ActionParamToDouble(const ActionParam& param)
    {
        if (const auto* d = std::get_if<double>(&param))
        {
            return *d;
        }
        if (const auto* i = std::get_if<int64_t>(&param))
        {
            return static_cast<double>(*i);
        }
        if (const auto* u = std::get_if<uint32_t>(&param))
        {
            return static_cast<double>(*u);
        }
        return 0.0;
    }

    std::string ActionParamToString(const ActionParam& param)
    {
        if (const auto* s = std::get_if<std::string>(&param))
        {
            return *s;
        }
        return "";
    }

} // namespace Spark::Gameplay
