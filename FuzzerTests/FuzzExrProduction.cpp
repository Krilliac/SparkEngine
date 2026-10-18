/**
 * @file FuzzExrProduction.cpp
 * @brief libc++-compiled production adapter for the OpenEXR libFuzzer harness.
 *
 * EXRLoader::Load is header-only and decodes through the vendored tinyexr
 * (compiled into this target with the fuzzer's instrumentation, over the
 * instrumented miniz). Every accepted image is checked against the bounds the
 * loader promises, and every rejected one must leave the caller's image
 * untouched; a violation aborts so libFuzzer records it as a crash.
 */

#include "FuzzExrProduction.h"

#include "Graphics/EXRLoader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::uint64_t kMaxDimension = 16384;
    // EXRLoader bounds width * height * (channels + 4) floats by 128 MiB, and a
    // decodable file has at least three colour channels.
    constexpr std::uint64_t kMaxWorkingBytes = 128ull * 1024ull * 1024ull;
    constexpr std::uint64_t kMinWorkingBytesPerPixel = (3 + 4) * sizeof(float);

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzExr: EXRLoader::Load violated: %s\n", what);
        std::abort();
    }

    Spark::Graphics::EXRImage Sentinel()
    {
        Spark::Graphics::EXRImage image;
        image.pixels = {42.0f, -1.0f, 7.0f};
        image.width = 3;
        image.height = 1;
        image.channels = 1;
        image.isHDR = false;
        return image;
    }

    bool IsSentinel(const Spark::Graphics::EXRImage& image)
    {
        const Spark::Graphics::EXRImage expected = Sentinel();
        return image.pixels == expected.pixels && image.width == expected.width && image.height == expected.height &&
               image.channels == expected.channels && image.isHDR == expected.isHDR;
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production loader and tinyexr.
extern "C" int SparkFuzzLoadExr(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    Spark::Graphics::EXRImage image = Sentinel();
    if (!Spark::Graphics::EXRLoader::Load(data, size, image))
    {
        if (!IsSentinel(image))
            InvariantFailure("a rejected file modified the caller's image");
        return 0;
    }

    if (image.width == 0 || image.height == 0 || image.width > kMaxDimension || image.height > kMaxDimension)
        InvariantFailure("dimension bound");
    if (image.channels != 4 || !image.isHDR)
        InvariantFailure("RGBA float output");
    const std::uint64_t pixelCount = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.pixels.size() != pixelCount * 4)
        InvariantFailure("pixel buffer size");
    if (pixelCount > kMaxWorkingBytes / kMinWorkingBytesPerPixel)
        InvariantFailure("working-set bound");
    return 0;
}
