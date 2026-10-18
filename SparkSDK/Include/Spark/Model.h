/**
 * @file Model.h
 * @brief Legacy native OBJ model used by engine actors.
 */

#pragma once

#include <Spark/NativeTypes.h>

#include <string>

// Forward declaration
class GraphicsEngine;

//------------------------------------------------------------------------------
//  Model ‒ loads a simple OBJ file with tinyobjloader and creates GPU buffers
//------------------------------------------------------------------------------
class Model
{
  public:
    /**
     * @brief Default constructor
     */
    Model() = default;

    /**
     * @brief Destructor - cleans up DirectX resources
     */
    ~Model();

    // Load OBJ from wide-string path and build vertex / index buffers.
    //   - filename : full or relative path to .obj (UTF-16 wide string)
    //   - device   : valid ID3D11Device*
    HRESULT LoadObj(const std::wstring& filename, ID3D11Device* device);

    // ✅ ENHANCED: Render with shader setup using graphics engine
    void Render(ID3D11DeviceContext* ctx, GraphicsEngine* graphics = nullptr, const DirectX::XMMATRIX* world = nullptr,
                const DirectX::XMMATRIX* view = nullptr, const DirectX::XMMATRIX* proj = nullptr);

  private:
    ID3D11Buffer* m_vb{nullptr};
    ID3D11Buffer* m_ib{nullptr};
    UINT m_indexCount{0};
};
