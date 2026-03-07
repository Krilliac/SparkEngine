#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "PyramidObject.h"
#include <string>
#include <iostream>

PyramidObject::PyramidObject(float size)
    : m_size(size)
{
    std::wcout << L"[INFO] PyramidObject constructed. size=" << size << std::endl;
    ASSERT_MSG(size > 0.f, "Pyramid size must be positive");
    SetName("Pyramid_" + std::to_string(GetID()));
}

HRESULT PyramidObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    std::wcout << L"[OPERATION] PyramidObject::Initialize called. size=" << m_size << std::endl;
    ASSERT(device != nullptr && context != nullptr);
    return GameObject::Initialize(device, context);
}

void PyramidObject::CreateMesh()
{
    std::wcout << L"[OPERATION] PyramidObject::CreateMesh called. size=" << m_size << std::endl;
    if (!m_mesh) {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for PyramidObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for PyramidObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    ASSERT_MSG(SUCCEEDED(hr), "Mesh initialization failed");
    
    bool loaded = false;
    if (!m_modelPath.empty()) {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for PyramidObject. loaded=" << loaded << std::endl;
    }
    
    if (!loaded) {
        // Try CreatePyramid first (proper implementation)
        hr = m_mesh->CreatePyramid(m_size, m_size); // Use size for both base and height
        if (FAILED(hr)) {
            // Fallback to cube if pyramid creation fails
            hr = m_mesh->CreateCube(m_size);
        }
        if (FAILED(hr)) {
            // Further fallback to triangle
            hr = m_mesh->CreateTriangle(m_size);
        }
        if (FAILED(hr)) {
            // Final fallback to plane
            hr = m_mesh->CreatePlane(m_size, m_size);
        }
        std::wcout << L"[INFO] Procedural pyramid mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        ASSERT_MSG(SUCCEEDED(hr), "Failed to create procedural pyramid mesh");
    }
    
    ASSERT_MSG(m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
        "Pyramid mesh must have vertices and indices after loading/creation");
}
#endif // SPARK_PLATFORM_WINDOWS
