/**
 * @file FuzzShaderBlobProduction.cpp
 * @brief libc++-compiled production adapter for the shader-daemon blob libFuzzer harness.
 *
 * The fuzz input is handed to the shipped DecodeCompiledShaderBlob, the decoder
 * ShaderDiskCache::Lookup runs on bytes another process (the shader daemon)
 * returned. A violation of the decoder's contract aborts so libFuzzer records a
 * crash rather than a silent pass:
 *  - an accepted blob's bytecode never exceeds kMaxShaderDaemonBytecodeBytes or
 *    the input length (a length field may not size memory the input lacks),
 *  - an accepted blob re-encodes through EncodeCompiledShaderBlob to exactly
 *    the input prefix the decoder consumed (the success byte normalised to
 *    0/1), and that encoding decodes to a field-equal blob,
 *  - a rejected blob leaves the caller's output untouched, the publish-on-success
 *    contract DecodeCompiledShaderBlob documents.
 */

#include "FuzzShaderBlobProduction.h"

#include "Graphics/ShaderDaemonBridge.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // Offset of the success byte in the wire header ([u8 version][u8 target][u8 stage][u8 success]).
    constexpr std::size_t kSuccessByteOffset = 3;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzShaderBlob: DecodeCompiledShaderBlob violated: %s\n", what);
        std::abort();
    }

    Spark::Graphics::CompiledShaderBlob MakeSentinel()
    {
        Spark::Graphics::CompiledShaderBlob sentinel;
        sentinel.bytecode = {0xA5, 0x5A};
        sentinel.target = Spark::Graphics::ShaderTarget::SPIRV;
        sentinel.stage = Spark::Graphics::ShaderStage::Compute;
        sentinel.entryPoint = "sentinel-entry";
        sentinel.errors = "sentinel-errors";
        sentinel.success = true;
        sentinel.inputCount = 0xDEADBEEFu;
        sentinel.outputCount = 0xDEADBEEFu;
        sentinel.cbufferCount = 0xDEADBEEFu;
        sentinel.textureCount = 0xDEADBEEFu;
        sentinel.samplerCount = 0xDEADBEEFu;
        return sentinel;
    }

    bool SameBlob(const Spark::Graphics::CompiledShaderBlob& a, const Spark::Graphics::CompiledShaderBlob& b)
    {
        return a.bytecode == b.bytecode && a.target == b.target && a.stage == b.stage && a.entryPoint == b.entryPoint &&
               a.errors == b.errors && a.success == b.success && a.inputCount == b.inputCount &&
               a.outputCount == b.outputCount && a.cbufferCount == b.cbufferCount && a.textureCount == b.textureCount &&
               a.samplerCount == b.samplerCount;
    }

    void CheckAccepted(const std::vector<std::uint8_t>& input, const Spark::Graphics::CompiledShaderBlob& decoded)
    {
        if (decoded.bytecode.size() > Spark::Graphics::kMaxShaderDaemonBytecodeBytes)
            InvariantFailure("accepted bytecode above kMaxShaderDaemonBytecodeBytes");
        if (decoded.bytecode.size() + decoded.entryPoint.size() + decoded.errors.size() > input.size())
            InvariantFailure("accepted blob holds more payload bytes than the input");

        const std::vector<std::uint8_t> reencoded = Spark::Graphics::EncodeCompiledShaderBlob(decoded);
        if (reencoded.size() > input.size())
            InvariantFailure("re-encoding is longer than the accepted input");
        for (std::size_t index = 0; index < reencoded.size(); ++index)
        {
            std::uint8_t expected = input[index];
            if (index == kSuccessByteOffset)
                expected = expected != 0 ? 1u : 0u;
            if (reencoded[index] != expected)
                InvariantFailure("re-encoding differs from the consumed input prefix");
        }

        Spark::Graphics::CompiledShaderBlob again = MakeSentinel();
        if (!Spark::Graphics::DecodeCompiledShaderBlob(reencoded, again))
            InvariantFailure("re-encoded blob is rejected");
        if (!SameBlob(again, decoded))
            InvariantFailure("re-encoded blob decodes to different fields");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzDecodeShaderBlob(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::vector<std::uint8_t> input =
        size == 0 ? std::vector<std::uint8_t>() : std::vector<std::uint8_t>(data, data + size);
    const Spark::Graphics::CompiledShaderBlob sentinel = MakeSentinel();
    Spark::Graphics::CompiledShaderBlob decoded = MakeSentinel();
    if (!Spark::Graphics::DecodeCompiledShaderBlob(input, decoded))
    {
        if (!SameBlob(decoded, sentinel))
            InvariantFailure("a rejected blob modified the caller's output");
        return 0;
    }
    CheckAccepted(input, decoded);
    return 0;
}
