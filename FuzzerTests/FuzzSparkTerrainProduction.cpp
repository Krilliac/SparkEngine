/**
 * @file FuzzSparkTerrainProduction.cpp
 * @brief libc++-compiled production adapter for the .sparkterrain libFuzzer harness.
 *
 * TerrainRenderer::LoadSparkTerrain opens the file, hands the stream to SparkTerrain::DecodeRuntime and
 * copies the result onto the TerrainComponent that TerrainSystem uploads to the clipmap as
 * resolution x resolution samples. The adapter decodes the fuzz input from an in-memory stream and
 * checks what those consumers index. A violation aborts so libFuzzer records a crash rather than a
 * silent pass:
 *  - a rejected asset leaves the caller's RuntimeTerrain untouched,
 *  - heights holds exactly resolution^2 samples, with resolution in the format's bounds,
 *  - splatmap holds exactly splatResolution^2 RGBA texels,
 *  - every published float is finite, minHeight <= maxHeight, lodLevels is in range, and there are at
 *    most kMaxTextureLayers layer paths of at most kMaxStringLength bytes.
 */

#include "FuzzSparkTerrainProduction.h"

#include "Graphics/TerrainAssetFormat.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace
{
    namespace Format = Spark::Graphics::SparkTerrain;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzSparkTerrain: DecodeRuntime violated: %s\n", what);
        std::abort();
    }

    void CheckAccepted(const Format::RuntimeTerrain& terrain)
    {
        if (terrain.resolution < Format::kMinHeightmapResolution ||
            terrain.resolution > Format::kMaxHeightmapResolution)
            InvariantFailure("heightmap resolution outside the format bounds");
        const std::uint64_t resolution = static_cast<std::uint64_t>(terrain.resolution);
        if (terrain.heights.size() != resolution * resolution)
            InvariantFailure("heights does not hold resolution^2 samples");
        for (const float sample : terrain.heights)
        {
            if (!std::isfinite(sample))
                InvariantFailure("non-finite height sample");
        }

        if (terrain.splatResolution < 0 || terrain.splatResolution > Format::kMaxSplatmapResolution)
            InvariantFailure("splatmap resolution outside the format bounds");
        const std::uint64_t splat = static_cast<std::uint64_t>(terrain.splatResolution);
        if (terrain.splatmap.size() != splat * splat * 4u)
            InvariantFailure("splatmap does not hold splatResolution^2 RGBA texels");

        if (!std::isfinite(terrain.size) || !std::isfinite(terrain.lodBias) || !std::isfinite(terrain.heightScale) ||
            !std::isfinite(terrain.minHeight) || !std::isfinite(terrain.maxHeight))
            InvariantFailure("non-finite published float");
        if (terrain.minHeight > terrain.maxHeight)
            InvariantFailure("minHeight exceeds maxHeight");
        if (terrain.lodLevels < Format::kMinLodLevels || terrain.lodLevels > Format::kMaxLodLevels)
            InvariantFailure("LOD level count outside the supported range");

        if (terrain.layerDiffusePaths.size() > Format::kMaxTextureLayers)
            InvariantFailure("more texture layers than the format allows");
        for (const std::string& path : terrain.layerDiffusePaths)
        {
            if (path.size() > Format::kMaxStringLength)
                InvariantFailure("layer path longer than the format allows");
        }
        if (terrain.name.size() > Format::kMaxStringLength)
            InvariantFailure("terrain name longer than the format allows");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzDecodeSparkTerrain(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    std::istringstream stream(size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size));

    // A sentinel: a rejected decode must not touch it.
    Format::RuntimeTerrain terrain;
    terrain.name = "sentinel";
    terrain.resolution = -7;
    std::string error;
    if (!Format::DecodeRuntime(stream, terrain, error))
    {
        if (error.empty())
            InvariantFailure("rejected without a reason");
        if (terrain.name != "sentinel" || terrain.resolution != -7 || !terrain.heights.empty() ||
            !terrain.splatmap.empty() || !terrain.layerDiffusePaths.empty())
            InvariantFailure("a rejected asset modified the caller's terrain");
        return 0;
    }

    CheckAccepted(terrain);
    return 0;
}
