/**
 * @file InstanceRenderer.h
 * @brief GPU instancing and draw-call batching system for DirectX 11
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides automatic batching of draw calls by mesh+material key, GPU instance
 * buffers, indirect draw support, and per-batch frustum culling. Each frame,
 * callers Submit() per-instance data; Flush() uploads buffers and issues
 * DrawIndexedInstanced (or DrawIndexedInstancedIndirect) calls in batch order.
 *
 * ## Usage
 * @code
 *   InstanceRenderer renderer;
 *   renderer.Initialize(device.Get(), context.Get());
 *
 *   // Per frame:
 *   renderer.BeginFrame();
 *   for (auto& obj : visibleObjects)
 *       renderer.Submit(obj.meshID, obj.materialID, obj.instanceData);
 *   renderer.Flush(frustum);
 *   renderer.EndFrame();
 *
 *   // Shutdown:
 *   renderer.Shutdown();
 * @endcode
 *
 * ## Integration
 * - FrustumCulling.h: per-batch AABB visibility test before draw
 * - SimpleConsole: `inst_stats`, `inst_enabled` commands
 * - EngineContext: accessed via service locator, not globals
 *
 * @see FrustumCulling, GraphicsEngine, Mesh, MaterialSystem
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#endif // SPARK_PLATFORM_WINDOWS

#include "FrustumCulling.h"

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <cstring>
#include <functional>

using Microsoft::WRL::ComPtr;

#ifdef SPARK_PLATFORM_WINDOWS

namespace Spark::Graphics
{

    // ========================================================================
    // Constants
    // ========================================================================

    /** @brief Default per-batch instance capacity before first grow. */
    static constexpr uint32_t INSTANCE_BATCH_DEFAULT_CAPACITY = 256;

    /** @brief Growth factor when a batch exceeds its current capacity. */
    static constexpr float INSTANCE_BATCH_GROW_FACTOR = 2.0f;

    /** @brief Maximum instances per single batch (safety limit). */
    static constexpr uint32_t INSTANCE_BATCH_MAX_CAPACITY = 1 << 20; // ~1M

    // ========================================================================
    // InstanceData
    // ========================================================================

    /**
     * @brief Per-instance data uploaded to the GPU structured buffer.
     *
     * Layout is designed for 16-byte alignment to satisfy HLSL packing rules.
     * PrevWorld enables per-pixel motion vectors for temporal anti-aliasing.
     */
    struct InstanceData
    {
        DirectX::XMFLOAT4X4 World;         ///< Current frame world transform (64 bytes)
        DirectX::XMFLOAT4X4 PrevWorld;     ///< Previous frame world transform for motion vectors (64 bytes)
        DirectX::XMFLOAT4   ColorTint;     ///< RGBA color multiplier (16 bytes)
        uint32_t             MaterialID;    ///< Index into material array (4 bytes)
        uint32_t             LODLevel;      ///< Discrete LOD level (4 bytes)
        float                Padding0;      ///< Pad to 16-byte boundary (4 bytes)
        float                Padding1;      ///< Pad to 16-byte boundary (4 bytes)
        DirectX::XMFLOAT4   CustomData;    ///< Arbitrary per-instance float4 for shaders (16 bytes)
        // Total: 176 bytes, 16-byte aligned

        InstanceData()
            : World{}
            , PrevWorld{}
            , ColorTint{1.0f, 1.0f, 1.0f, 1.0f}
            , MaterialID{0}
            , LODLevel{0}
            , Padding0{0.0f}
            , Padding1{0.0f}
            , CustomData{0.0f, 0.0f, 0.0f, 0.0f}
        {
            DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
            DirectX::XMStoreFloat4x4(&World, identity);
            DirectX::XMStoreFloat4x4(&PrevWorld, identity);
        }
    };

    // ========================================================================
    // BatchKey
    // ========================================================================

    /**
     * @brief Composite key for grouping instances that share a mesh and material.
     *
     * Instances with identical BatchKey are rendered in a single
     * DrawIndexedInstanced call.
     */
    struct BatchKey
    {
        uint64_t MeshID;
        uint64_t MaterialID;

        bool operator==(const BatchKey& other) const
        {
            return MeshID == other.MeshID && MaterialID == other.MaterialID;
        }
    };

    /** @brief Hash functor for BatchKey, used by std::unordered_map. */
    struct BatchKeyHash
    {
        std::size_t operator()(const BatchKey& key) const
        {
            // FNV-1a inspired combine
            std::size_t h = std::hash<uint64_t>{}(key.MeshID);
            h ^= std::hash<uint64_t>{}(key.MaterialID) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // ========================================================================
    // InstanceBatch
    // ========================================================================

    /**
     * @brief A single draw-call batch: all instances sharing one mesh + material.
     *
     * Owns a D3D11 dynamic structured buffer that is grown as needed and
     * uploaded once per frame via Map/Unmap with DISCARD.
     */
    class InstanceBatch
    {
    public:
        InstanceBatch() = default;
        ~InstanceBatch() = default;

        // Non-copyable, movable
        InstanceBatch(const InstanceBatch&) = delete;
        InstanceBatch& operator=(const InstanceBatch&) = delete;
        InstanceBatch(InstanceBatch&&) noexcept = default;
        InstanceBatch& operator=(InstanceBatch&&) noexcept = default;

        /**
         * @brief Create the GPU buffer with a given initial capacity.
         * @param pDevice  D3D11 device pointer.
         * @param capacity Initial instance count the buffer can hold.
         * @return S_OK on success, error HRESULT otherwise.
         */
        HRESULT Initialize(
            _In_ ID3D11Device* pDevice,
            uint32_t capacity = INSTANCE_BATCH_DEFAULT_CAPACITY)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            m_capacity = capacity;
            m_instanceCount = 0;
            m_dirty = false;
            m_instances.reserve(capacity);
            return CreateBuffer(pDevice, capacity);
#else
            (void)pDevice;
            (void)capacity;
            return S_OK;
#endif
        }

        /**
         * @brief Add an instance to this batch (CPU side).
         * @param data  Per-instance data to append.
         */
        void AddInstance(const InstanceData& data)
        {
            m_instances.push_back(data);
            m_instanceCount = static_cast<uint32_t>(m_instances.size());
            m_dirty = true;
        }

        /**
         * @brief Upload dirty CPU data to the GPU buffer, growing if needed.
         * @param pDevice   D3D11 device (needed if buffer must grow).
         * @param pContext  D3D11 device context for Map/Unmap.
         * @return S_OK on success.
         */
        HRESULT UploadToGPU(
            _In_ ID3D11Device* pDevice,
            _In_ ID3D11DeviceContext* pContext)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_dirty || m_instanceCount == 0)
            {
                return S_OK;
            }

            // Grow buffer if current capacity is insufficient
            if (m_instanceCount > m_capacity)
            {
                uint32_t newCapacity = m_capacity;
                while (newCapacity < m_instanceCount && newCapacity < INSTANCE_BATCH_MAX_CAPACITY)
                {
                    newCapacity = static_cast<uint32_t>(
                        static_cast<float>(newCapacity) * INSTANCE_BATCH_GROW_FACTOR);
                }
                newCapacity = (newCapacity < m_instanceCount) ? m_instanceCount : newCapacity;

                HRESULT hr = CreateBuffer(pDevice, newCapacity);
                if (FAILED(hr))
                {
                    return hr;
                }
                m_capacity = newCapacity;
            }

            // Map and copy
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = pContext->Map(
                m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr))
            {
                return hr;
            }

            memcpy(mapped.pData, m_instances.data(),
                   m_instanceCount * sizeof(InstanceData));

            pContext->Unmap(m_instanceBuffer.Get(), 0);
            m_dirty = false;
            return S_OK;
#else
            (void)pDevice;
            (void)pContext;
            return S_OK;
#endif
        }

        /** @brief Clear CPU-side instances for the next frame. */
        void Reset()
        {
            m_instances.clear();
            m_instanceCount = 0;
            m_dirty = false;
        }

        // -- Accessors -------------------------------------------------------

        uint32_t GetInstanceCount() const { return m_instanceCount; }
        uint32_t GetCapacity() const { return m_capacity; }
        bool IsDirty() const { return m_dirty; }

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11Buffer* GetBuffer() const { return m_instanceBuffer.Get(); }
        ID3D11ShaderResourceView* GetSRV() const { return m_instanceSRV.Get(); }
#endif

        /** @brief Optional per-batch AABB for frustum culling. */
        void SetBounds(const AABB& bounds) { m_bounds = bounds; m_hasBounds = true; }
        bool HasBounds() const { return m_hasBounds; }
        const AABB& GetBounds() const { return m_bounds; }

    private:
#ifdef SPARK_PLATFORM_WINDOWS
        /**
         * @brief (Re)create the structured buffer and its SRV.
         */
        HRESULT CreateBuffer(ID3D11Device* pDevice, uint32_t capacity)
        {
            m_instanceBuffer.Reset();
            m_instanceSRV.Reset();

            D3D11_BUFFER_DESC bufDesc{};
            bufDesc.ByteWidth = capacity * sizeof(InstanceData);
            bufDesc.Usage = D3D11_USAGE_DYNAMIC;
            bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = sizeof(InstanceData);

            HRESULT hr = pDevice->CreateBuffer(
                &bufDesc, nullptr, m_instanceBuffer.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = capacity;

            hr = pDevice->CreateShaderResourceView(
                m_instanceBuffer.Get(), &srvDesc,
                m_instanceSRV.ReleaseAndGetAddressOf());
            return hr;
        }
#endif

        std::vector<InstanceData> m_instances;
        uint32_t m_instanceCount = 0;
        uint32_t m_capacity = 0;
        bool m_dirty = false;
        bool m_hasBounds = false;
        AABB m_bounds{};

#ifdef SPARK_PLATFORM_WINDOWS
        ComPtr<ID3D11Buffer>             m_instanceBuffer;
        ComPtr<ID3D11ShaderResourceView> m_instanceSRV;
#endif
    };

    // ========================================================================
    // IndirectDrawBuffer
    // ========================================================================

    /**
     * @brief Manages a D3D11 argument buffer for DrawIndexedInstancedIndirect.
     *
     * Supports compute-shader-driven GPU culling: a compute shader writes
     * visible instance counts into this buffer, and the draw call reads
     * arguments without CPU readback.
     */
    class IndirectDrawBuffer
    {
    public:
        IndirectDrawBuffer() = default;
        ~IndirectDrawBuffer() = default;

        /**
         * @brief Argument layout matching D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS.
         */
        struct DrawArgs
        {
            uint32_t IndexCountPerInstance;
            uint32_t InstanceCount;
            uint32_t StartIndexLocation;
            int32_t  BaseVertexLocation;
            uint32_t StartInstanceLocation;
        };

        /**
         * @brief Create the indirect argument buffer.
         * @param pDevice    D3D11 device.
         * @param maxBatches Maximum number of indirect draw arg structs.
         * @return S_OK on success.
         */
        HRESULT Initialize(
            _In_ ID3D11Device* pDevice,
            uint32_t maxBatches = 256)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            m_maxBatches = maxBatches;

            // Argument buffer: writable by compute shader, readable by DrawIndirect
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = maxBatches * sizeof(DrawArgs);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;

            HRESULT hr = pDevice->CreateBuffer(
                &desc, nullptr, m_argBuffer.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            // UAV so compute shaders can write draw args
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R32_UINT;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = maxBatches * (sizeof(DrawArgs) / sizeof(uint32_t));

            hr = pDevice->CreateShaderResourceView(
                m_argBuffer.Get(), nullptr, m_argSRV.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            hr = pDevice->CreateUnorderedAccessView(
                m_argBuffer.Get(), &uavDesc, m_argUAV.ReleaseAndGetAddressOf());
            return hr;
#else
            (void)pDevice;
            (void)maxBatches;
            return S_OK;
#endif
        }

        /**
         * @brief Upload CPU-authored draw args (fallback when no compute culling).
         */
        HRESULT UploadArgs(
            _In_ ID3D11DeviceContext* pContext,
            const std::vector<DrawArgs>& args)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (args.empty() || !m_argBuffer)
            {
                return S_OK;
            }
            uint32_t count = static_cast<uint32_t>(args.size());
            if (count > m_maxBatches)
            {
                count = m_maxBatches;
            }

            D3D11_BOX box{};
            box.left = 0;
            box.right = count * sizeof(DrawArgs);
            box.top = 0;
            box.bottom = 1;
            box.front = 0;
            box.back = 1;

            pContext->UpdateSubresource(
                m_argBuffer.Get(), 0, &box, args.data(), 0, 0);
            return S_OK;
#else
            (void)pContext;
            (void)args;
            return S_OK;
#endif
        }

        void Shutdown()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            m_argBuffer.Reset();
            m_argSRV.Reset();
            m_argUAV.Reset();
#endif
        }

        // -- Accessors -------------------------------------------------------

        uint32_t GetMaxBatches() const { return m_maxBatches; }

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11Buffer*              GetArgBuffer() const { return m_argBuffer.Get(); }
        ID3D11ShaderResourceView*  GetSRV() const { return m_argSRV.Get(); }
        ID3D11UnorderedAccessView* GetUAV() const { return m_argUAV.Get(); }
#endif

    private:
        uint32_t m_maxBatches = 0;

#ifdef SPARK_PLATFORM_WINDOWS
        ComPtr<ID3D11Buffer>              m_argBuffer;
        ComPtr<ID3D11ShaderResourceView>  m_argSRV;
        ComPtr<ID3D11UnorderedAccessView> m_argUAV;
#endif
    };

    // ========================================================================
    // RenderStatistics
    // ========================================================================

    /**
     * @brief Per-frame rendering statistics reported by InstanceRenderer.
     */
    struct RenderStatistics
    {
        uint32_t TotalInstances = 0;      ///< Instances submitted this frame
        uint32_t VisibleInstances = 0;    ///< Instances in visible batches
        uint32_t BatchCount = 0;          ///< Unique mesh+material batches
        uint32_t DrawCallsIssued = 0;     ///< Actual GPU draw calls
        uint32_t DrawCallsSaved = 0;      ///< Instances minus draw calls
        uint32_t BufferUploads = 0;       ///< Number of Map/Unmap operations
        uint32_t BufferGrows = 0;         ///< Times a batch buffer was re-allocated
        uint32_t FrustumCulledBatches = 0;///< Batches skipped by frustum culling
    };

    // ========================================================================
    // Embedded HLSL Shaders
    // ========================================================================

    /**
     * @brief Instancing vertex shader that reads per-instance data from a
     *        StructuredBuffer bound to SRV slot t0.
     *
     * SV_InstanceID indexes into the StructuredBuffer to fetch the world
     * matrix and per-instance attributes. The previous-frame world matrix
     * is used to compute motion vectors in a subsequent pixel shader pass.
     */
    static constexpr const char* INSTANCING_VS_HLSL = R"(
// ============================================================================
// SparkEngine Instancing Vertex Shader
// ============================================================================

struct InstanceData
{
    float4x4 World;
    float4x4 PrevWorld;
    float4   ColorTint;
    uint     MaterialID;
    uint     LODLevel;
    float    Padding0;
    float    Padding1;
    float4   CustomData;
};

StructuredBuffer<InstanceData> g_InstanceBuffer : register(t0);

cbuffer PerFrameConstants : register(b0)
{
    float4x4 g_View;
    float4x4 g_Projection;
    float4x4 g_PrevViewProjection;
    float3   g_CameraPosition;
    float    g_Time;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 PositionCS     : SV_POSITION;
    float3 WorldPosition  : WORLDPOS;
    float3 WorldNormal    : NORMAL;
    float2 TexCoord       : TEXCOORD0;
    float4 ColorTint      : COLOR0;
    float4 PrevPositionCS : TEXCOORD1;   // For motion vectors
    float4 CustomData     : TEXCOORD2;
    uint   MaterialID     : TEXCOORD3;
    uint   InstanceID     : TEXCOORD4;
};

VSOutput main(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output = (VSOutput)0;

    InstanceData inst = g_InstanceBuffer[instanceID];

    // Transform to world space
    float4 worldPos = mul(float4(input.Position, 1.0), inst.World);
    output.WorldPosition = worldPos.xyz;

    // Transform to clip space
    float4 viewPos = mul(worldPos, g_View);
    output.PositionCS = mul(viewPos, g_Projection);

    // World-space normal (assumes uniform scale; for non-uniform, use inverse-transpose)
    output.WorldNormal = normalize(mul(input.Normal, (float3x3)inst.World));

    // Previous frame clip-space position for motion vectors
    float4 prevWorldPos = mul(float4(input.Position, 1.0), inst.PrevWorld);
    output.PrevPositionCS = mul(prevWorldPos, g_PrevViewProjection);

    // Pass-through per-instance data
    output.TexCoord   = input.TexCoord;
    output.ColorTint  = inst.ColorTint;
    output.CustomData = inst.CustomData;
    output.MaterialID = inst.MaterialID;
    output.InstanceID = instanceID;

    return output;
}
)";

    /**
     * @brief Minimal instancing pixel shader for testing / fallback.
     *
     * Outputs the instance color tint modulated with a simple N-dot-L
     * directional light, plus motion vectors in a second render target.
     */
    static constexpr const char* INSTANCING_PS_HLSL = R"(
// ============================================================================
// SparkEngine Instancing Pixel Shader (fallback)
// ============================================================================

struct PSInput
{
    float4 PositionCS     : SV_POSITION;
    float3 WorldPosition  : WORLDPOS;
    float3 WorldNormal    : NORMAL;
    float2 TexCoord       : TEXCOORD0;
    float4 ColorTint      : COLOR0;
    float4 PrevPositionCS : TEXCOORD1;
    float4 CustomData     : TEXCOORD2;
    uint   MaterialID     : TEXCOORD3;
    uint   InstanceID     : TEXCOORD4;
};

struct PSOutput
{
    float4 Color        : SV_Target0;
    float2 MotionVector : SV_Target1;
};

static const float3 g_LightDir = normalize(float3(0.5, -1.0, 0.3));

PSOutput main(PSInput input)
{
    PSOutput output = (PSOutput)0;

    // Simple N-dot-L lighting
    float3 N = normalize(input.WorldNormal);
    float NdotL = saturate(dot(N, -g_LightDir));
    float3 diffuse = input.ColorTint.rgb * (0.15 + 0.85 * NdotL);

    output.Color = float4(diffuse, input.ColorTint.a);

    // Motion vector: current vs previous screen position
    float2 curSS  = input.PositionCS.xy / input.PositionCS.w;
    float2 prevSS = input.PrevPositionCS.xy / input.PrevPositionCS.w;
    output.MotionVector = (curSS - prevSS) * 0.5;

    return output;
}
)";

    /**
     * @brief GPU culling compute shader stub.
     *
     * Reads instance bounds, tests against frustum planes, and writes
     * surviving draw args into the indirect argument buffer.
     */
    static constexpr const char* GPU_CULL_CS_HLSL = R"(
// ============================================================================
// SparkEngine GPU Frustum Culling Compute Shader
// ============================================================================

struct InstanceData
{
    float4x4 World;
    float4x4 PrevWorld;
    float4   ColorTint;
    uint     MaterialID;
    uint     LODLevel;
    float    Padding0;
    float    Padding1;
    float4   CustomData;
};

struct DrawArgs
{
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    int  BaseVertexLocation;
    uint StartInstanceLocation;
};

cbuffer CullConstants : register(b0)
{
    float4 g_FrustumPlanes[6];
    uint   g_TotalInstances;
    uint   g_IndexCountPerInstance;
    uint   g_Pad0;
    uint   g_Pad1;
};

StructuredBuffer<InstanceData> g_Instances : register(t0);
RWStructuredBuffer<DrawArgs>   g_DrawArgs  : register(u0);
RWStructuredBuffer<uint>       g_VisibleIndices : register(u1);

groupshared uint gs_VisibleCount;

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint gidx : SV_GroupIndex)
{
    if (gidx == 0)
    {
        gs_VisibleCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < g_TotalInstances)
    {
        // Extract instance world-space center from the translation row
        float3 center = float3(
            g_Instances[dtid.x].World[3][0],
            g_Instances[dtid.x].World[3][1],
            g_Instances[dtid.x].World[3][2]);

        // Conservative sphere test (radius from scale)
        float radius = length(float3(
            length(float3(g_Instances[dtid.x].World[0][0],
                          g_Instances[dtid.x].World[0][1],
                          g_Instances[dtid.x].World[0][2])),
            length(float3(g_Instances[dtid.x].World[1][0],
                          g_Instances[dtid.x].World[1][1],
                          g_Instances[dtid.x].World[1][2])),
            length(float3(g_Instances[dtid.x].World[2][0],
                          g_Instances[dtid.x].World[2][1],
                          g_Instances[dtid.x].World[2][2]))));

        bool visible = true;
        [unroll]
        for (uint i = 0; i < 6; ++i)
        {
            float dist = dot(g_FrustumPlanes[i].xyz, center) + g_FrustumPlanes[i].w;
            if (dist < -radius)
            {
                visible = false;
                break;
            }
        }

        if (visible)
        {
            uint slot;
            InterlockedAdd(gs_VisibleCount, 1, slot);
            // Write visible index for compaction (optional second pass)
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // First thread writes the indirect draw args
    if (gidx == 0 && gs_VisibleCount > 0)
    {
        uint argSlot;
        InterlockedAdd(g_DrawArgs[0].InstanceCount, gs_VisibleCount, argSlot);
    }
}
)";

    // ========================================================================
    // InstanceRenderer
    // ========================================================================

    /**
     * @brief High-level instanced rendering manager.
     *
     * Collects per-instance submissions each frame, batches them by
     * mesh+material, uploads structured buffers, and issues
     * DrawIndexedInstanced or DrawIndexedInstancedIndirect calls.
     *
     * Thread safety: Submit() must be called from the main thread only
     * (same restriction as all D3D11 rendering in SparkEngine).
     */
    class InstanceRenderer
    {
    public:
        InstanceRenderer() = default;
        ~InstanceRenderer() { Shutdown(); }

        // Non-copyable
        InstanceRenderer(const InstanceRenderer&) = delete;
        InstanceRenderer& operator=(const InstanceRenderer&) = delete;

        // --------------------------------------------------------------------
        // Lifecycle
        // --------------------------------------------------------------------

        /**
         * @brief Initialize the renderer, compile shaders, create shared state.
         * @param pDevice   D3D11 device.
         * @param pContext  D3D11 immediate context.
         * @return S_OK on success.
         */
        HRESULT Initialize(
            _In_ ID3D11Device* pDevice,
            _In_ ID3D11DeviceContext* pContext)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!pDevice || !pContext)
            {
                return E_INVALIDARG;
            }

            m_pDevice = pDevice;
            m_pContext = pContext;
            m_initialized = true;

            // Compile the instancing vertex shader
            HRESULT hr = CompileShaderFromString(
                INSTANCING_VS_HLSL, "main", "vs_5_0",
                m_vsBlob.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            hr = pDevice->CreateVertexShader(
                m_vsBlob->GetBufferPointer(),
                m_vsBlob->GetBufferSize(),
                nullptr,
                m_vertexShader.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            // Compile the fallback pixel shader
            ComPtr<ID3DBlob> psBlob;
            hr = CompileShaderFromString(
                INSTANCING_PS_HLSL, "main", "ps_5_0",
                psBlob.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            hr = pDevice->CreatePixelShader(
                psBlob->GetBufferPointer(),
                psBlob->GetBufferSize(),
                nullptr,
                m_pixelShader.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            // Create the standard input layout (Position, Normal, TexCoord)
            D3D11_INPUT_ELEMENT_DESC layoutDesc[] =
            {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
            };

            hr = pDevice->CreateInputLayout(
                layoutDesc, _countof(layoutDesc),
                m_vsBlob->GetBufferPointer(),
                m_vsBlob->GetBufferSize(),
                m_inputLayout.ReleaseAndGetAddressOf());
            if (FAILED(hr))
            {
                return hr;
            }

            // Initialize indirect draw buffer
            hr = m_indirectDrawBuffer.Initialize(pDevice, 256);
            if (FAILED(hr))
            {
                return hr;
            }

            return S_OK;
#else
            (void)pDevice;
            (void)pContext;
            return S_OK;
#endif
        }

        /**
         * @brief Release all GPU resources and clear batches.
         */
        void Shutdown()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            m_batches.clear();
            m_vertexShader.Reset();
            m_pixelShader.Reset();
            m_inputLayout.Reset();
            m_vsBlob.Reset();
            m_indirectDrawBuffer.Shutdown();
            m_pDevice = nullptr;
            m_pContext = nullptr;
            m_initialized = false;
#endif
        }

        // --------------------------------------------------------------------
        // Per-Frame Interface
        // --------------------------------------------------------------------

        /**
         * @brief Reset all batches for a new frame. Call at the start of each frame.
         */
        void BeginFrame()
        {
            m_stats = {};
            for (auto& [key, batch] : m_batches)
            {
                batch.Reset();
            }
        }

        /**
         * @brief Submit a single instance for deferred batched rendering.
         *
         * Instances with the same meshID + materialID are automatically
         * grouped into the same InstanceBatch.
         *
         * @param meshID      Opaque identifier for the mesh.
         * @param materialID  Opaque identifier for the material.
         * @param data        Per-instance transform and attribute data.
         */
        void Submit(uint64_t meshID, uint64_t materialID, const InstanceData& data)
        {
            BatchKey key{meshID, materialID};
            auto it = m_batches.find(key);
            if (it == m_batches.end())
            {
                auto [inserted, ok] = m_batches.emplace(key, InstanceBatch{});
                if (ok && m_pDevice)
                {
                    inserted->second.Initialize(m_pDevice);
                }
                it = inserted;
            }
            it->second.AddInstance(data);
            ++m_stats.TotalInstances;
        }

        /**
         * @brief Upload all instance buffers and issue draw calls.
         *
         * Performs per-batch frustum culling (if bounds are set), uploads
         * dirty buffers, binds the instancing shader, and draws.
         *
         * @param frustum  Current camera frustum for per-batch culling.
         */
        void Flush(const Frustum& frustum)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_initialized || !m_pDevice || !m_pContext)
            {
                return;
            }

            m_stats.BatchCount = static_cast<uint32_t>(m_batches.size());

            for (auto& [key, batch] : m_batches)
            {
                if (batch.GetInstanceCount() == 0)
                {
                    continue;
                }

                // Per-batch frustum culling
                if (m_frustumCullingEnabled && batch.HasBounds())
                {
                    if (!frustum.IsVisible(batch.GetBounds()))
                    {
                        ++m_stats.FrustumCulledBatches;
                        continue;
                    }
                }

                // Upload
                uint32_t prevCapacity = batch.GetCapacity();
                HRESULT hr = batch.UploadToGPU(m_pDevice, m_pContext);
                if (FAILED(hr))
                {
                    continue;
                }
                ++m_stats.BufferUploads;

                if (batch.GetCapacity() != prevCapacity)
                {
                    ++m_stats.BufferGrows;
                }

                m_stats.VisibleInstances += batch.GetInstanceCount();
            }
#else
            (void)frustum;
#endif
        }

        /**
         * @brief Issue DrawIndexedInstanced calls for all uploaded batches.
         *
         * Call after Flush(). The caller is responsible for binding the
         * correct vertex/index buffers for each mesh before calling this,
         * or use the callback overload.
         *
         * @param bindMeshCallback  Called per batch with (meshID, materialID)
         *                          so the caller can bind the right VB/IB.
         */
        void DrawInstanced(
            const std::function<void(uint64_t meshID, uint64_t materialID,
                                     uint32_t indexCount)>& bindMeshCallback)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_initialized || !m_pContext)
            {
                return;
            }

            // Bind instancing shaders and input layout
            m_pContext->IASetInputLayout(m_inputLayout.Get());
            m_pContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
            m_pContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);

            for (auto& [key, batch] : m_batches)
            {
                uint32_t instanceCount = batch.GetInstanceCount();
                if (instanceCount == 0)
                {
                    continue;
                }

                // Bind structured buffer SRV to VS slot t0
                ID3D11ShaderResourceView* srv = batch.GetSRV();
                m_pContext->VSSetShaderResources(0, 1, &srv);

                // Let caller bind the mesh's VB/IB and tell us the index count
                uint32_t indexCount = 0;
                if (bindMeshCallback)
                {
                    // The callback should bind VB/IB and return index count
                    // via the indexCount parameter captured by reference
                    bindMeshCallback(key.MeshID, key.MaterialID, indexCount);
                }

                if (indexCount > 0)
                {
                    m_pContext->DrawIndexedInstanced(
                        indexCount, instanceCount, 0, 0, 0);
                }

                ++m_stats.DrawCallsIssued;
            }

            m_stats.DrawCallsSaved =
                (m_stats.TotalInstances > m_stats.DrawCallsIssued)
                    ? (m_stats.TotalInstances - m_stats.DrawCallsIssued)
                    : 0;
#else
            (void)bindMeshCallback;
#endif
        }

        /**
         * @brief Issue DrawIndexedInstancedIndirect calls using the indirect buffer.
         *
         * Use when GPU culling has populated the IndirectDrawBuffer.
         *
         * @param bindMeshCallback  Per-batch callback for VB/IB binding.
         */
        void DrawInstancedIndirect(
            const std::function<void(uint64_t meshID, uint64_t materialID)>& bindMeshCallback)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            if (!m_initialized || !m_pContext)
            {
                return;
            }

            m_pContext->IASetInputLayout(m_inputLayout.Get());
            m_pContext->VSSetShader(m_vertexShader.Get(), nullptr, 0);
            m_pContext->PSSetShader(m_pixelShader.Get(), nullptr, 0);

            uint32_t batchIndex = 0;
            for (auto& [key, batch] : m_batches)
            {
                if (batch.GetInstanceCount() == 0)
                {
                    continue;
                }

                ID3D11ShaderResourceView* srv = batch.GetSRV();
                m_pContext->VSSetShaderResources(0, 1, &srv);

                if (bindMeshCallback)
                {
                    bindMeshCallback(key.MeshID, key.MaterialID);
                }

                uint32_t offset = batchIndex *
                    static_cast<uint32_t>(sizeof(IndirectDrawBuffer::DrawArgs));

                m_pContext->DrawIndexedInstancedIndirect(
                    m_indirectDrawBuffer.GetArgBuffer(), offset);

                ++m_stats.DrawCallsIssued;
                ++batchIndex;
            }

            m_stats.DrawCallsSaved =
                (m_stats.TotalInstances > m_stats.DrawCallsIssued)
                    ? (m_stats.TotalInstances - m_stats.DrawCallsIssued)
                    : 0;
#else
            (void)bindMeshCallback;
#endif
        }

        /**
         * @brief Finalize frame. Currently a no-op placeholder for future use.
         */
        void EndFrame()
        {
            // Reserved for double-buffered upload or async readback
        }

        // --------------------------------------------------------------------
        // Configuration
        // --------------------------------------------------------------------

        void SetFrustumCullingEnabled(bool enabled) { m_frustumCullingEnabled = enabled; }
        bool IsFrustumCullingEnabled() const { return m_frustumCullingEnabled; }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // --------------------------------------------------------------------
        // Statistics & Diagnostics
        // --------------------------------------------------------------------

        const RenderStatistics& GetStatistics() const { return m_stats; }

        /**
         * @brief Format statistics as a human-readable string.
         */
        std::string GetStatisticsString() const
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "InstanceRenderer: %u instances, %u batches, "
                     "%u draw calls (%u saved), %u culled batches, "
                     "%u uploads, %u grows",
                     m_stats.TotalInstances,
                     m_stats.BatchCount,
                     m_stats.DrawCallsIssued,
                     m_stats.DrawCallsSaved,
                     m_stats.FrustumCulledBatches,
                     m_stats.BufferUploads,
                     m_stats.BufferGrows);
            return std::string(buf);
        }

        // --------------------------------------------------------------------
        // Console Integration
        // --------------------------------------------------------------------

        /**
         * @brief Register console commands for runtime control.
         *
         * Registers:
         *  - `inst_stats`   : Print current frame statistics
         *  - `inst_enabled` : Toggle instanced rendering on/off
         *  - `inst_culling` : Toggle per-batch frustum culling
         *
         * @tparam ConsoleT  Console type (SimpleConsole) to avoid hard include.
         * @param console     Reference to the console singleton.
         */
        template <typename ConsoleT>
        void RegisterConsoleCommands(ConsoleT& console)
        {
            console.RegisterCommand(
                "inst_stats",
                [this](const std::vector<std::string>&) -> std::string
                {
                    return GetStatisticsString();
                },
                "Print instance renderer statistics for the current frame",
                "Graphics");

            console.RegisterCommand(
                "inst_enabled",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!args.empty())
                    {
                        m_enabled = (args[0] == "1" || args[0] == "true");
                    }
                    return std::string("Instance rendering: ") +
                           (m_enabled ? "ON" : "OFF");
                },
                "Toggle instanced rendering (inst_enabled [0|1])",
                "Graphics");

            console.RegisterCommand(
                "inst_culling",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!args.empty())
                    {
                        m_frustumCullingEnabled =
                            (args[0] == "1" || args[0] == "true");
                    }
                    return std::string("Frustum culling: ") +
                           (m_frustumCullingEnabled ? "ON" : "OFF");
                },
                "Toggle per-batch frustum culling (inst_culling [0|1])",
                "Graphics");
        }

        // --------------------------------------------------------------------
        // Accessors
        // --------------------------------------------------------------------

        IndirectDrawBuffer& GetIndirectDrawBuffer() { return m_indirectDrawBuffer; }
        const IndirectDrawBuffer& GetIndirectDrawBuffer() const { return m_indirectDrawBuffer; }

        /** @brief Access the VS blob for creating compatible input layouts. */
#ifdef SPARK_PLATFORM_WINDOWS
        ID3DBlob* GetVSBlob() const { return m_vsBlob.Get(); }
        ID3D11VertexShader* GetVertexShader() const { return m_vertexShader.Get(); }
        ID3D11PixelShader* GetPixelShader() const { return m_pixelShader.Get(); }
#endif

    private:
        // --------------------------------------------------------------------
        // Shader Compilation Helper
        // --------------------------------------------------------------------

#ifdef SPARK_PLATFORM_WINDOWS
        static HRESULT CompileShaderFromString(
            const char* source,
            const char* entryPoint,
            const char* target,
            ID3DBlob** ppBlob)
        {
            ComPtr<ID3DBlob> errorBlob;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

#if defined(_DEBUG) || defined(DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
            flags &= ~D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

            HRESULT hr = D3DCompile(
                source,
                strlen(source),
                nullptr,    // source name
                nullptr,    // defines
                nullptr,    // includes
                entryPoint,
                target,
                flags,
                0,
                ppBlob,
                errorBlob.GetAddressOf());

            if (FAILED(hr) && errorBlob)
            {
                OutputDebugStringA(
                    static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            return hr;
        }
#endif

        // --------------------------------------------------------------------
        // Member Data
        // --------------------------------------------------------------------

        ID3D11Device*        m_pDevice = nullptr;   ///< Non-owning
        ID3D11DeviceContext*  m_pContext = nullptr;  ///< Non-owning
        bool                 m_initialized = false;
        bool                 m_enabled = true;
        bool                 m_frustumCullingEnabled = true;

        /// Batches keyed by mesh+material
        std::unordered_map<BatchKey, InstanceBatch, BatchKeyHash> m_batches;

        /// Indirect draw argument buffer
        IndirectDrawBuffer m_indirectDrawBuffer;

        /// Per-frame statistics
        RenderStatistics m_stats{};

#ifdef SPARK_PLATFORM_WINDOWS
        ComPtr<ID3D11VertexShader> m_vertexShader;
        ComPtr<ID3D11PixelShader>  m_pixelShader;
        ComPtr<ID3D11InputLayout>  m_inputLayout;
        ComPtr<ID3DBlob>           m_vsBlob;
#endif
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
