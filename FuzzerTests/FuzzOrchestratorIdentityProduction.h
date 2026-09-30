/**
 * @file FuzzOrchestratorIdentityProduction.h
 * @brief C ABI adapter for the SparkOrchestrator mutation identity state reader
 *        (Spark::Daemon::OrchestratorIdentityLease::Acquire).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzAcquireOrchestratorIdentity(const std::uint8_t* data, std::size_t size);
