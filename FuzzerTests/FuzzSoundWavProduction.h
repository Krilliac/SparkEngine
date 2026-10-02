/**
 * @file FuzzSoundWavProduction.h
 * @brief C ABI adapter for the SoundEffect WAV production parser (SoundEffect::LoadFromMemory).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as a WAV file through the shipped SoundEffect loader.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadWav(const std::uint8_t* data, std::size_t size);
