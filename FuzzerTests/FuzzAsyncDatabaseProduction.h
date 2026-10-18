/**
 * @file FuzzAsyncDatabaseProduction.h
 * @brief C ABI adapter for the AsyncDatabase key/value store loader
 *        (Spark::Persistence::SQLiteConnection::Open).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzOpenAsyncDatabase(const std::uint8_t* data, std::size_t size);
