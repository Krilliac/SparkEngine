/**
 * @file Hash.h
 * @brief Compile-time and runtime hash utilities for strings and composite keys
 * @author Spark Engine Team
 * @date 2026
 *
 * Umbrella header for the hash utilities. Provides FNV-1a hashing (32-bit and
 * 64-bit) with `constexpr` support so string keys can be hashed at compile
 * time. Includes a hash-combining helper modelled after `boost::hash_combine`
 * for building composite hash values from multiple fields, user-defined
 * literals for ergonomic compile-time hashing, and transparent string functors
 * for heterogeneous map lookup.
 *
 * The implementation lives in three sibling headers, all included here:
 * - `HashFNV1a.h` — FNV-1a 32/64-bit hashing and `HashLiterals`
 * - `HashCombine.h` — composite hash combining helpers
 * - `HashTransparentString.h` — transparent string hash/equality functors
 *
 * ## Why FNV-1a?
 * FNV-1a is simple, dependency-free, produces excellent distribution for short
 * ASCII strings (common in game engines: asset names, command names, component
 * type tags), and is `constexpr`-friendly with no lookup tables.
 *
 * ## Usage
 * @code
 *   using namespace Spark::HashLiterals;
 *
 *   // Compile-time string hash (no runtime cost)
 *   constexpr uint64_t key = "player_spawn"_hash64;
 *   constexpr uint32_t key32 = "player_spawn"_hash32;
 *
 *   // Runtime hashing
 *   uint64_t h = Spark::FNV1a64("some_asset_name");
 *
 *   // Composite key for std::unordered_map
 *   size_t combined = Spark::CombineHash(typeHash, instanceId);
 *
 *   // Switch on a string (compile-time dispatch)
 *   switch (Spark::FNV1a64(commandName)) {
 *       case "quit"_hash64:  DoQuit();  break;
 *       case "reload"_hash64: DoReload(); break;
 *   }
 * @endcode
 */

#pragma once

#include "HashCombine.h"
#include "HashFNV1a.h"
#include "HashTransparentString.h"
