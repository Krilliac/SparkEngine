/**
 * @file FuzzSceneManifestProduction.h
 * @brief C ABI adapter for the .sparkscene production parser (SceneManifest::ParseFromString).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseSceneManifest(const std::uint8_t* data, std::size_t size);
