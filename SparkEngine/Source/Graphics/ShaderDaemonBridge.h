/**
 * @file ShaderDaemonBridge.h
 * @brief Translate between engine `CompiledShaderBlob` and daemon opaque bytes.
 *
 * The daemon's `ShaderService` is domain-agnostic — it stores raw
 * `vector<uint8_t>` blobs. The engine's `CompiledShaderBlob` carries extra
 * metadata (target, stage, entry point, errors, reflection counters). These
 * helpers serialise the full struct into a single byte buffer that the
 * daemon can store and the engine can reconstruct.
 *
 * Lives in `Graphics/` so the daemon target (which doesn't link
 * `SparkEngineLib`) is not exposed to `Spark::Graphics::*` types. The
 * engine-side client wrapper `ShaderServiceClient` takes raw bytes; these
 * helpers sit on top of it.
 *
 * ## Wire format (engine ↔ daemon blob bytes)
 *
 * ```
 *   [u8 version][u8 target][u8 stage][u8 success]
 *   [u32 bytecodeLen][bytecodeBytes]
 *   [u32 entryPointLen][entryPointBytes]
 *   [u32 errorsLen][errorsBytes]
 *   [u32 inputCount][u32 outputCount][u32 cbufferCount]
 *   [u32 textureCount][u32 samplerCount]
 * ```
 *
 * `version = 1`. Bump it when the layout changes; decoders should reject
 * unknown versions so a newer daemon + older engine cleanly misses
 * instead of deserialising garbage.
 */

#pragma once

#include "ShaderCrossCompiler.h"

#include <cstdint>
#include <vector>

namespace Spark::Graphics
{

    inline constexpr uint8_t kShaderDaemonBlobVersion = 1;

    /// Serialise a compiled shader blob into bytes suitable for
    /// `ShaderServiceClient::PutCacheEntry`.
    [[nodiscard]] std::vector<uint8_t> EncodeCompiledShaderBlob(const CompiledShaderBlob& blob);

    /// Parse bytes from `ShaderServiceClient::GetCacheEntry` back into a
    /// `CompiledShaderBlob`. Returns false on version mismatch or truncation.
    [[nodiscard]] bool DecodeCompiledShaderBlob(const std::vector<uint8_t>& bytes, CompiledShaderBlob& out);

} // namespace Spark::Graphics
