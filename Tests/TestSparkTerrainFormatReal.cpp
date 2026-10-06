/**
 * @file TestSparkTerrainFormatReal.cpp
 * @brief Production tests for the runtime .sparkterrain decoder (SEC-120)
 *
 * Drives Spark::Graphics::SparkTerrain::DecodeRuntime and TerrainRenderer::LoadSparkTerrain, which is the
 * path TerrainSystem uses for an authored terrain asset. Registered as the pinned SparkTerrainFormat_
 * family in Tests/CMakeLists.txt.
 */

#include "TestFramework.h"

#include "Engine/ECS/Components/TerrainComponents.h"
#include "Graphics/TerrainAssetFormat.h"
#include "Graphics/TerrainRenderer.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    namespace Format = Spark::Graphics::SparkTerrain;

    template <typename T> void Put(std::string& bytes, const T& value)
    {
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    void PutString(std::string& bytes, const std::string& text)
    {
        Put(bytes, static_cast<uint32_t>(text.size()));
        bytes.append(text);
    }

    /// A .sparkterrain in the layout TerrainEditor::SaveTerrain writes (see TerrainAssetFormat.h), with one
    /// texture layer, a 2x2 splatmap and one detail mesh.
    std::string EditorLayout(int32_t width, int32_t height)
    {
        std::string bytes;
        Put(bytes, Format::kMagic);
        Put(bytes, Format::kVersion);
        PutString(bytes, "Ridge");
        Put(bytes, 256.0f); // size
        Put(bytes, 1.0f);   // position
        Put(bytes, 2.0f);
        Put(bytes, 3.0f);
        Put(bytes, int32_t{3}); // lodLevels
        Put(bytes, 1.5f);       // lodBias
        Put(bytes, uint8_t{0}); // generateCollider
        Put(bytes, width);
        Put(bytes, height);
        Put(bytes, 2.0f);  // heightScale
        Put(bytes, -8.0f); // minHeight
        Put(bytes, 40.0f); // maxHeight
        for (int32_t i = 0; i < width * height; ++i)
            Put(bytes, static_cast<float>(i) * 0.25f);

        Put(bytes, uint32_t{1});
        PutString(bytes, "Rock");
        PutString(bytes, "Textures/Terrain/Rock_D.png");
        PutString(bytes, "Textures/Terrain/Rock_N.png");
        PutString(bytes, "");
        for (int i = 0; i < 8; ++i)
            Put(bytes, 1.0f);

        Put(bytes, int32_t{2}); // splatmapResolution
        for (uint8_t i = 0; i < 16; ++i)
            Put(bytes, i);

        Put(bytes, uint32_t{1}); // detail meshes: present in the file, not consumed by the runtime
        PutString(bytes, "Grass");
        PutString(bytes, "Meshes/Grass.fbx");
        PutString(bytes, "");
        Put(bytes, 1.0f);
        Put(bytes, 50.0f);
        Put(bytes, uint32_t{0});
        return bytes;
    }

    std::filesystem::path WriteScratch(const std::string& name, const std::string& bytes)
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("spark_terrain_format_" + std::to_string(ticks) + "_" + name);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
} // namespace

TEST(SparkTerrainFormat_RejectsNonSquareHeightmap)
{
    // 8x2: every field is in bounds, but the runtime publishes one resolution (8) and TerrainSystem
    // uploads 8x8 samples to the clipmap from a 16-sample heightmap.
    const std::string bytes = EditorLayout(8, 2);

    std::istringstream stream(bytes);
    Format::RuntimeTerrain decoded;
    std::string error;
    EXPECT_FALSE(Format::DecodeRuntime(stream, decoded, error));
    EXPECT_STR_CONTAINS(error, "not square");
    EXPECT_TRUE(decoded.heights.empty());
    EXPECT_EQ(decoded.resolution, 0);

    const std::filesystem::path path = WriteScratch("nonsquare.sparkterrain", bytes);
    TerrainComponent component;
    EXPECT_FALSE(Spark::Graphics::TerrainRenderer::LoadSparkTerrain(path.string(), component));
    EXPECT_TRUE(component.heightmap.empty());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SparkTerrainFormat_RoundTripsEditorLayout)
{
    std::istringstream stream(EditorLayout(3, 3));
    Format::RuntimeTerrain decoded;
    std::string error;
    ASSERT_TRUE(Format::DecodeRuntime(stream, decoded, error));

    EXPECT_EQ(decoded.name, std::string("Ridge"));
    EXPECT_NEAR(decoded.size, 256.0f, 0.0001f);
    EXPECT_EQ(decoded.lodLevels, 3);
    EXPECT_NEAR(decoded.lodBias, 1.5f, 0.0001f);
    EXPECT_FALSE(decoded.generateCollider);
    EXPECT_EQ(decoded.resolution, 3);
    EXPECT_NEAR(decoded.heightScale, 2.0f, 0.0001f);
    EXPECT_NEAR(decoded.minHeight, -8.0f, 0.0001f);
    EXPECT_NEAR(decoded.maxHeight, 40.0f, 0.0001f);
    ASSERT_EQ(decoded.heights.size(), size_t{9});
    EXPECT_NEAR(decoded.heights[8], 2.0f, 0.0001f);
    ASSERT_EQ(decoded.layerDiffusePaths.size(), size_t{1});
    EXPECT_EQ(decoded.layerDiffusePaths[0], std::string("Textures/Terrain/Rock_D.png"));
    EXPECT_EQ(decoded.splatResolution, 2);
    ASSERT_EQ(decoded.splatmap.size(), size_t{16});
    EXPECT_EQ(static_cast<int>(decoded.splatmap[15]), 15);

    // The same bytes through the file loader reach the component TerrainSystem consumes.
    const std::filesystem::path path = WriteScratch("valid.sparkterrain", EditorLayout(3, 3));
    TerrainComponent component;
    ASSERT_TRUE(Spark::Graphics::TerrainRenderer::LoadSparkTerrain(path.string(), component));
    EXPECT_EQ(component.heightmapResolution, 3);
    EXPECT_EQ(component.heightmap.size(), size_t{9});
    EXPECT_EQ(component.lodLevels, 3);
    EXPECT_EQ(component.splatmapResolution, 2);
    EXPECT_TRUE(component.dirty);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
