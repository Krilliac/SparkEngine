/**
 * @file FuzzEventResponseProduction.cpp
 * @brief libc++-compiled production adapter for the event-response rule file libFuzzer harness.
 *
 * EventResponseSystem::LoadFromJson reads a rules file and hands its text to
 * Spark::Gameplay::ParseEventResponseRules (Engine/Gameplay/EventResponseRules.cpp); the
 * engine then runs the rules' actions, which read their parameters through
 * ActionParamToInt64/ToDouble/ToString. Engine builds parse through the vendored nlohmann
 * backend of Json::Parse, so this target defines SPARK_HAS_NLOHMANN_JSON as they do. A
 * violated contract aborts so libFuzzer records a crash:
 *  - a rejected file leaves the caller's rules untouched,
 *  - an accepted file yields no more rules than the text has objects, known trigger and
 *    action kinds, no conditions, parameters that are null, strings, booleans read as int64,
 *    or finite doubles, and parameter readers that are defined for every value (UBSan sees
 *    every conversion) and agree with each other,
 *  - WriteEventResponseRules output reads back as the same rules (a boolean parameter reads
 *    back as the equal double), and writing those again gives the same bytes,
 *  - parsing the same text twice gives the same rules.
 */

#include "FuzzEventResponseProduction.h"

#include "Engine/Gameplay/EventResponseSystem.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEventResponse: ParseEventResponseRules violated: %s\n", what);
        std::abort();
    }

    using Spark::Gameplay::ActionParam;
    using Spark::Gameplay::ActionType;
    using Spark::Gameplay::EventResponseRule;
    using Spark::Gameplay::EventTriggerType;

    /// Same parameter, allowing the int64 a boolean reads as to come back as the equal double.
    bool SameParam(const ActionParam& a, const ActionParam& b)
    {
        if (a.index() == b.index())
        {
            if (const auto* da = std::get_if<double>(&a))
            {
                return *da == std::get<double>(b); // == treats -0.0 and 0.0 alike
            }
            return a == b;
        }
        const auto* ia = std::get_if<std::int64_t>(&a);
        const auto* db = std::get_if<double>(&b);
        return ia != nullptr && db != nullptr && static_cast<double>(*ia) == *db;
    }

    bool SameRules(const std::vector<EventResponseRule>& a, const std::vector<EventResponseRule>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t r = 0; r < a.size(); ++r)
        {
            const EventResponseRule& x = a[r];
            const EventResponseRule& y = b[r];
            if (x.name != y.name || x.sourceEntityId != y.sourceEntityId || x.trigger != y.trigger ||
                x.triggerParam != y.triggerParam || x.enabled != y.enabled || x.oneShot != y.oneShot ||
                x.actions.size() != y.actions.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < x.actions.size(); ++i)
            {
                const auto& px = x.actions[i].params;
                const auto& py = y.actions[i].params;
                if (x.actions[i].type != y.actions[i].type || px.size() != py.size())
                {
                    return false;
                }
                for (std::size_t p = 0; p < px.size(); ++p)
                {
                    if (!SameParam(px[p], py[p]))
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    void CheckParam(const ActionParam& param)
    {
        if (std::holds_alternative<std::uint32_t>(param))
        {
            InvariantFailure("the reader produced a uint32 parameter");
        }
        const std::int64_t asInt = Spark::Gameplay::ActionParamToInt64(param);
        const double asDouble = Spark::Gameplay::ActionParamToDouble(param);
        const std::string asString = Spark::Gameplay::ActionParamToString(param);
        if (const auto* d = std::get_if<double>(&param))
        {
            if (!std::isfinite(*d))
            {
                InvariantFailure("an accepted parameter is not finite");
            }
            if (asDouble != *d)
            {
                InvariantFailure("ActionParamToDouble changed a double");
            }
            const bool fits = *d >= -9223372036854775808.0 && *d < 9223372036854775808.0;
            if (fits ? asInt != static_cast<std::int64_t>(*d) : asInt != 0)
            {
                InvariantFailure("ActionParamToInt64 disagrees with truncation");
            }
        }
        else if (const auto* i = std::get_if<std::int64_t>(&param))
        {
            if ((*i != 0 && *i != 1) || asInt != *i || asDouble != static_cast<double>(*i))
            {
                InvariantFailure("a boolean parameter is not 0 or 1, or reads inconsistently");
            }
        }
        else if (asInt != 0 || asDouble != 0.0)
        {
            InvariantFailure("a string or null parameter reads as a non-zero number");
        }
        const auto* text = std::get_if<std::string>(&param);
        if (text != nullptr ? asString != *text : !asString.empty())
        {
            InvariantFailure("ActionParamToString disagrees with the parameter");
        }
    }

    void CheckRules(const std::vector<EventResponseRule>& rules, std::string_view json)
    {
        if (rules.size() > static_cast<std::size_t>(std::count(json.begin(), json.end(), '{')))
        {
            InvariantFailure("more rules were read than the text has objects");
        }
        for (const EventResponseRule& rule : rules)
        {
            if (static_cast<std::size_t>(rule.trigger) >= static_cast<std::size_t>(EventTriggerType::Count))
            {
                InvariantFailure("a rule has an unknown trigger kind");
            }
            if (!rule.conditions.IsEmpty())
            {
                InvariantFailure("a rule read conditions the format does not have");
            }
            for (const auto& action : rule.actions)
            {
                if (static_cast<std::size_t>(action.type) >= static_cast<std::size_t>(ActionType::Count))
                {
                    InvariantFailure("an action has an unknown kind");
                }
                for (const ActionParam& param : action.params)
                {
                    CheckParam(param);
                }
            }
        }
    }

    std::string Write(const std::vector<EventResponseRule>& rules)
    {
        std::ostringstream out;
        Spark::Gameplay::WriteEventResponseRules(out, rules);
        return out.str();
    }

    std::vector<EventResponseRule> Sentinel()
    {
        std::vector<EventResponseRule> rules(1);
        rules[0].name = "sentinel-rule";
        rules[0].sourceEntityId = 77;
        rules[0].actions.push_back({ActionType::Delay, {2.5}});
        return rules;
    }
} // namespace

extern "C" int SparkFuzzParseEventResponseRules(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string json = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    std::vector<EventResponseRule> rules = Sentinel();
    if (!Spark::Gameplay::ParseEventResponseRules(json, rules, "fuzz-rules.json"))
    {
        if (!SameRules(rules, Sentinel()))
        {
            InvariantFailure("a rejected file changed the caller's rules");
        }
        return 0;
    }
    CheckRules(rules, json);

    std::vector<EventResponseRule> again = Sentinel();
    if (!Spark::Gameplay::ParseEventResponseRules(json, again, "fuzz-rules.json") || !SameRules(rules, again))
    {
        InvariantFailure("parsing the same text twice gave different rules");
    }

    // A boolean parameter is written as an integer and reads back as a double, so the text is a
    // fixed point from the second write on.
    std::vector<EventResponseRule> reloaded;
    if (!Spark::Gameplay::ParseEventResponseRules(Write(rules), reloaded, "fuzz-rules-written.json"))
    {
        InvariantFailure("written rules no longer parse");
    }
    if (!SameRules(rules, reloaded))
    {
        InvariantFailure("write -> parse changed the rules");
    }
    const std::string written = Write(reloaded);
    std::vector<EventResponseRule> settled;
    if (!Spark::Gameplay::ParseEventResponseRules(written, settled, "fuzz-rules-rewritten.json") ||
        !SameRules(reloaded, settled) || Write(settled) != written)
    {
        InvariantFailure("rewritten rules are not a fixed point of write -> parse");
    }
    return 0;
}
