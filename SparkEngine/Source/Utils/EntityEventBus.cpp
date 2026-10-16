/**
 * @file EntityEventBus.cpp
 * @brief Out-of-line home of the EntityEventBus singleton (see EntityEventBus::Global).
 */

#include "Utils/EntityEventBus.h"

namespace Spark
{
    EntityEventBus& EntityEventBus::Global()
    {
        static EntityEventBus instance;
        return instance;
    }
} // namespace Spark
