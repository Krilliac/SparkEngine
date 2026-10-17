/**
 * @file FuzzOrchestrationJournalProduction.h
 * @brief C ABI adapter for the SparkDaemon orchestration journal recovery path
 *        (Spark::Daemon::RecoverOrchestrationJournal).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzRecoverOrchestrationJournal(const std::uint8_t* data, std::size_t size);
