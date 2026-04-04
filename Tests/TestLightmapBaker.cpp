// TestLightmapBaker.cpp - Tests for Spark::Graphics::LightmapBaker
#include "TestFramework.h"
#include "Graphics/LightmapBaker.h"

// Helper to create a simple triangle with lightmap UVs
static Spark::Graphics::BakeTriangle MakeTriangle(float x0, float y0, float z0, float x1, float y1, float z1, float x2,
                                                  float y2, float z2, float u0, float v0, float u1, float v1, float u2,
                                                  float v2)
{
    Spark::Graphics::BakeTriangle tri;
    tri.v0[0] = x0;
    tri.v0[1] = y0;
    tri.v0[2] = z0;
    tri.v1[0] = x1;
    tri.v1[1] = y1;
    tri.v1[2] = z1;
    tri.v2[0] = x2;
    tri.v2[1] = y2;
    tri.v2[2] = z2;
    // Upward normal
    tri.n0[0] = 0;
    tri.n0[1] = 1;
    tri.n0[2] = 0;
    tri.n1[0] = 0;
    tri.n1[1] = 1;
    tri.n1[2] = 0;
    tri.n2[0] = 0;
    tri.n2[1] = 1;
    tri.n2[2] = 0;
    tri.uv0[0] = u0;
    tri.uv0[1] = v0;
    tri.uv1[0] = u1;
    tri.uv1[1] = v1;
    tri.uv2[0] = u2;
    tri.uv2[1] = v2;
    return tri;
}

// ============================================================================
// Initialization
// ============================================================================

TEST(LightmapBaker_Initialize)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();
    EXPECT_EQ(baker.GetMeshCount(), static_cast<size_t>(0));
    EXPECT_EQ(baker.GetLightCount(), static_cast<size_t>(0));
    baker.Shutdown();
}

// ============================================================================
// Scene building
// ============================================================================

TEST(LightmapBaker_AddMesh)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    Spark::Graphics::BakeMesh mesh;
    mesh.name = "Floor";
    mesh.triangles.push_back(MakeTriangle(0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1));
    baker.AddMesh(mesh);

    EXPECT_EQ(baker.GetMeshCount(), static_cast<size_t>(1));
    EXPECT_EQ(baker.GetTriangleCount(), static_cast<size_t>(1));
    baker.Shutdown();
}

TEST(LightmapBaker_AddLights)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    baker.AddDirectionalLight(0, -1, 0, 1, 1, 1, 1.0f);
    baker.AddPointLight(5, 3, 0, 1, 0.8f, 0.6f, 2.0f, 15.0f);

    EXPECT_EQ(baker.GetLightCount(), static_cast<size_t>(2));
    baker.Shutdown();
}

TEST(LightmapBaker_ClearScene)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    Spark::Graphics::BakeMesh mesh;
    mesh.triangles.push_back(MakeTriangle(0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1));
    baker.AddMesh(mesh);
    baker.AddDirectionalLight(0, -1, 0, 1, 1, 1);

    baker.ClearScene();
    EXPECT_EQ(baker.GetMeshCount(), static_cast<size_t>(0));
    EXPECT_EQ(baker.GetLightCount(), static_cast<size_t>(0));
    baker.Shutdown();
}

// ============================================================================
// Baking
// ============================================================================

TEST(LightmapBaker_Bake_EmptyScene)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    Spark::Graphics::BakeSettings settings;
    settings.resolution = 16;
    auto result = baker.Bake(settings);
    EXPECT_FALSE(result.success);
    baker.Shutdown();
}

TEST(LightmapBaker_Bake_WithGeometry)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    // Create a quad covering UV space [0,0]-[1,1]
    Spark::Graphics::BakeMesh mesh;
    mesh.name = "Ground";
    mesh.triangles.push_back(MakeTriangle(0, 0, 0, 10, 0, 0, 0, 0, 10, 0, 0, 1, 0, 0, 1));
    mesh.triangles.push_back(MakeTriangle(10, 0, 0, 10, 0, 10, 0, 0, 10, 1, 0, 1, 1, 0, 1));
    baker.AddMesh(mesh);

    baker.AddDirectionalLight(0, -1, 0.5f, 1.0f, 1.0f, 0.95f, 1.0f);

    Spark::Graphics::BakeSettings settings;
    settings.resolution = 8;
    settings.samplesPerTexel = 4;
    settings.bounces = 1;
    settings.denoise = false;

    auto result = baker.Bake(settings);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.width, 8);
    EXPECT_EQ(result.height, 8);
    EXPECT_EQ(result.lightmapData.size(), static_cast<size_t>(8 * 8 * 3));
    EXPECT_GT(result.totalSamples, 0);
    baker.Shutdown();
}

TEST(LightmapBaker_Bake_WithDenoise)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    Spark::Graphics::BakeMesh mesh;
    mesh.name = "Surface";
    mesh.triangles.push_back(MakeTriangle(0, 0, 0, 10, 0, 0, 0, 0, 10, 0, 0, 1, 0, 0, 1));
    baker.AddMesh(mesh);
    baker.AddDirectionalLight(0, -1, 0, 1, 1, 1, 1.0f);

    Spark::Graphics::BakeSettings settings;
    settings.resolution = 8;
    settings.denoise = true;
    settings.denoiseRadius = 1;

    auto result = baker.Bake(settings);
    EXPECT_TRUE(result.success);
    baker.Shutdown();
}

TEST(LightmapBaker_Bake_PointLight)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    Spark::Graphics::BakeMesh mesh;
    mesh.name = "Floor";
    mesh.triangles.push_back(MakeTriangle(0, 0, 0, 10, 0, 0, 0, 0, 10, 0, 0, 1, 0, 0, 1));
    baker.AddMesh(mesh);

    baker.AddPointLight(5, 5, 5, 1, 0.5f, 0.2f, 2.0f, 20.0f);

    Spark::Graphics::BakeSettings settings;
    settings.resolution = 4;

    auto result = baker.Bake(settings);
    EXPECT_TRUE(result.success);
    baker.Shutdown();
}

// ============================================================================
// Console status
// ============================================================================

TEST(LightmapBaker_ConsoleStatus)
{
    auto& baker = Spark::Graphics::LightmapBaker::GetInstance();
    baker.Initialize();

    Spark::Graphics::BakeMesh mesh;
    mesh.triangles.push_back(MakeTriangle(0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1));
    baker.AddMesh(mesh);

    std::string status = baker.Console_GetStatus();
    EXPECT_TRUE(status.find("1 meshes") != std::string::npos);
    EXPECT_TRUE(status.find("1 triangles") != std::string::npos);
    baker.Shutdown();
}
