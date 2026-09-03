// TestCSGSystem.cpp - Tests for Spark::LevelDesign::CSGSystem
#include "TestFramework.h"
#include "Engine/LevelDesign/CSGSystem.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(CSG_Initialize)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(0));
    csg.Shutdown();
}

// ============================================================================
// Brush creation
// ============================================================================

TEST(CSG_CreateBox)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {2.0f, 3.0f, 4.0f});
    EXPECT_TRUE(id > 0);
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(1));
    auto* brush = csg.GetBrush(id);
    ASSERT_TRUE(brush != nullptr);
    EXPECT_EQ(brush->faces.size(), static_cast<size_t>(6)); // Box has 6 faces
    csg.Shutdown();
}

TEST(CSG_CreateCylinder)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    csg.SetCylinderSegments(8);
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Cylinder, {1.0f, 3.0f, 0.0f});
    EXPECT_TRUE(id > 0);
    auto* brush = csg.GetBrush(id);
    ASSERT_TRUE(brush != nullptr);
    // 8 side faces + 2 caps = 10
    EXPECT_EQ(brush->faces.size(), static_cast<size_t>(10));
    csg.Shutdown();
}

TEST(CSG_CreateSphere)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    csg.SetSphereTessellation(4, 8);
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Sphere, {1.0f, 0, 0});
    EXPECT_TRUE(id > 0);
    auto* brush = csg.GetBrush(id);
    ASSERT_TRUE(brush != nullptr);
    EXPECT_TRUE(brush->faces.size() > 0);
    csg.Shutdown();
}

TEST(CSG_CreateWedge)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Wedge, {2.0f, 2.0f, 2.0f});
    EXPECT_TRUE(id > 0);
    auto* brush = csg.GetBrush(id);
    ASSERT_TRUE(brush != nullptr);
    EXPECT_EQ(brush->faces.size(), static_cast<size_t>(5)); // Wedge has 5 faces
    csg.Shutdown();
}

TEST(CSG_CreateCone)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    csg.SetCylinderSegments(8);
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Cone, {1.0f, 3.0f, 0});
    EXPECT_TRUE(id > 0);
    auto* brush = csg.GetBrush(id);
    ASSERT_TRUE(brush != nullptr);
    // 8 side triangles + 1 bottom cap = 9
    EXPECT_EQ(brush->faces.size(), static_cast<size_t>(9));
    csg.Shutdown();
}

// ============================================================================
// Mesh generation
// ============================================================================

TEST(CSG_GenerateMesh_Box)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    auto* solid = csg.GetBrush(id);
    ASSERT_TRUE(solid != nullptr);
    auto mesh = csg.GenerateMesh(*solid);
    EXPECT_EQ(mesh.triangleCount, static_cast<uint32_t>(12)); // 6 faces * 2 triangles each
    EXPECT_EQ(mesh.indices.size(), static_cast<size_t>(36));
    csg.Shutdown();
}

TEST(CSG_BuildAll_SingleBrush)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {2, 2, 2});
    auto mesh = csg.BuildAll();
    EXPECT_EQ(mesh.triangleCount, static_cast<uint32_t>(12));
    csg.Shutdown();
}

// ============================================================================
// Operations
// ============================================================================

TEST(CSG_RemoveBrush)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    csg.RemoveBrush(id);
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(0));
    csg.Shutdown();
}

TEST(CSG_ClearAll)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    csg.CreateBrush(Spark::LevelDesign::BrushShape::Cylinder, {1, 2, 0});
    csg.ClearAll();
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(0));
    csg.Shutdown();
}

TEST(CSG_SetBrushOperation)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    uint32_t id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    csg.SetBrushOperation(id, Spark::LevelDesign::CSGOperation::Subtractive);
    auto* brush = csg.GetBrush(id);
    EXPECT_TRUE(brush->operation == Spark::LevelDesign::CSGOperation::Subtractive);
    csg.Shutdown();
}

TEST(CSG_ConsoleGetStatus)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    auto status = csg.Console_GetStatus();
    EXPECT_TRUE(status.find("Brushes: 1") != std::string::npos);
    csg.Shutdown();
}
