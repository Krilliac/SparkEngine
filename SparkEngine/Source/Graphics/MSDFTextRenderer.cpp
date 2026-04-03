/**
 * @file MSDFTextRenderer.cpp
 * @brief MSDF text rendering implementation with instanced glyph quads
 */

#include "MSDFTextRenderer.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include <cstring>

namespace Spark::Graphics
{

    // =========================================================================
    // MSDF Shader Source
    // =========================================================================

    static const char* kTextVS = R"(
cbuffer TextCB : register(b0) {
    float4x4 projection;
    float4 screenSize;
};
struct VS_IN {
    float2 pos      : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 params   : TEXCOORD1;
};
struct VS_OUT {
    float4 pos      : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 params   : TEXCOORD1;
};
VS_OUT main(VS_IN v) {
    VS_OUT o;
    o.pos = mul(projection, float4(v.pos, 0, 1));
    o.uv = v.uv;
    o.color = v.color;
    o.params = v.params;
    return o;
}
)";

    static const char* kTextPS = R"(
Texture2D<float4> atlasTex : register(t0);
SamplerState linearSamp : register(s0);

struct PS_IN {
    float4 pos      : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 params   : TEXCOORD1;  // x=outlineWidth, y=hasShadow, z=distRange, w=fontSize
};

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

float4 main(PS_IN input) : SV_Target {
    float3 msdf = atlasTex.Sample(linearSamp, input.uv).rgb;

    // Compute signed distance from the median of RGB channels
    float sd = median(msdf.r, msdf.g, msdf.b);

    // Screen-space distance for anti-aliased edges
    float distRange = input.params.z;
    float fontSize = input.params.w;
    float screenPxRange = distRange * (fontSize / 32.0); // Scale factor
    screenPxRange = max(screenPxRange, 1.0);

    float screenPxDistance = screenPxRange * (sd - 0.5);
    float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);

    // Outline
    float outlineWidth = input.params.x;
    if (outlineWidth > 0.0) {
        float outlineDist = screenPxRange * (sd - 0.5 + outlineWidth);
        float outlineOpacity = clamp(outlineDist + 0.5, 0.0, 1.0);

        // Blend: outline where glyph body is transparent
        float bodyMask = opacity;
        float outlineMask = outlineOpacity * (1.0 - bodyMask);

        float4 bodyColor = input.color * bodyMask;
        float4 outlineColor = float4(0, 0, 0, 1) * outlineMask; // Black outline
        return bodyColor + outlineColor;
    }

    return float4(input.color.rgb, input.color.a * opacity);
}
)";

    // =========================================================================
    // Implementation
    // =========================================================================

    bool MSDFTextRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t screenWidth,
                                      uint32_t screenHeight)
    {
        m_device = device;
        m_context = context;
        m_screenWidth = screenWidth;
        m_screenHeight = screenHeight;

        if (!CompileShaders())
            return false;
        if (!CreateBuffers())
            return false;

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MSDFTextRenderer initialized (%ux%u screen)", screenWidth,
                       screenHeight);
        return true;
    }

    void MSDFTextRenderer::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MSDFTextRenderer shutting down (%zu fonts loaded)",
                       m_fonts.size());
        m_fonts.clear();
        m_activeFont = nullptr;
        m_initialized = false;
    }

    bool MSDFTextRenderer::CompileShaders()
    {
        UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        ComPtr<ID3DBlob> errorBlob;

        // Vertex shader
        HRESULT hr = D3DCompile(kTextVS, strlen(kTextVS), "TextVS", nullptr, nullptr, "main", "vs_5_0", flags, 0,
                                &vsBlob, &errorBlob);
        if (FAILED(hr))
            return false;
        hr =
            m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
        if (FAILED(hr))
            return false;

        // Input layout
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr =
            m_device->CreateInputLayout(layout, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
        if (FAILED(hr))
            return false;

        // Pixel shader
        hr = D3DCompile(kTextPS, strlen(kTextPS), "TextPS", nullptr, nullptr, "main", "ps_5_0", flags, 0, &psBlob,
                        &errorBlob);
        if (FAILED(hr))
            return false;
        hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
        if (FAILED(hr))
            return false;

        return true;
    }

    bool MSDFTextRenderer::CreateBuffers()
    {
        // Vertex buffer (dynamic, holds glyph quads)
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = kMaxGlyphsPerBatch * 6 * sizeof(GlyphVertex); // 6 verts per glyph (2 triangles)
        vbDesc.Usage = D3D11_USAGE_DYNAMIC;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = m_device->CreateBuffer(&vbDesc, nullptr, &m_vertexBuffer);
        if (FAILED(hr))
            return false;

        // Constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(TextCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        if (FAILED(hr))
            return false;

        // Sampler
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = m_device->CreateSamplerState(&sampDesc, &m_linearSampler);
        if (FAILED(hr))
            return false;

        // Alpha blend state
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = m_device->CreateBlendState(&blendDesc, &m_alphaBlendState);
        if (FAILED(hr))
            return false;

        // Depth stencil state (no depth test for UI text)
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable = FALSE;
        hr = m_device->CreateDepthStencilState(&dsDesc, &m_depthStencilState);
        if (FAILED(hr))
            return false;

        // Rasterizer state
        D3D11_RASTERIZER_DESC rsDesc = {};
        rsDesc.FillMode = D3D11_FILL_SOLID;
        rsDesc.CullMode = D3D11_CULL_NONE;
        rsDesc.ScissorEnable = FALSE;
        hr = m_device->CreateRasterizerState(&rsDesc, &m_rasterizerState);
        if (FAILED(hr))
            return false;

        return true;
    }

    bool MSDFTextRenderer::LoadFont(const std::string& fontName, const uint8_t* atlasData, uint32_t atlasWidth,
                                    uint32_t atlasHeight, const MSDFFont& font)
    {
        LoadedFont loaded;
        loaded.metrics = font;

        // Create atlas texture
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = atlasWidth;
        texDesc.Height = atlasHeight;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = atlasData;
        initData.SysMemPitch = atlasWidth * 4;

        HRESULT hr = m_device->CreateTexture2D(&texDesc, &initData, &loaded.atlasTexture);
        if (FAILED(hr))
            return false;

        hr = m_device->CreateShaderResourceView(loaded.atlasTexture.Get(), nullptr, &loaded.atlasSRV);
        if (FAILED(hr))
            return false;

        m_fonts[fontName] = std::move(loaded);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "MSDF font atlas loaded: '%s' (%ux%u)", fontName.c_str(),
                       atlasWidth, atlasHeight);
        return true;
    }

    bool MSDFTextRenderer::SetActiveFont(const std::string& fontName)
    {
        auto it = m_fonts.find(fontName);
        if (it == m_fonts.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "MSDFTextRenderer: font '%s' not found", fontName.c_str());
            return false;
        }
        m_activeFont = &it->second;
        return true;
    }

    void MSDFTextRenderer::DrawText(const TextDrawRequest& request)
    {
        if (!m_activeFont)
            return;
        GenerateGlyphQuads(request);
    }

    void MSDFTextRenderer::GenerateGlyphQuads(const TextDrawRequest& request)
    {
        const auto& font = m_activeFont->metrics;
        float scale = request.fontSize;
        float cursorX = request.x;
        float cursorY = request.y;
        uint32_t prevCodepoint = 0;

        for (size_t i = 0; i < request.text.size(); ++i)
        {
            uint32_t codepoint = static_cast<uint32_t>(request.text[i]);

            if (codepoint == '\n')
            {
                cursorX = request.x;
                cursorY += font.lineHeight * scale;
                prevCodepoint = 0;
                continue;
            }

            const MSDFGlyph* glyph = font.GetGlyph(codepoint);
            if (!glyph)
            {
                glyph = font.GetGlyph('?'); // Fallback
                if (!glyph)
                    continue;
            }

            // Apply kerning
            if (prevCodepoint != 0)
                cursorX += font.GetKerning(prevCodepoint, codepoint) * scale;

            // Glyph quad corners in screen space
            float x0 = cursorX + glyph->planeBoundsLeft * scale;
            float y0 = cursorY - glyph->planeBoundsTop * scale;
            float x1 = cursorX + glyph->planeBoundsRight * scale;
            float y1 = cursorY - glyph->planeBoundsBottom * scale;

            // Atlas UVs
            float u0 = glyph->atlasLeft;
            float v0 = glyph->atlasTop;
            float u1 = glyph->atlasRight;
            float v1 = glyph->atlasBottom;

            XMFLOAT4 params = {request.hasOutline ? request.outlineWidth : 0.0f, request.hasShadow ? 1.0f : 0.0f,
                               font.distanceRange, request.fontSize};

            // Two triangles (6 vertices)
            GlyphVertex v;
            v.color = request.color;
            v.params = params;

            // Triangle 1
            v.position = {x0, y0};
            v.texCoord = {u0, v0};
            m_glyphVertices.push_back(v);
            v.position = {x1, y0};
            v.texCoord = {u1, v0};
            m_glyphVertices.push_back(v);
            v.position = {x0, y1};
            v.texCoord = {u0, v1};
            m_glyphVertices.push_back(v);

            // Triangle 2
            v.position = {x1, y0};
            v.texCoord = {u1, v0};
            m_glyphVertices.push_back(v);
            v.position = {x1, y1};
            v.texCoord = {u1, v1};
            m_glyphVertices.push_back(v);
            v.position = {x0, y1};
            v.texCoord = {u0, v1};
            m_glyphVertices.push_back(v);

            cursorX += glyph->advance * scale;
            prevCodepoint = codepoint;
        }
    }

    void MSDFTextRenderer::Flush()
    {
        if (!m_initialized || !m_activeFont || m_glyphVertices.empty())
            return;

        // Update constant buffer with orthographic projection
        TextCB cb = {};
        float w = static_cast<float>(m_screenWidth);
        float h = static_cast<float>(m_screenHeight);

        // Orthographic projection (screen space: 0,0 top-left)
        XMMATRIX proj = XMMatrixOrthographicOffCenterLH(0.0f, w, h, 0.0f, 0.0f, 1.0f);
        XMStoreFloat4x4(&cb.projection, XMMatrixTranspose(proj));
        cb.screenSize = {w, h, 1.0f / w, 1.0f / h};

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return;
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_constantBuffer.Get(), 0);

        // Upload glyph vertices
        size_t vertCount = std::min(m_glyphVertices.size(), static_cast<size_t>(kMaxGlyphsPerBatch * 6));
        if (m_glyphVertices.size() > static_cast<size_t>(kMaxGlyphsPerBatch * 6))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "MSDFTextRenderer: glyph count %zu exceeds batch limit %u, clamping",
                           m_glyphVertices.size() / 6, kMaxGlyphsPerBatch);
        }
        if (FAILED(m_context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
            return;
        memcpy(mapped.pData, m_glyphVertices.data(), vertCount * sizeof(GlyphVertex));
        m_context->Unmap(m_vertexBuffer.Get(), 0);

        // Set pipeline state
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->IASetInputLayout(m_inputLayout.Get());
        UINT stride = sizeof(GlyphVertex);
        UINT offset = 0;
        ID3D11Buffer* vb = m_vertexBuffer.Get();
        m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);

        ID3D11Buffer* cbs[] = {m_constantBuffer.Get()};
        m_context->VSSetConstantBuffers(0, 1, cbs);

        ID3D11ShaderResourceView* srv = m_activeFont->atlasSRV.Get();
        m_context->PSSetShaderResources(0, 1, &srv);

        ID3D11SamplerState* samp = m_linearSampler.Get();
        m_context->PSSetSamplers(0, 1, &samp);

        float blendFactor[4] = {0, 0, 0, 0};
        m_context->OMSetBlendState(m_alphaBlendState.Get(), blendFactor, 0xFFFFFFFF);
        m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        m_context->RSSetState(m_rasterizerState.Get());

        // Draw
        m_context->Draw(static_cast<UINT>(vertCount), 0);

        m_glyphVertices.clear();
    }

    void MSDFTextRenderer::OnResize(uint32_t width, uint32_t height)
    {
        m_screenWidth = width;
        m_screenHeight = height;
    }

    XMFLOAT2 MSDFTextRenderer::MeasureText(const std::string& text, float fontSize) const
    {
        if (!m_activeFont)
            return {0.0f, 0.0f};

        const auto& font = m_activeFont->metrics;
        float maxWidth = 0.0f;
        float cursorX = 0.0f;
        int lineCount = 1;
        uint32_t prevCodepoint = 0;

        for (size_t i = 0; i < text.size(); ++i)
        {
            uint32_t cp = static_cast<uint32_t>(text[i]);
            if (cp == '\n')
            {
                maxWidth = std::max(maxWidth, cursorX);
                cursorX = 0.0f;
                lineCount++;
                prevCodepoint = 0;
                continue;
            }

            const MSDFGlyph* glyph = font.GetGlyph(cp);
            if (!glyph)
                continue;

            if (prevCodepoint != 0)
                cursorX += font.GetKerning(prevCodepoint, cp) * fontSize;

            cursorX += glyph->advance * fontSize;
            prevCodepoint = cp;
        }

        maxWidth = std::max(maxWidth, cursorX);
        return {maxWidth, static_cast<float>(lineCount) * font.lineHeight * fontSize};
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
