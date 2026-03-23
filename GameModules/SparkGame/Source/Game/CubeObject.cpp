#include "Core/Platform.h"
#include "CubeObject.h"
#include "Utils/Assert.h" // custom assert
#include "Utils/Validate.h"
#include "Game/PlaceholderMesh.h" // Add this include for LoadOrPlaceholderMesh
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <iostream>
#include <string>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

CubeObject::CubeObject(float size) : m_size(size)
{
    std::wcout << L"[INFO] CubeObject constructed. size=" << size << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, size > 0.0f, "Cube size must be positive");
    SetName("Cube_" + std::to_string(GetID()));
}

HRESULT CubeObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] CubeObject::Initialize called. size=" << m_size << std::endl;
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);
    return GameObject::Initialize(device, context);
}

void CubeObject::CreateMesh()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] CubeObject::CreateMesh called. size=" << m_size << std::endl;
    if (!m_mesh)
    {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for CubeObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for CubeObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty())
    {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for CubeObject. loaded=" << loaded << std::endl;
    }
    if (!loaded)
    {
        hr = m_mesh->CreateCube(m_size);
        if (FAILED(hr))
        {
            hr = m_mesh->CreateTriangle(m_size);
        }
        if (FAILED(hr))
        {
            hr = m_mesh->CreatePlane(m_size, m_size);
        }
        std::wcout << L"[INFO] Procedural cube mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Failed to create procedural cube mesh");
    }
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
                      "Cube mesh must have vertices and indices after loading/creation");
}
