/**
 * @file PlatformD3DStubs.h
 * @brief D3D11/DXGI type stubs and ComPtr for non-Windows platforms
 *
 * Provides opaque forward declarations of D3D11 types, a minimal ComPtr stub,
 * DXGI_FORMAT enum, and D3D11 enum stubs so that engine headers referencing
 * D3D11 types compile on Linux and macOS.
 */

#pragma once

#ifndef SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <cstddef>

// ============================================================================
// D3D11 Type Stubs (opaque forward declarations for non-Windows)
// ============================================================================

// These are forward-declared as opaque types so that headers referencing
// D3D11 types can compile on non-Windows platforms. The actual implementations
// are never instantiated on non-Windows.

struct ID3D11Device;
struct ID3D11Device1;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11UnorderedAccessView;
struct ID3D11Texture2D;
struct ID3D11Buffer;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;
struct ID3D11BlendState;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11ComputeShader;
struct ID3D11GeometryShader;
struct ID3D11HullShader;
struct ID3D11DomainShader;
struct ID3D11InputLayout;
struct ID3D11Query;
struct ID3DBlob;
struct IDXGIAdapter;
struct IDXGIFactory;
struct IDXGIFactory2;
struct IDXGIOutput;
struct IDXGIDebug;
struct ID3D11Resource;
struct ID3D11DeviceChild;

// FILETIME stub
struct FILETIME
{
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
};

// Minimal ComPtr stub for non-Windows
namespace Microsoft
{
    namespace WRL
    {
        template <typename T> class ComPtr
        {
            T* ptr = nullptr;

          public:
            ComPtr() = default;
            ~ComPtr() { /* No Release() on stubs */ }
            ComPtr(const ComPtr&) = default;
            ComPtr& operator=(const ComPtr&) = default;
            ComPtr(ComPtr&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
            ComPtr& operator=(ComPtr&& o) noexcept
            {
                ptr = o.ptr;
                o.ptr = nullptr;
                return *this;
            }
            T* Get() const { return ptr; }
            T** GetAddressOf() { return &ptr; }
            T** ReleaseAndGetAddressOf()
            {
                ptr = nullptr;
                return &ptr;
            }
            T* operator->() const { return ptr; }
            T& operator*() const { return *ptr; }
            operator bool() const { return ptr != nullptr; }
            bool operator==(std::nullptr_t) const { return ptr == nullptr; }
            bool operator!=(std::nullptr_t) const { return ptr != nullptr; }
            void Reset() { ptr = nullptr; }
        };
    } // namespace WRL
} // namespace Microsoft

// DXGI_FORMAT stub
#ifndef DXGI_FORMAT_UNKNOWN
enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
};
#endif

// D3D11 enum stubs
#ifndef D3D11_BIND_VERTEX_BUFFER
enum D3D11_BIND_FLAG
{
    D3D11_BIND_VERTEX_BUFFER = 0x1,
    D3D11_BIND_INDEX_BUFFER = 0x2,
    D3D11_BIND_CONSTANT_BUFFER = 0x4,
    D3D11_BIND_SHADER_RESOURCE = 0x8,
    D3D11_BIND_RENDER_TARGET = 0x20,
    D3D11_BIND_DEPTH_STENCIL = 0x40,
};
#endif

#ifndef D3D11_FILTER_MIN_MAG_MIP_LINEAR
enum D3D11_FILTER
{
    D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
    D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D11_FILTER_ANISOTROPIC = 0x55,
};
enum D3D11_TEXTURE_ADDRESS_MODE
{
    D3D11_TEXTURE_ADDRESS_WRAP = 1,
    D3D11_TEXTURE_ADDRESS_MIRROR = 2,
    D3D11_TEXTURE_ADDRESS_CLAMP = 3,
    D3D11_TEXTURE_ADDRESS_BORDER = 4,
    D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5,
};
constexpr float D3D11_FLOAT32_MAX = 3.402823466e+38F;
#endif

// XMUINT4 type
namespace DirectX
{
    struct XMUINT4
    {
        uint32_t x, y, z, w;
        XMUINT4() : x(0), y(0), z(0), w(0) {}
        XMUINT4(uint32_t _x, uint32_t _y, uint32_t _z, uint32_t _w) : x(_x), y(_y), z(_z), w(_w) {}
    };
} // namespace DirectX

#endif // !SPARK_PLATFORM_WINDOWS
