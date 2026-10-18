/**
 * @file FuzzObjStaticProduction.h
 * @brief C ABI adapter for the OBJ static-mesh production loader (Detail::LoadOBJStaticMesh).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzLoadObjStatic(const std::uint8_t* data, std::size_t size);
