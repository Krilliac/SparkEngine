/**
 * @file LifecycleStages.h
 * @brief Factory functions for concrete lifecycle stage instances
 */

#pragma once

#include "Core/Lifecycle/LifecycleStage.h"

#include <memory>

namespace Spark::Core::Lifecycle
{
    std::unique_ptr<LifecycleStage> CreateInitDebugStage();
    std::unique_ptr<LifecycleStage> CreateInitNetworkingStage();
    std::unique_ptr<LifecycleStage> CreateInitGameplayStage();
    std::unique_ptr<LifecycleStage> CreateUpdateStage();
    std::unique_ptr<LifecycleStage> CreateShutdownStage();
} // namespace Spark::Core::Lifecycle
