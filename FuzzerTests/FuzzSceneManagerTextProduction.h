/**
 * @file FuzzSceneManagerTextProduction.h
 * @brief C ABI adapter for SceneManager's text scene readers (SceneTextFormat.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseSceneManagerText(const std::uint8_t* data, std::size_t size);
