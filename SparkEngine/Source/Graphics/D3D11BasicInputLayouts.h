#pragma once

#ifdef _WIN32
#include <guiddef.h>

namespace Spark::Graphics::Detail
{
    // The basic vertex shader owns the AssetPipeline layout through D3D11
    // private interface data. Shader reload/device recovery releases it with
    // the shader, without changing GraphicsEngine's instance layout.
    inline constexpr GUID kBasicAssetInputLayoutGuid = {
        0xa7d86a28, 0x9265, 0x4565, {0xa9, 0xf2, 0x3d, 0x18, 0x55, 0xe4, 0x71, 0xc0}};
} // namespace Spark::Graphics::Detail
#endif
