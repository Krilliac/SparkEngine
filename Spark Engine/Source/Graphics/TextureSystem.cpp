#ifdef SPARK_PLATFORM_WINDOWS
#include "../Core/Platform.h"
/**
 * @file TextureSystem.cpp
 * @brief Implementation of advanced texture loading and management system
 */

#include "TextureSystem.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <dxgi.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <wincodec.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>

using namespace DirectX;

// ============================================================================
// TEXTURE CLASS IMPLEMENTATION
// ============================================================================

Texture::Texture(const std::string& name, const TextureDesc& desc)
    : m_name(name), m_desc(desc)
{
}

HRESULT Texture::CreateFromFile(const std::string& filePath, ID3D11Device* device)
{
    ASSERT(device);
    
    // Load image using WIC
    HRESULT hr = S_OK;
    IWICImagingFactory* pFactory = nullptr;
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pFrame = nullptr;
    IWICFormatConverter* pConverter = nullptr;
    UINT width, height;
    
    // Create WIC factory
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, 
                          IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return hr;
    
    // Create decoder
    hr = pFactory->CreateDecoderFromFilename(
        std::wstring(filePath.begin(), filePath.end()).c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &pDecoder);
    if (FAILED(hr)) return hr;
    
    // Get the first frame
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) return hr;
    
    // Convert to RGBA format
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) return hr;
    
    hr = pConverter->Initialize(
        pFrame,
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    
    // Get image dimensions
    hr = pFrame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    
    // Update descriptor
    m_desc.width = static_cast<uint32_t>(width);
    m_desc.height = static_cast<uint32_t>(height);
    m_desc.mipLevels = 1;
    m_desc.arraySize = 1;
    m_desc.format = TextureFormat::R8G8B8A8_UNORM;
    
    // Create texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = m_desc.mipLevels;
    texDesc.ArraySize = m_desc.arraySize;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = m_desc.sampleCount;
    texDesc.SampleDesc.Quality = m_desc.sampleQuality;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = nullptr;
    initData.SysMemPitch = m_desc.width * 4;
    initData.SysMemSlicePitch = initData.SysMemPitch * m_desc.height;
    
    // Create texture
    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr)) return hr;
    
    m_resource = texture;
    
    // Create shader resource view
    hr = CreateViews(device);
    if (SUCCEEDED(hr)) {
        m_loaded = true;
        m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
        Spark::SimpleConsole::GetInstance().LogInfo("Loaded texture: " + filePath);
    }
    
    // Cleanup
    if (pFactory) pFactory->Release();
    if (pDecoder) pDecoder->Release();
    if (pFrame) pFrame->Release();
    if (pConverter) pConverter->Release();
    
    return hr;
}

HRESULT Texture::CreateFromData(const void* data, size_t dataSize, ID3D11Device* device)
{
    ASSERT(device && data && dataSize > 0);
    
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = m_desc.mipLevels;
    texDesc.ArraySize = m_desc.arraySize;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = m_desc.sampleCount;
    texDesc.SampleDesc.Quality = m_desc.sampleQuality;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::RenderTarget)) {
        texDesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::DepthStencil)) {
        texDesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    }
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::UnorderedAccess)) {
        texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = data;
    initData.SysMemPitch = m_desc.width * GetFormatBytesPerPixel(m_desc.format);
    initData.SysMemSlicePitch = initData.SysMemPitch * m_desc.height;
    
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr)) return hr;
    
    m_resource = texture;
    
    hr = CreateViews(device);
    if (SUCCEEDED(hr)) {
        m_loaded = true;
        m_memoryUsage = dataSize;
    }
    
    return hr;
}

HRESULT Texture::CreateRenderTarget(ID3D11Device* device)
{
    ASSERT(device);
    
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = m_desc.mipLevels;
    texDesc.ArraySize = m_desc.arraySize;
    texDesc.Format = GetDXGIFormat(m_desc.format);
    texDesc.SampleDesc.Count = m_desc.sampleCount;
    texDesc.SampleDesc.Quality = m_desc.sampleQuality;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &texture);
    if (FAILED(hr)) return hr;
    
    m_resource = texture;
    
    hr = CreateViews(device);
    if (SUCCEEDED(hr)) {
        m_loaded = true;
        m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    }
    
    return hr;
}

HRESULT Texture::CreateDepthStencil(ID3D11Device* device)
{
    ASSERT(device);
    
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_desc.width;
    texDesc.Height = m_desc.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    texDesc.SampleDesc.Count = m_desc.sampleCount;
    texDesc.SampleDesc.Quality = m_desc.sampleQuality;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &texture);
    if (FAILED(hr)) return hr;
    
    m_resource = texture;
    
    hr = CreateViews(device);
    if (SUCCEEDED(hr)) {
        m_loaded = true;
        m_memoryUsage = static_cast<size_t>(m_desc.width * m_desc.height * 4);
    }
    
    return hr;
}

void Texture::Release()
{
    m_srv.Reset();
    m_rtv.Reset();
    m_dsv.Reset();
    m_uav.Reset();
    m_resource.Reset();
    m_loaded = false;
    m_memoryUsage = 0;
}

void Texture::Bind(ID3D11DeviceContext* context, uint32_t slot)
{
    ASSERT(context);
    if (m_srv) {
        context->PSSetShaderResources(slot, 1, m_srv.GetAddressOf());
    }
}

void Texture::UnBind(ID3D11DeviceContext* context, uint32_t slot)
{
    ASSERT(context);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(slot, 1, &nullSRV);
}

HRESULT Texture::CreateViews(ID3D11Device* device)
{
    ASSERT(device && m_resource);
    
    HRESULT hr = S_OK;
    
    // Create SRV
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::ShaderResource)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = GetDXGIFormat(m_desc.format);
        
        if (m_desc.type == TextureType::TextureCube) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = m_desc.mipLevels;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = m_desc.mipLevels;
        }
        
        hr = device->CreateShaderResourceView(m_resource.Get(), &srvDesc, &m_srv);
        if (FAILED(hr)) return hr;
    }
    
    // Create RTV
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::RenderTarget)) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = GetDXGIFormat(m_desc.format);
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        
        hr = device->CreateRenderTargetView(m_resource.Get(), &rtvDesc, &m_rtv);
        if (FAILED(hr)) return hr;
    }
    
    // Create DSV
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::DepthStencil)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        
        hr = device->CreateDepthStencilView(m_resource.Get(), &dsvDesc, &m_dsv);
        if (FAILED(hr)) return hr;
    }
    
    // Create UAV
    if (static_cast<uint32_t>(m_desc.usage) & static_cast<uint32_t>(TextureUsage::UnorderedAccess)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = GetDXGIFormat(m_desc.format);
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        
        hr = device->CreateUnorderedAccessView(m_resource.Get(), &uavDesc, &m_uav);
        if (FAILED(hr)) return hr;
    }
    
    return S_OK;
}

DXGI_FORMAT Texture::GetDXGIFormat(TextureFormat format) const
{
    switch (format) {
        case TextureFormat::R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case TextureFormat::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC1_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case TextureFormat::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC3_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case TextureFormat::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case TextureFormat::BC7_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case TextureFormat::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// ============================================================================
// TEXTURE SYSTEM IMPLEMENTATION
// ============================================================================

TextureSystem::TextureSystem()
    : m_device(nullptr), m_context(nullptr)
{
}

TextureSystem::~TextureSystem()
{
    Shutdown();
}

HRESULT TextureSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT(device && context);
    
    m_device = device;
    m_context = context;
    
    // Initialize metrics
    memset(&m_metrics, 0, sizeof(m_metrics));
    
    // Create default textures
    HRESULT hr = CreateDefaultTextures();
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create default textures");
        return hr;
    }
    
    // Start streaming threads
    SetStreamingThreadCount(2);
    
    Spark::SimpleConsole::GetInstance().LogSuccess("TextureSystem initialized successfully");
    return S_OK;
}

void TextureSystem::Shutdown()
{
    // Stop streaming threads
    m_shouldStop = true;
    m_streamingCondition.notify_all();
    
    for (auto& thread : m_streamingThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Clear all textures
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures.clear();
    }
    
    m_whiteTexture.reset();
    m_blackTexture.reset();
    m_normalTexture.reset();
    m_noiseTexture.reset();
    
    m_device = nullptr;
    m_context = nullptr;
    
    Spark::SimpleConsole::GetInstance().LogInfo("TextureSystem shutdown complete");
}

void TextureSystem::Update(float deltaTime)
{
    UpdateMetrics();
    
    // Check memory budget and garbage collect if needed
    if (GetMemoryUsage() > m_memoryBudget) {
        GarbageCollect();
    }
}

std::shared_ptr<Texture> TextureSystem::LoadTexture(const std::string& filePath, const TextureDesc& desc)
{
    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        auto it = m_textures.find(filePath);
        if (it != m_textures.end()) {
            return it->second;
        }
    }
    
    // Adjust description for quality settings
    TextureDesc adjustedDesc = AdjustDescForQuality(desc);
    
    // Load the texture
    auto texture = LoadTextureFromFile(filePath, adjustedDesc);
    if (texture) {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures[filePath] = texture;
        
        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        m_metrics.loadedTextures++;
    }
    
    return texture;
}

std::shared_ptr<Texture> TextureSystem::CreateTexture(const std::string& name, const TextureDesc& desc)
{
    auto texture = std::make_shared<Texture>(name, desc);
    
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        m_textures[name] = texture;
    }
    
    return texture;
}

void TextureSystem::LoadTextureAsync(const std::string& filePath, 
                                    std::function<void(std::shared_ptr<Texture>)> callback,
                                    const TextureDesc& desc)
{
    // Check if already loaded
    {
        std::lock_guard<std::mutex> lock(m_texturesMutex);
        auto it = m_textures.find(filePath);
        if (it != m_textures.end()) {
            if (callback) {
                callback(it->second);
            }
            return;
        }
    }
    
    // Queue for streaming
    StreamingRequest request;
    request.filePath = filePath;
    request.desc = AdjustDescForQuality(desc);
    request.callback = callback;
    request.priority = 0;
    request.urgent = false;
    
    {
        std::lock_guard<std::mutex> lock(m_streamingMutex);
        m_streamingQueue.push(request);
    }
    
    m_streamingCondition.notify_one();
}

std::shared_ptr<Texture> TextureSystem::GetTexture(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_textures.find(name);
    return (it != m_textures.end()) ? it->second : nullptr;
}

void TextureSystem::UnloadTexture(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        m_textures.erase(it);
        
        std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
        m_metrics.loadedTextures--;
    }
}

void TextureSystem::UnloadAllTextures()
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    m_textures.clear();
    
    std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
    m_metrics.loadedTextures = 0;
}

size_t TextureSystem::GetMemoryUsage() const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    size_t totalMemory = 0;
    
    for (const auto& pair : m_textures) {
        totalMemory += pair.second->GetMemoryUsage();
    }
    
    return totalMemory;
}

void TextureSystem::GarbageCollect()
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    
    // Remove textures that are only referenced by the texture system
    auto it = m_textures.begin();
    while (it != m_textures.end()) {
        if (it->second.use_count() == 1) {
            Spark::SimpleConsole::GetInstance().LogInfo("Garbage collecting texture: " + it->first);
            it = m_textures.erase(it);
        } else {
            ++it;
        }
    }
}

void TextureSystem::SetStreamingThreadCount(int count)
{
    // Stop existing threads
    m_shouldStop = true;
    m_streamingCondition.notify_all();
    
    for (auto& thread : m_streamingThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    m_streamingThreads.clear();
    m_shouldStop = false;
    
    // Start new threads
    for (int i = 0; i < count; ++i) {
        m_streamingThreads.emplace_back(&TextureSystem::StreamingThreadFunction, this);
    }
}

// Console integration methods
TextureSystem::TextureMetrics TextureSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string TextureSystem::Console_ListTextures() const
{
    std::lock_guard<std::mutex> lock(m_texturesMutex);
    std::stringstream ss;
    
    ss << "=== Loaded Textures (" << m_textures.size() << ") ===\n";
    for (const auto& pair : m_textures) {
        const auto& texture = pair.second;
        ss << pair.first << " - " << texture->GetDesc().width << "x" << texture->GetDesc().height;
        ss << " (" << (texture->GetMemoryUsage() / 1024) << " KB)\n";
    }
    
    return ss.str();
}

std::string TextureSystem::Console_GetTextureInfo(const std::string& name) const
{
    auto texture = GetTexture(name);
    if (!texture) {
        return "Texture not found: " + name;
    }
    
    const auto& desc = texture->GetDesc();
    std::stringstream ss;
    
    ss << "=== Texture Info: " << name << " ===\n";
    ss << "Dimensions: " << desc.width << "x" << desc.height << "x" << desc.depth << "\n";
    ss << "Mip Levels: " << desc.mipLevels << "\n";
    ss << "Array Size: " << desc.arraySize << "\n";
    ss << "Memory Usage: " << (texture->GetMemoryUsage() / 1024) << " KB\n";
    ss << "Loaded: " << (texture->IsLoaded() ? "Yes" : "No") << "\n";
    ss << "Streaming: " << (texture->IsStreaming() ? "Yes" : "No") << "\n";
    
    return ss.str();
}

void TextureSystem::Console_SetQuality(const std::string& quality)
{
    if (quality == "low") {
        SetTextureQuality(TextureQuality::Low);
    } else if (quality == "medium") {
        SetTextureQuality(TextureQuality::Medium);
    } else if (quality == "high") {
        SetTextureQuality(TextureQuality::High);
    } else if (quality == "ultra") {
        SetTextureQuality(TextureQuality::Ultra);
    }
    
    Spark::SimpleConsole::GetInstance().LogSuccess("Texture quality set to: " + quality);
}

void TextureSystem::Console_SetMemoryBudget(size_t budgetMB)
{
    SetMemoryBudget(budgetMB * 1024 * 1024);
    Spark::SimpleConsole::GetInstance().LogSuccess("Texture memory budget set to: " + std::to_string(budgetMB) + " MB");
}

void TextureSystem::Console_ForceGC()
{
    size_t beforeMemory = GetMemoryUsage();
    GarbageCollect();
    size_t afterMemory = GetMemoryUsage();
    
    Spark::SimpleConsole::GetInstance().LogSuccess("Garbage collection freed: " + 
        std::to_string((beforeMemory - afterMemory) / 1024) + " KB");
}

void TextureSystem::Console_ReloadTexture(const std::string& name)
{
    UnloadTexture(name);
    // Note: Texture will be reloaded on next access
    Spark::SimpleConsole::GetInstance().LogSuccess("Marked texture for reload: " + name);
}

void TextureSystem::Console_ReloadAllTextures()
{
    UnloadAllTextures();
    Spark::SimpleConsole::GetInstance().LogSuccess("Marked all textures for reload");
}

// Private helper methods
HRESULT TextureSystem::CreateDefaultTextures()
{
    // Create white texture (1x1 white pixel)
    {
        uint32_t whitePixel = 0xFFFFFFFF;
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;
        
        m_whiteTexture = std::make_shared<Texture>("__white", desc);
        HRESULT hr = m_whiteTexture->CreateFromData(&whitePixel, 4, m_device);
        if (FAILED(hr)) return hr;
    }
    
    // Create black texture (1x1 black pixel)
    {
        uint32_t blackPixel = 0xFF000000;
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;
        
        m_blackTexture = std::make_shared<Texture>("__black", desc);
        HRESULT hr = m_blackTexture->CreateFromData(&blackPixel, 4, m_device);
        if (FAILED(hr)) return hr;
    }
    
    // Create default normal texture (1x1 normal pointing up)
    {
        uint32_t normalPixel = 0xFFFF8080; // (0.5, 0.5, 1.0) in normal map format
        TextureDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;
        
        m_normalTexture = std::make_shared<Texture>("__normal", desc);
        HRESULT hr = m_normalTexture->CreateFromData(&normalPixel, 4, m_device);
        if (FAILED(hr)) return hr;
    }
    
    // Create noise texture (64x64 random noise)
    {
        const int noiseSize = 64;
        std::vector<uint32_t> noiseData(noiseSize * noiseSize);
        
        for (int i = 0; i < noiseSize * noiseSize; ++i) {
            uint8_t r = rand() % 256;
            uint8_t g = rand() % 256;
            uint8_t b = rand() % 256;
            noiseData[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
        
        TextureDesc desc;
        desc.width = noiseSize;
        desc.height = noiseSize;
        desc.format = TextureFormat::R8G8B8A8_UNORM;
        desc.usage = TextureUsage::ShaderResource;
        
        m_noiseTexture = std::make_shared<Texture>("__noise", desc);
        HRESULT hr = m_noiseTexture->CreateFromData(noiseData.data(), noiseData.size() * 4, m_device);
        if (FAILED(hr)) return hr;
    }
    
    return S_OK;
}

void TextureSystem::StreamingThreadFunction()
{
    while (!m_shouldStop) {
        StreamingRequest request;
        bool hasRequest = false;
        
        // Get next request
        {
            std::unique_lock<std::mutex> lock(m_streamingMutex);
            m_streamingCondition.wait(lock, [this] { return !m_streamingQueue.empty() || m_shouldStop; });
            
            if (m_shouldStop) break;
            
            if (!m_streamingQueue.empty()) {
                request = m_streamingQueue.front();
                m_streamingQueue.pop();
                hasRequest = true;
            }
        }
        
        if (hasRequest) {
            // Load texture
            auto texture = LoadTextureFromFile(request.filePath, request.desc);
            
            if (texture) {
                // Add to cache
                {
                    std::lock_guard<std::mutex> lock(m_texturesMutex);
                    m_textures[request.filePath] = texture;
                }
                
                // Call callback
                if (request.callback) {
                    request.callback(texture);
                }
                
                std::lock_guard<std::mutex> metricsLock(m_metricsMutex);
                m_metrics.loadedTextures++;
            } else {
                // Call callback with null on failure
                if (request.callback) {
                    request.callback(nullptr);
                }
            }
        }
    }
}

void TextureSystem::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    
    m_metrics.totalMemoryUsage = GetMemoryUsage();
    // Update other metrics as needed
}

TextureDesc TextureSystem::AdjustDescForQuality(const TextureDesc& desc) const
{
    TextureDesc adjustedDesc = desc;
    
    switch (m_quality) {
        case TextureQuality::Low:
            adjustedDesc.width = std::max(1u, desc.width / 4);
            adjustedDesc.height = std::max(1u, desc.height / 4);
            break;
        case TextureQuality::Medium:
            adjustedDesc.width = std::max(1u, desc.width / 2);
            adjustedDesc.height = std::max(1u, desc.height / 2);
            break;
        case TextureQuality::High:
        case TextureQuality::Ultra:
        default:
            // No changes for high/ultra quality
            break;
    }
    
    return adjustedDesc;
}

std::shared_ptr<Texture> TextureSystem::LoadTextureFromFile(const std::string& filePath, const TextureDesc& desc)
{
    if (!std::filesystem::exists(filePath)) {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return nullptr;
    }
    
    auto texture = std::make_shared<Texture>(filePath, desc);
    HRESULT hr = texture->CreateFromFile(filePath, m_device);
    
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + filePath);
        return nullptr;
    }
    
    return texture;
}

// Utility functions
TextureFormat GetOptimalFormat(const std::string& filePath, bool sRGB)
{
    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".dds") {
        return sRGB ? TextureFormat::BC7_SRGB : TextureFormat::BC7_UNORM;
    } else if (ext == ".hdr") {
        return TextureFormat::R16G16B16A16_FLOAT;
    } else {
        return sRGB ? TextureFormat::R8G8B8A8_SRGB : TextureFormat::R8G8B8A8_UNORM;
    }
}

bool IsCompressedFormat(TextureFormat format)
{
    return format == TextureFormat::BC1_UNORM || format == TextureFormat::BC1_SRGB ||
           format == TextureFormat::BC3_UNORM || format == TextureFormat::BC3_SRGB ||
           format == TextureFormat::BC7_UNORM || format == TextureFormat::BC7_SRGB;
}

uint32_t GetFormatBytesPerPixel(TextureFormat format)
{
    switch (format) {
        case TextureFormat::R8G8B8A8_UNORM:
        case TextureFormat::R8G8B8A8_SRGB:
            return 4;
        case TextureFormat::R16G16B16A16_FLOAT:
            return 8;
        case TextureFormat::R32G32B32A32_FLOAT:
            return 16;
        case TextureFormat::R16_FLOAT:
            return 2;
        case TextureFormat::R32_FLOAT:
            return 4;
        default:
            return 4; // Default to 4 bytes per pixel
    }
}
#endif // SPARK_PLATFORM_WINDOWS
