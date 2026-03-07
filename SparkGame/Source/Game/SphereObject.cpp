#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// SphereObject.cpp
#include "SphereObject.h"
#include "Utils/Assert.h"
#include <iostream>

SphereObject::SphereObject(float radius, int slices, int stacks)
    : m_radius(radius)
    , m_slices(slices)
    , m_stacks(stacks)
{
    std::wcout << L"[INFO] SphereObject constructed. radius=" << radius << L" slices=" << slices << L" stacks=" << stacks << std::endl;
    ASSERT_MSG(radius > 0.0f, "Sphere radius must be positive");
    ASSERT_MSG(slices >= 3, "Sphere slices must be >= 3");
    ASSERT_MSG(stacks >= 2, "Sphere stacks must be >= 2");
    SetName("Sphere_" + std::to_string(GetID()));
}

HRESULT SphereObject::Initialize(ID3D11Device* d, ID3D11DeviceContext* c)
{
    std::wcout << L"[OPERATION] SphereObject::Initialize called. radius=" << m_radius << L" slices=" << m_slices << L" stacks=" << m_stacks << std::endl;
    ASSERT(d != nullptr);
    ASSERT(c != nullptr);
    return GameObject::Initialize(d, c);
}

void SphereObject::CreateMesh()
{
    std::wcout << L"[OPERATION] SphereObject::CreateMesh called. radius=" << m_radius << L" slices=" << m_slices << L" stacks=" << m_stacks << std::endl;
    if (!m_mesh) {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for SphereObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for SphereObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    ASSERT_MSG(SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty()) {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for SphereObject. loaded=" << loaded << std::endl;
    }
    if (!loaded) {
        hr = m_mesh->CreateSphere(m_radius, m_slices, m_stacks);
        if (FAILED(hr)) {
            hr = m_mesh->CreateCube(m_radius);
        }
        if (FAILED(hr)) {
            hr = m_mesh->CreatePlane(m_radius, m_radius);
        }
        std::wcout << L"[INFO] Procedural sphere mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        ASSERT_MSG(SUCCEEDED(hr), "Failed to create procedural sphere mesh");
    }
    ASSERT_MSG(m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
        "Sphere mesh must have vertices and indices after loading/creation");
}
#endif // SPARK_PLATFORM_WINDOWS
