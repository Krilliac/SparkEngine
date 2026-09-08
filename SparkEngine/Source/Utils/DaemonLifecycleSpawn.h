/**
 * @file DaemonLifecycleSpawn.h
 * @brief Private endpoint readiness and daemon auto-spawn contract
 */

#pragma once

#include <string>

namespace Spark::Daemon::Detail
{

    bool TrySpawnDaemon(const std::string& socketPath, const std::string& binaryOverride);

} // namespace Spark::Daemon::Detail
