/**
 * @file EngineSdkStateValidation.cpp
 * @brief Routes SDK IStateValidation calls from game modules to the host InvalidStateDetector.
 */

#include "EngineSdkStateValidation.h"

#include "Utils/InvalidStateDetector.h"

#include <string>
#include <utility>

bool EngineSdkStateValidation::AddRule(std::string_view name, std::string_view category,
                                       Spark::StateViolationSeverity severity, Spark::StateCheckFn check)
{
    // A nameless rule cannot be reported, a category-less one cannot be removed by its module, and an empty
    // check would throw when the detector runs it: refuse all three up front.
    if (name.empty() || category.empty() || !check)
    {
        return false;
    }
    // A rule added before the detector starts is kept: modules load ahead of the lifecycle on the windowed hosts.
    Spark::StateValidationRule rule;
    rule.name = std::string(name);
    rule.category = std::string(category);
    rule.severity = severity;
    rule.enabled = true;
    rule.checkFn = std::move(check);
    Spark::InvalidStateDetector::GetInstance().AddRule(std::move(rule));
    return true;
}

void EngineSdkStateValidation::RemoveRulesByCategory(std::string_view category)
{
    Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory(std::string(category));
}
