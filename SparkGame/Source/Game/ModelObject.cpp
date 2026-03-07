#include "Core/Platform.h"
/**
 * @file ModelObject.cpp
 * @brief Implementation of ModelObject class
 * @author Spark Engine Team
 * @date 2025
 */

#include "ModelObject.h"
#include "Utils/Assert.h"
#include <iostream>

ModelObject::ModelObject(const std::wstring& modelPath)
    : m_modelPath(modelPath)
    , m_model(std::make_unique<Model>())
{
    SetName("ModelObject");
}

HRESULT ModelObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT(device != nullptr);
    ASSERT(context != nullptr);

    // Load the model
    HRESULT hr = m_model->LoadObj(m_modelPath, device);
    if (FAILED(hr)) {
        // Convert wstring to string for logging
        std::string modelPathStr(m_modelPath.begin(), m_modelPath.end());
        std::wcout << L"Warning: Failed to load model: " << m_modelPath << std::endl;
        return hr;
    }

    // Call base class initialization
    return GameObject::Initialize(device, context);
}

void ModelObject::Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
{
    if (!IsVisible() || !m_model) {
        return;
    }

    ASSERT(m_context != nullptr);

    // Build full world matrix with scale, rotation, and translation
    DirectX::XMFLOAT3 pos = GetPosition();
    DirectX::XMFLOAT3 rot = GetRotation();
    DirectX::XMFLOAT3 scl = GetScale();
    DirectX::XMMATRIX world = DirectX::XMMatrixScaling(scl.x, scl.y, scl.z) *
                               DirectX::XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z) *
                               DirectX::XMMatrixTranslation(pos.x, pos.y, pos.z);
    
    // ? ENHANCED: Get graphics engine reference (you may need to adjust this based on how you access it)
    // For now, we'll try to get it from a global or pass it down from the render system
    extern std::unique_ptr<GraphicsEngine> g_graphics;
    GraphicsEngine* graphics = g_graphics.get();

    // **ENHANCED RENDERING: Set up shaders, constant buffers, and matrices**
    try {
        m_model->Render(m_context, graphics, &world, &view, &proj);
    } catch (...) {
        // Handle rendering errors gracefully
        static int errorCount = 0;
        if (++errorCount <= 3) {
            std::wcout << L"Warning: Model rendering error for " << m_modelPath << std::endl;
        }
    }
}

void ModelObject::Update(float deltaTime)
{
    // Base update
    GameObject::Update(deltaTime);
    
    // Add any model-specific update logic here if needed
}

// Implement pure virtual methods from GameObject
void ModelObject::OnHit(GameObject* target)
{
    // Handle collision with another game object
    // For now, just do nothing - override in derived classes for specific behavior
    ASSERT(target != nullptr);
}

void ModelObject::OnHitWorld(const DirectX::XMFLOAT3& hitPoint, const DirectX::XMFLOAT3& normal)
{
    // Handle collision with world geometry
    // For now, just do nothing - override in derived classes for specific behavior
}
