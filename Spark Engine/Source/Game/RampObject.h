#pragma once
#include "../Core/Platform.h"

#include "Game/GameObject.h"
#include "Game/PlaceholderMesh.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

class RampObject : public GameObject
{
public:
    RampObject(float length = 2.0f, float height = 1.0f);
    ~RampObject() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void    Update(float dt) override { GameObject::Update(dt); }
    void    Render(const DirectX::XMMATRIX& v, const DirectX::XMMATRIX& p) override
    {
        GameObject::Render(v, p);
    }
    void    OnHit(GameObject*) override {}
    void    OnHitWorld(const DirectX::XMFLOAT3&, const DirectX::XMFLOAT3&) override {}

protected:
    void CreateMesh() override;

private:
    float        m_length;
    float        m_height;
    std::wstring m_modelPath{ L"Assets\\Models\\Ramp.obj" };
};