/**
 * @file FuzzGltfProduction.h
 * @brief C ABI adapter for the glTF/GLB production loaders (static mesh, skinned mesh, skin animations).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Load @p data as a .gltf/.glb file through every shipped glTF loader.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadGltf(const std::uint8_t* data, std::size_t size);
