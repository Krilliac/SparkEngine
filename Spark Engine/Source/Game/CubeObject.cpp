#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "CubeObject.h"
#include "Utils/Assert.h"          // custom assert
#include "PlaceholderMesh.h"       // Add this include for LoadOrPlaceholderMesh
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

CubeObject::CubeObject(float size)
    : m_size(size)
{
    std::wcout << L"[INFO] CubeObject constructed. size=" << size << std::endl;
    ASSERT_MSG(size > 0.0f, "Cube size must be positive");
    SetName("Cube_" + std::to_string(GetID()));
}

HRESULT CubeObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    std::wcout << L"[OPERATION] CubeObject::Initialize called. size=" << m_size << std::endl;
    ASSERT(device != nullptr);
    ASSERT(context != nullptr);
    return GameObject::Initialize(device, context);
}

void CubeObject::CreateMesh()
{
    std::wcout << L"[OPERATION] CubeObject::CreateMesh called. size=" << m_size << std::endl;
    if (!m_mesh) {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for CubeObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for CubeObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    ASSERT_MSG(SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty()) {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for CubeObject. loaded=" << loaded << std::endl;
    }
    if (!loaded) {
        hr = m_mesh->CreateCube(m_size);
        if (FAILED(hr)) {
            hr = m_mesh->CreateTriangle(m_size);
        }
        if (FAILED(hr)) {
            hr = m_mesh->CreatePlane(m_size, m_size);
        }
        std::wcout << L"[INFO] Procedural cube mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        ASSERT_MSG(SUCCEEDED(hr), "Failed to create procedural cube mesh");
    }
    ASSERT_MSG(m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
        "Cube mesh must have vertices and indices after loading/creation");
}
#endif // SPARK_PLATFORM_WINDOWS
