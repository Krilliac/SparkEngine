/**
 * @file GraphicsDeviceResourcesWindowsShaders.cpp
 * @brief D3D11 basic shader system initialization and shader compilation
 *
 * Basic shader / constant buffer initialization plus HLSL compilation from
 * file and the embedded fallback shaders, split out of
 * GraphicsDeviceResourcesWindows.cpp (which keeps device and render-target
 * creation plus pipeline setup). Linux counterpart lives in
 * GraphicsDeviceResourcesLinuxShaders.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "Shader.h"
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>
#include <d3dcompiler.h>

#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================================
// BASIC SHADER SYSTEM IMPLEMENTATION
// ============================================================================

HRESULT GraphicsEngine::InitializeBasicShaders()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Initializing basic shader system", L"INFO");

    // Create constant buffer first
    HRESULT hr = CreateBasicConstantBuffer();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create basic constant buffer", L"ERROR");
        return hr;
    }

    // The embedded source is the single source of truth for the basic shaders:
    // it is compiled from this translation unit, so its cbuffer layout cannot
    // drift from PerObjectConstants/PerFrameConstants. The previous from-file
    // attempt named Shaders/HLSL/BasicVertex.hlsl, which has never existed in
    // the repository, so it failed on every launch and logged a warning before
    // taking this path anyway.
    ComPtr<ID3DBlob> vsBlob;
    hr = CompileEmbeddedVertexShader(&vsBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to compile embedded vertex shader", L"ERROR");
        return hr;
    }

    // Create vertex shader
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                      &m_basicVertexShader);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create vertex shader", L"ERROR");
        return hr;
    }

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};

    hr = m_device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                     &m_basicInputLayout);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create input layout", L"ERROR");
        return hr;
    }

    // Embedded for the same reason as the vertex shader above.
    ComPtr<ID3DBlob> psBlob;
    hr = CompileEmbeddedPixelShader(&psBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to compile embedded pixel shader", L"ERROR");
        return hr;
    }

    // Create pixel shader
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_basicPixelShader);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create pixel shader", L"ERROR");
        return hr;
    }

    // Create basic sampler state
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = m_device->CreateSamplerState(&samplerDesc, &m_basicSamplerState);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create sampler state", L"ERROR");
        return hr;
    }

    // Create default 1x1 white texture
    hr = CreateDefaultTexture();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Basic shader system initialized successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateBasicConstantBuffer()
{
    // Create constant buffer for per-object rendering constants
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(PerObjectConstants);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.MiscFlags = 0;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_basicConstantBuffer);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create basic constant buffer", L"ERROR");
        return hr;
    }

    // Create constant buffer for per-frame constants
    bufferDesc.ByteWidth = sizeof(PerFrameConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_basicFrameConstantBuffer);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create frame constant buffer", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Basic constant buffers created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CompileShaderFromFile(const std::wstring& filename, const char* entryPoint,
                                              const char* shaderModel, ID3DBlob** blobOut)
{
    HRESULT hr = S_OK;

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompileFromFile(filename.c_str(), nullptr, nullptr, entryPoint, shaderModel, shaderFlags, 0, blobOut,
                            &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedVertexShader(ID3DBlob** blobOut)
{
    // Embedded vertex shader source code
    const char* vertexShaderSource = R"(
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;
            matrix WorldViewProjection;
            matrix WorldInverseTranspose;
            // MUST mirror Shader.h PerObjectConstants exactly. This field was
            // missing, shifting every following member by 64 bytes — the GPU
            // read ObjectColor from a zeroed matrix region, so every basic-
            // shader draw came out rgba(0,0,0,0): the black-screen bug.
            matrix PreviousWorld;
            float3 ObjectPosition;
            float ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties;
            float4 UVTiling;
        };

        struct VertexInput
        {
            float3 Position : POSITION;
            float3 Normal   : NORMAL;
            float2 TexCoord : TEXCOORD0;
        };

        struct VertexOutput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        VertexOutput main(VertexInput input)
        {
            VertexOutput output = (VertexOutput)0;

            output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
            output.WorldPos = mul(float4(input.Position, 1.0f), World).xyz;
            output.Normal = normalize(mul(input.Normal, (float3x3)WorldInverseTranspose));
            output.TexCoord = input.TexCoord * UVTiling.xy + UVTiling.zw;
            output.Color = ObjectColor;

            return output;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "EmbeddedVertexShader", nullptr, nullptr,
                            "main", "vs_5_0", shaderFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded vertex shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded vertex shader compiled successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedPixelShader(ID3DBlob** blobOut)
{
    // Embedded pixel shader source code
    const char* pixelShaderSource = R"(
        cbuffer PerFrameConstants : register(b1)
        {
            matrix ViewMatrix;
            matrix ProjectionMatrix;
            matrix ViewProjectionMatrix;
            float3 CameraPosition;
            float Time;
            float3 CameraDirection;
            float DeltaTime;
            float2 ScreenResolution;
            float2 InvScreenResolution;

            float3 DirectionalLightDir;
            float DirectionalLightIntensity;
            float3 DirectionalLightColor;
            float AmbientIntensity;
            float3 AmbientColor;
            float _padding1;
        };

        // Per-object slot b0 (bound to the PS in SetBasicShaders). Mirrors the
        // vertex shader's layout; the PS only needs MaterialProperties
        // (z: emissive, w: alpha), but the whole cbuffer must be declared so the
        // field offsets line up with the constant buffer.
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;
            matrix WorldViewProjection;
            matrix WorldInverseTranspose;
            matrix PreviousWorld;
            float3 ObjectPosition;
            float  ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties; // x: metallic, y: roughness, z: emissive, w: alpha
            float4 UVTiling;
        };

        Texture2D MainTexture : register(t0);
        // Normal (t1) + roughness (t2) maps for the tangent-less normal-mapping
        // path. SetBasicShaders() default-binds a flat FLOAT normal (0.5,0.5,1)
        // and a fully-rough (1.0) roughness, which make both new terms exact
        // no-ops — see CotangentFrame() and the specular block below.
        Texture2D NormalTexture : register(t1);
        Texture2D RoughnessTexture : register(t2);
        SamplerState MainSampler : register(s0);

        struct PixelInput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        // Screen-space cotangent frame (Schuler, "Normal Mapping Without
        // Precomputed Tangents" / Mikkelsen's derivative method). The vertex
        // format has no tangents, so we rebuild the (T, B, N) frame per pixel:
        // ddx/ddy of the world position give two in-triangle edge vectors,
        // ddx/ddy of the UVs give the same edges in texture space, and solving
        // that 2x2 system (via the cross-product co-vectors below) yields the
        // directions in which u and v increase across the surface.
        float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
        {
            float3 dp1 = ddx(p);
            float3 dp2 = ddy(p);
            float2 duv1 = ddx(uv);
            float2 duv2 = ddy(uv);

            // Co-vectors perpendicular to N: projecting the edge vectors
            // through these solves for T (du direction) and B (dv direction)
            // while keeping both in the surface plane.
            float3 dp2perp = cross(dp2, N);
            float3 dp1perp = cross(N, dp1);
            float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
            float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

            // Scale-invariant normalization (max keeps the T:B aspect so
            // mirrored/anisotropic UVs stay correct). Guard degenerate UVs:
            // constant texcoords across the pixel quad make T = B = 0, and
            // rsqrt(0) = INF would turn the frame into NaNs; forcing 0 makes
            // the tangential components drop out so N passes through instead.
            float maxLen2 = max(dot(T, T), dot(B, B));
            float invmax = (maxLen2 > 1e-10f) ? rsqrt(maxLen2) : 0.0f;
            return float3x3(T * invmax, B * invmax, N);
        }

        float4 main(PixelInput input) : SV_TARGET
        {
            float4 texColor = MainTexture.Sample(MainSampler, input.TexCoord);

            // Perturb the geometric normal with the t1 normal map.
            // Identity proof for the default binding: the default texture is
            // FLOAT-format (0.5, 0.5, 1), so nTS = sample*2-1 == (0, 0, 1)
            // EXACTLY (an 8-bit 128/255 texel would leave +0.0039 in x/y).
            // mul(rowVector, matrix) = x*T + y*B + z*N, so (0,0,1) selects the
            // third row: geoN, already unit length -- the lighting below sees
            // the same normalize(input.Normal) the pre-normal-map shader used.
            float3 geoN = normalize(input.Normal);
            float3 nTS = NormalTexture.Sample(MainSampler, input.TexCoord).xyz * 2.0f - 1.0f;
            float3x3 TBN = CotangentFrame(geoN, input.WorldPos, input.TexCoord);
            float3 normal = normalize(mul(nTS, TBN));

            float3 lightDir = normalize(-DirectionalLightDir);
            float NdotL = max(0.0f, dot(normal, lightDir));

            float3 diffuse = DirectionalLightColor * DirectionalLightIntensity * NdotL;
            float3 ambient = AmbientColor * AmbientIntensity;

            // Soft camera-facing fill (headlight): surfaces turned toward the
            // viewer get a little extra warm light so they read with detail
            // instead of crushing to black when they face away from the sun.
            float3 viewDir = normalize(CameraPosition - input.WorldPos);
            float fill = 0.35f * max(0.0f, dot(normal, viewDir));

            float3 lighting = diffuse + ambient + float3(fill, fill * 0.95f, fill * 0.88f);

            // Modest Blinn-Phong specular scaled by (1 - roughness). The
            // default t2 binding is fully rough (1.0), so gloss == 0 and this
            // whole term is EXACTLY zero for draws that never bind a roughness
            // map -- specular is opt-in per material. The saturate(NdotL*4)
            // fade kills the highlight on the unlit side without a hard edge.
            float roughness = saturate(RoughnessTexture.Sample(MainSampler, input.TexCoord).r);
            float gloss = 1.0f - roughness;
            float3 halfVec = normalize(lightDir + viewDir);
            float NdotH = max(0.0f, dot(normal, halfVec));
            float specPower = lerp(8.0f, 96.0f, gloss); // rough -> broad sheen, smooth -> tight highlight
            float3 specular = DirectionalLightColor * DirectionalLightIntensity *
                              pow(NdotH, specPower) * gloss * 0.5f * saturate(NdotL * 4.0f);

            float4 finalColor = texColor * input.Color;
            finalColor.rgb *= lighting;
            finalColor.rgb += specular; // dielectric-style: not tinted by albedo

            // Emissive term (MaterialProperties.z, default 0 == no change): adds
            // the surface color back UNLIT so glow strips / holo panels / muzzle
            // flashes read as light sources even in shadow. Backward-compatible.
            finalColor.rgb += texColor.rgb * input.Color.rgb * MaterialProperties.z;

            // Alpha (MaterialProperties.w, default 1): lets materials fade when a
            // blend state is active; opaque draws keep w==1 so this is a no-op.
            finalColor.a = texColor.a * input.Color.a * MaterialProperties.w;

            return finalColor;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(pixelShaderSource, strlen(pixelShaderSource), "EmbeddedPixelShader", nullptr, nullptr,
                            "main", "ps_5_0", shaderFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compiled successfully", L"SUCCESS");
    return S_OK;
}
#endif // SPARK_PLATFORM_WINDOWS
