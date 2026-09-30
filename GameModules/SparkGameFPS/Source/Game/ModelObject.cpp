#include "ModelObject.h"
#include "Core/FPSLog.h"
#include "Core/Platform.h"
/**
 * @file ModelObject.cpp
 * @brief Implementation of ModelObject class
 * @author Spark Engine Team
 * @date 2025
 */
#include "Core/FPSAssert.h"

#include <iostream>

ModelObject::ModelObject(const std::wstring& modelPath) : m_modelPath(modelPath), m_model(std::make_unique<Model>())
{
    FPS_LOG_INFO("ModelObject constructed");
    SetName("ModelObject");
}

HRESULT ModelObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    FPS_REQUIRE_NOT_NULL(device);
    FPS_REQUIRE_NOT_NULL(context);

    // Load the model
    HRESULT hr = m_model->LoadObj(m_modelPath, device);
    if (FAILED(hr))
    {
        // Convert wstring to string for logging
        std::string modelPathStr(m_modelPath.begin(), m_modelPath.end());
        FPS_LOG_ERROR("Failed to load model");
        std::wcout << L"Warning: Failed to load model: " << m_modelPath << std::endl;
        return hr;
    }

    // Call base class initialization
    return GameObject::Initialize(device, context);
}

void ModelObject::Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
{
    if (!IsVisible() || !m_model)
    {
        return;
    }

    if (m_context == nullptr)
    {
        FPS_LOG_ERROR("{}: 'm_context' must not be null", __func__);
        return;
    }

    // Build full world matrix with scale, rotation, and translation
    DirectX::XMFLOAT3 pos = GetPosition();
    DirectX::XMFLOAT3 rot = GetRotation();
    DirectX::XMFLOAT3 scl = GetScale();
    DirectX::XMMATRIX world = DirectX::XMMatrixScaling(scl.x, scl.y, scl.z) *
                              DirectX::XMMatrixRotationRollPitchYaw(rot.x, rot.y, rot.z) *
                              DirectX::XMMatrixTranslation(pos.x, pos.y, pos.z);

    GraphicsEngine* graphics = m_graphics;

    // Model::Render owns the legacy geometry draw, while GameObject owns the
    // confined JSON material contract shared by authored and procedural paths.
    // Prepare the state first and pass nullptr so Model does not reset it.
    try
    {
        PrepareBasicMaterialRender(graphics, world, view, proj);
        m_model->Render(m_context, nullptr, &world, &view, &proj);
    }
    catch (...)
    {
        // Handle rendering errors gracefully
        static int errorCount = 0;
        if (++errorCount <= 3)
        {
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
    if (target == nullptr)
    {
        FPS_LOG_ERROR("{}: 'target' must not be null", __func__);
        return;
    }
}

void ModelObject::OnHitWorld(const DirectX::XMFLOAT3& hitPoint, const DirectX::XMFLOAT3& normal)
{
    // Handle collision with world geometry
    // For now, just do nothing - override in derived classes for specific behavior
}
