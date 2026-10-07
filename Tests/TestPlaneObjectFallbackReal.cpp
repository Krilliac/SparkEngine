/**
 * @file TestPlaneObjectFallbackReal.cpp
 * @brief Read back the production missing-asset ground mesh and preserve sprite winding.
 */
#include "TestFramework.h"

#ifdef _WIN32

#include "Game/PlaneObject.h"
#include "Graphics/GraphicsEngine.h"
#include "RHI210D3D11GoldenSupport.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
    class EmptyPlaneProject
    {
      public:
        EmptyPlaneProject() : m_previous(std::filesystem::current_path())
        {
            m_root = std::filesystem::temp_directory_path() /
                     ("spark-plane-fallback-" + std::to_string(GetCurrentProcessId()) + "-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            if (!std::filesystem::create_directory(m_root))
                throw std::runtime_error("Plane fallback test requires a fresh directory");
            std::filesystem::current_path(m_root);
        }

        ~EmptyPlaneProject()
        {
            std::error_code error;
            std::filesystem::current_path(m_previous, error);
            if (!error)
                std::filesystem::remove_all(m_root, error);
        }

        EmptyPlaneProject(const EmptyPlaneProject&) = delete;
        EmptyPlaneProject& operator=(const EmptyPlaneProject&) = delete;

      private:
        std::filesystem::path m_previous;
        std::filesystem::path m_root;
    };

    struct WarpMeshDevice
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> messages;

        HRESULT Initialize()
        {
            const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
            const HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                                     &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
            if (FAILED(result))
                return result;
            return device.As(&messages);
        }
    };

    void CheckUploadedPlane(const RHI210Golden::TriangleMesh& mesh, float width, float depth, float direction)
    {
        ASSERT_EQ(mesh.TriangleCount(), 2u);
        ASSERT_EQ(mesh.positions.size(), 6u);
        ASSERT_EQ(mesh.normals.size(), 6u);
        ASSERT_EQ(mesh.uvs.size(), 6u);
        EXPECT_NEAR(mesh.boundsMin.x, -width * 0.5f, 1.0e-6f);
        EXPECT_NEAR(mesh.boundsMax.x, width * 0.5f, 1.0e-6f);
        EXPECT_NEAR(mesh.boundsMin.z, -depth * 0.5f, 1.0e-6f);
        EXPECT_NEAR(mesh.boundsMax.z, depth * 0.5f, 1.0e-6f);
        for (size_t i = 0; i < mesh.positions.size(); ++i)
        {
            EXPECT_NEAR(mesh.positions[i].y, 0.0f, 1.0e-6f);
            EXPECT_NEAR(mesh.normals[i].x, 0.0f, 1.0e-6f);
            EXPECT_NEAR(mesh.normals[i].y, direction, 1.0e-6f);
            EXPECT_NEAR(mesh.normals[i].z, 0.0f, 1.0e-6f);
            EXPECT_NEAR(mesh.uvs[i].x, mesh.positions[i].x / width + 0.5f, 1.0e-6f);
            EXPECT_NEAR(mesh.uvs[i].y, 0.5f - mesh.positions[i].z / depth, 1.0e-6f);
        }
        for (size_t triangle = 0; triangle < mesh.TriangleCount(); ++triangle)
        {
            const auto& a = mesh.positions[triangle * 3];
            const auto& b = mesh.positions[triangle * 3 + 1];
            const auto& c = mesh.positions[triangle * 3 + 2];
            const float crossY = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            EXPECT_NEAR(crossY, direction * width * depth, 1.0e-5f);
        }
    }

    void CheckDebugMessages(ID3D11InfoQueue* messages)
    {
        GraphicsEngine::ValidationCounts counts;
        GraphicsEngine::AccumulateValidationMessages(messages, counts);
        EXPECT_EQ(counts.corruption, 0u);
        EXPECT_EQ(counts.errors, 0u);
        EXPECT_EQ(counts.warnings, 0u);
        EXPECT_EQ(messages->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
    }
} // namespace

TEST(D3D11PlaneFallback_MissingAssetFacesUpWithOriginalDimensionsAndUVs)
{
    WarpMeshDevice warp;
    ASSERT_TRUE(SUCCEEDED(warp.Initialize()));
    EmptyPlaneProject project;
    ASSERT_FALSE(std::filesystem::exists("Assets/Models/Plane.obj"));
    PlaneObject plane(7.5f, 3.25f);
    ASSERT_TRUE(SUCCEEDED(plane.Initialize(warp.device.Get(), warp.context.Get())));
    ASSERT_TRUE(plane.GetMesh() != nullptr);
    const auto mesh = RHI210Golden::ReadMeshBuffers(*plane.GetMesh(), warp.device.Get(), warp.context.Get());
    ASSERT_TRUE(mesh.has_value());
    CheckUploadedPlane(*mesh, 7.5f, 3.25f, 1.0f);
    plane.Shutdown();
    CheckDebugMessages(warp.messages.Get());
}

TEST(D3D11PlaneFallback_GenericSpriteMeshRetainsItsWinding)
{
    WarpMeshDevice warp;
    ASSERT_TRUE(SUCCEEDED(warp.Initialize()));
    Mesh sprite;
    ASSERT_TRUE(SUCCEEDED(sprite.Initialize(warp.device.Get(), warp.context.Get())));
    ASSERT_TRUE(SUCCEEDED(sprite.CreatePlane(2.0f, 5.0f)));
    const auto mesh = RHI210Golden::ReadMeshBuffers(sprite, warp.device.Get(), warp.context.Get());
    ASSERT_TRUE(mesh.has_value());
    CheckUploadedPlane(*mesh, 2.0f, 5.0f, -1.0f);
    sprite.Shutdown();
    CheckDebugMessages(warp.messages.Get());
}

#endif
