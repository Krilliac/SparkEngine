#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "PlaneObject.h"
#include "Utils/Assert.h"
#include <string>
#include <iostream>

PlaneObject::PlaneObject(float width, float depth)
    : m_width(width), m_depth(depth)
{
    std::wcout << L"[INFO] PlaneObject constructed. width=" << width << L" depth=" << depth << std::endl;
    ASSERT_MSG(width > 0.f && depth > 0.f, "Plane dimensions must be positive");
    SetName("Plane_" + std::to_string(GetID()));
}

HRESULT PlaneObject::Initialize(ID3D11Device* d, ID3D11DeviceContext* c)
{
    std::wcout << L"[OPERATION] PlaneObject::Initialize called. width=" << m_width << L" depth=" << m_depth << std::endl;
    ASSERT(d != nullptr);
    ASSERT(c != nullptr);
    return GameObject::Initialize(d, c);
}

void PlaneObject::CreateMesh()
{
    std::wcout << L"[OPERATION] PlaneObject::CreateMesh called. width=" << m_width << L" depth=" << m_depth << std::endl;
    if (!m_mesh) {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for PlaneObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for PlaneObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    ASSERT_MSG(SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty()) {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for PlaneObject. loaded=" << loaded << std::endl;
    }
    if (!loaded) {
        hr = m_mesh->CreatePlane(m_width, m_depth);
        if (FAILED(hr)) {
            hr = m_mesh->CreateCube(m_width);
        }
        if (FAILED(hr)) {
            hr = m_mesh->CreateTriangle(m_width);
        }
        std::wcout << L"[INFO] Procedural plane mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        ASSERT_MSG(SUCCEEDED(hr), "Failed to create procedural plane mesh");
    }
    ASSERT_MSG(m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
        "Plane mesh must have vertices and indices after loading/creation");
}
#endif // SPARK_PLATFORM_WINDOWS
