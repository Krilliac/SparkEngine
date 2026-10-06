/**
 * @file FuzzConfigParserProduction.h
 * @brief C ABI adapter for the INI configuration parser (ConfigParser::LoadFromString).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseConfig(const std::uint8_t* data, std::size_t size);
