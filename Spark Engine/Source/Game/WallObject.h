#pragma once
#include "../Core/Platform.h"

#include "Game/GameObject.h"
#include "Game/PlaceholderMesh.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

class WallObject : public GameObject
{
public:
    WallObject(float width = 5.0f, float height = 3.0f);
    ~WallObject() override = default;

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
    float        m_width;
    float        m_height;
    std::wstring m_modelPath{ L"Assets\\Models\\Wall.obj" };
};