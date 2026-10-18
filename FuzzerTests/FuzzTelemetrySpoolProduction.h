/**
 * @file FuzzTelemetrySpoolProduction.h
 * @brief C ABI adapter for the telemetry spool decoder (TelemetryDetail::Parse).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseTelemetrySpool(const std::uint8_t* data, std::size_t size);
