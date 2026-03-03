// Bullet.h
#pragma once
#include "../Core/Platform.h"

#include "Projectile.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

using DirectX::XMMATRIX;
using DirectX::XMFLOAT3;

class Bullet : public Projectile
{
public:
    Bullet();
    ~Bullet() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void    Update(float deltaTime) override;
    void    Render(const XMMATRIX& view, const XMMATRIX& projection) override;
};