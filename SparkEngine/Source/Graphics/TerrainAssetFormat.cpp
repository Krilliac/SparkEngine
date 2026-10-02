/**
 * @file TerrainAssetFormat.cpp
 * @brief Runtime .sparkterrain decoder (SparkTerrain::DecodeRuntime)
 *
 * Kept out of TerrainRenderer.cpp so the decoder has no renderer, device or logger dependency: the
 * SparkFuzzSparkTerrain harness links this file alone.
 */

#include "TerrainAssetFormat.h"

#include <cmath>
#include <format>
#include <utility>

namespace Spark::Graphics::SparkTerrain
{

    bool DecodeRuntime(std::istream& stream, RuntimeTerrain& out, std::string& error)
    {
        Reader reader(stream);

        uint32_t magic = 0;
        uint32_t version = 0;
        reader.ReadU32(magic);
        reader.ReadU32(version);
        if (reader.Failed() || magic != kMagic)
        {
            error = std::format("invalid magic 0x{:08X} (expected 0x{:08X})", magic, kMagic);
            return false;
        }
        if (version != kVersion)
        {
            error = std::format("unsupported version {} (expected {})", version, kVersion);
            return false;
        }

        RuntimeTerrain terrain;
        reader.ReadString(terrain.name);

        // Position is authored in the editor but placement comes from the entity transform at runtime.
        float position[3] = {};
        uint8_t generateCollider = 1;
        reader.ReadF32(terrain.size);
        reader.ReadF32(position[0]);
        reader.ReadF32(position[1]);
        reader.ReadF32(position[2]);
        reader.ReadI32(terrain.lodLevels);
        reader.ReadF32(terrain.lodBias);
        reader.ReadU8(generateCollider);
        terrain.generateCollider = generateCollider != 0;

        int32_t width = 0;
        int32_t height = 0;
        reader.ReadI32(width);
        reader.ReadI32(height);
        reader.ReadF32(terrain.heightScale);
        reader.ReadF32(terrain.minHeight);
        reader.ReadF32(terrain.maxHeight);
        if (reader.Failed())
        {
            error = "truncated header";
            return false;
        }
        if (!std::isfinite(terrain.size) || !std::isfinite(terrain.lodBias) || !std::isfinite(terrain.heightScale) ||
            !std::isfinite(terrain.minHeight) || !std::isfinite(terrain.maxHeight))
        {
            error = "non-finite size, LOD bias, height scale or height range";
            return false;
        }
        if (terrain.minHeight > terrain.maxHeight)
        {
            error = std::format("minHeight {} exceeds maxHeight {}", terrain.minHeight, terrain.maxHeight);
            return false;
        }
        if (terrain.lodLevels < kMinLodLevels || terrain.lodLevels > kMaxLodLevels)
        {
            error = std::format("LOD level count {} outside [{}, {}]", terrain.lodLevels, kMinLodLevels, kMaxLodLevels);
            return false;
        }
        if (width < kMinHeightmapResolution || width > kMaxHeightmapResolution)
        {
            error = std::format("heightmap {}x{} outside [{}, {}]", width, height, kMinHeightmapResolution,
                                kMaxHeightmapResolution);
            return false;
        }
        // The runtime has one resolution field and TerrainSystem uploads resolution x resolution samples;
        // a non-square map would publish fewer samples than every consumer indexes.
        if (height != width)
        {
            error = std::format("heightmap {}x{} is not square", width, height);
            return false;
        }
        terrain.resolution = width;

        const uint64_t heightCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(width);
        if (heightCount * sizeof(float) > reader.Remaining())
        {
            error = "truncated heightmap";
            return false;
        }
        terrain.heights.resize(static_cast<size_t>(heightCount));
        reader.ReadBytes(terrain.heights.data(), heightCount * sizeof(float));
        for (const float sample : terrain.heights)
        {
            if (!std::isfinite(sample))
            {
                error = "non-finite heightmap sample";
                return false;
            }
        }

        uint32_t layerCount = 0;
        reader.ReadU32(layerCount);
        if (reader.Failed() || layerCount > kMaxTextureLayers)
        {
            error = std::format("texture layer count {} (max {})", layerCount, kMaxTextureLayers);
            return false;
        }
        terrain.layerDiffusePaths.reserve(layerCount);
        for (uint32_t i = 0; i < layerCount && !reader.Failed(); ++i)
        {
            std::string layerName, diffuseTexture, normalTexture, maskTexture;
            reader.ReadString(layerName);
            reader.ReadString(diffuseTexture);
            reader.ReadString(normalTexture);
            reader.ReadString(maskTexture);

            // Per-layer material parameters have no TerrainComponent field yet; they are consumed so the
            // cursor stays aligned with the writer.
            float layerFloats[8] = {};
            for (float& value : layerFloats)
            {
                reader.ReadF32(value);
            }

            terrain.layerDiffusePaths.push_back(std::move(diffuseTexture));
        }
        if (reader.Failed())
        {
            error = "truncated texture layers";
            return false;
        }

        reader.ReadI32(terrain.splatResolution);
        if (reader.Failed() || terrain.splatResolution < 0 || terrain.splatResolution > kMaxSplatmapResolution)
        {
            error =
                std::format("splatmap resolution {} outside [0, {}]", terrain.splatResolution, kMaxSplatmapResolution);
            return false;
        }
        const uint64_t splatBytes =
            static_cast<uint64_t>(terrain.splatResolution) * static_cast<uint64_t>(terrain.splatResolution) * 4u;
        if (splatBytes > reader.Remaining())
        {
            error = "truncated splatmap";
            return false;
        }
        terrain.splatmap.resize(static_cast<size_t>(splatBytes));
        reader.ReadBytes(terrain.splatmap.data(), splatBytes);
        if (reader.Failed())
        {
            error = "truncated splatmap";
            return false;
        }

        // Detail meshes follow; the runtime has no consumer for them yet, so they are intentionally
        // not read here. The splatmap is the last field the runtime needs.
        out = std::move(terrain);
        return true;
    }

} // namespace Spark::Graphics::SparkTerrain
