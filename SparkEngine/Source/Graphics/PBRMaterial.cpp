#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file PBRMaterial.cpp
 * @brief PBR Material class implementation
 *
 * Contains all Material:: methods for PBR material data management,
 * texture binding, variant handling, and serialization.
 * Split from MaterialSystem.cpp for maintainability.
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <cstring>

#ifdef SPARK_PLATFORM_WINDOWS
#include <wincodec.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <wincodecsdk.h>

#ifdef SPARK_PLATFORM_WINDOWS

// ============================================================================
// MATERIAL CLASS IMPLEMENTATION
// ============================================================================

Material::Material(const std::string& name) : m_name(name)
{
    m_pbrProperties = {};
    m_advancedProperties = {};
    m_renderState = {};
    m_variants = {};
    m_activeVariant = "";
}

const MaterialTexture& Material::GetTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    if (it != m_textures.end())
    {
        return it->second;
    }

    Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have texture of type " +
                                                   std::to_string(static_cast<int>(type)));

    // Return a default empty texture if not found
    static MaterialTexture emptyTexture;
    return emptyTexture;
}

void Material::SetTexture(MaterialTextureType type, const MaterialTexture& texture)
{
    m_textures[type] = texture;
}

bool Material::LoadTexture(MaterialTextureType type, const std::string& filePath, ID3D11Device* device)
{
    if (!device)
    {
        Spark::SimpleConsole::GetInstance().LogError("Device is null");
        return false;
    }

    if (filePath.empty())
    {
        Spark::SimpleConsole::GetInstance().LogError("File path is empty");
        return false;
    }

    if (!std::filesystem::exists(filePath))
    {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return false;
    }

    // Check if texture already loaded
    if (m_textures.find(type) != m_textures.end())
    {
        Spark::SimpleConsole::GetInstance().LogInfo("Texture of type " + std::to_string(static_cast<int>(type)) +
                                                    " already loaded for material '" + m_name + "'");
        return true;
    }

    // Load texture using WIC
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Imaging Factory");
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(std::wstring(filePath.begin(), filePath.end()).c_str(), nullptr,
                                               GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Decoder for file: " + filePath);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to get frame from WIC Decoder for file: " + filePath);
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Format Converter");
        return false;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to initialize WIC Format Converter");
        return false;
    }

    UINT width, height;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to get image size from WIC Converter");
        return false;
    }

    std::vector<BYTE> imageData(width * height * 4); // 4 bytes per pixel (RGBA)
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(imageData.size()), imageData.data());
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to copy pixels from WIC Converter");
        return false;
    }

    // Create Direct3D texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = imageData.data();
    initData.SysMemPitch = width * 4;

    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create Direct3D texture for file: " + filePath);
        return false;
    }

    // Create shader resource view
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create Shader Resource View for texture: " + filePath);
        return false;
    }

    // Store texture
    MaterialTexture matTexture;
    matTexture.texture = srv;
    matTexture.filePath = filePath;
    matTexture.enabled = true;
    m_textures[type] = matTexture;

    Spark::SimpleConsole::GetInstance().LogInfo("Loaded texture: " + filePath + " for material '" + m_name + "'");
    return true;
}

void Material::UnloadTexture(MaterialTextureType type)
{
    auto it = m_textures.find(type);
    if (it != m_textures.end())
    {
        m_textures.erase(it);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have texture of type " +
                                                       std::to_string(static_cast<int>(type)) + " to unload");
    }
}

bool Material::HasTexture(MaterialTextureType type) const
{
    return m_textures.find(type) != m_textures.end();
}

void Material::BindToShader(ID3D11DeviceContext* context) const
{
    if (!context)
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Null context in Material::BindToShader for material: " +
                                                       m_name);
        return;
    }

    // Define texture slot mapping for consistent shader binding
    static const std::unordered_map<MaterialTextureType, UINT> textureSlotMapping = {
        {MaterialTextureType::Albedo, 0},
        {MaterialTextureType::Normal, 1},
        {MaterialTextureType::Metallic, 2},
        {MaterialTextureType::Roughness, 3},
        {MaterialTextureType::Occlusion, 4},
        {MaterialTextureType::Emissive, 5},
        {MaterialTextureType::Height, 6},
        {MaterialTextureType::DetailAlbedo, 7},
        {MaterialTextureType::DetailNormal, 8},
        {MaterialTextureType::Subsurface, 9},
        {MaterialTextureType::Transmission, 10},
        {MaterialTextureType::Clearcoat, 11},
        {MaterialTextureType::ClearcoatRoughness, 12},
        {MaterialTextureType::Anisotropy, 13},
        {MaterialTextureType::Custom0, 14},
        {MaterialTextureType::Custom1, 15},
        {MaterialTextureType::Custom2, 16},
        {MaterialTextureType::Custom3, 17}};

    // Bind material textures to their designated slots
    std::vector<ID3D11ShaderResourceView*> srvArray(18, nullptr); // Max 18 texture slots
    std::vector<ID3D11SamplerState*> samplerArray(18, nullptr);

    int boundTextures = 0;
    for (const auto& texturePair : m_textures)
    {
        MaterialTextureType type = texturePair.first;
        const MaterialTexture& matTexture = texturePair.second;

        auto slotIt = textureSlotMapping.find(type);
        if (slotIt == textureSlotMapping.end())
        {
            continue; // Unknown texture type
        }

        UINT slot = slotIt->second;
        if (slot >= 18)
            continue; // Safety check

        if (matTexture.enabled && matTexture.texture)
        {
            srvArray[slot] = matTexture.texture.Get();

            // For now, we'll use a default sampler since we don't have access to MaterialSystem here
            // In a full implementation, you'd pass the sampler or get it from a global manager
            boundTextures++;
        }
    }

    // Bind all textures at once for efficiency
    if (boundTextures > 0)
    {
        context->PSSetShaderResources(0, 18, srvArray.data());

        // Note: Samplers would also be bound here if we had access to them
        // context->PSSetSamplers(0, 18, samplerArray.data());
    }

    // Bind the material constant buffer if compiled
    if (m_constantBuffer)
    {
        // Update constant buffer contents
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            MaterialConstants constants = {};
            constants.albedoColor = m_pbrProperties.albedoColor;
            constants.metallicFactor = m_pbrProperties.metallicFactor;
            constants.roughnessFactor = m_pbrProperties.roughnessFactor;
            constants.normalScale = m_pbrProperties.normalScale;
            constants.occlusionStrength = m_pbrProperties.occlusionStrength;
            constants.emissiveColor = m_pbrProperties.emissiveColor;
            constants.emissiveFactor = m_pbrProperties.emissiveFactor;
            constants.alphaCutoff = m_pbrProperties.alphaCutoff;
            constants.indexOfRefraction = m_pbrProperties.indexOfRefraction;
            constants.pad0 = 0.0f;
            constants.pad1 = 0.0f;
            std::memcpy(mapped.pData, &constants, sizeof(MaterialConstants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }

        ID3D11Buffer* cbuffers[] = {m_constantBuffer.Get()};
        context->PSSetConstantBuffers(1, 1, cbuffers);
    }

    // Set pipeline states if compiled
    if (m_blendState)
    {
        const FLOAT blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    }

    if (m_depthStencilState)
    {
        context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
    }

    if (m_rasterizerState)
    {
        context->RSSetState(m_rasterizerState.Get());
    }

// Log binding for debugging in debug builds
#ifdef _DEBUG
    static int bindCount = 0;
    if (++bindCount % 100 == 0)
    { // Log every 100 binds to avoid spam
        Spark::SimpleConsole::GetInstance().LogInfo("Material '" + m_name + "' bound with " +
                                                    std::to_string(boundTextures) + " textures");
    }
#endif
}

void Material::CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
{
    m_variants[variantName] = defines;
}

void Material::SetActiveVariant(const std::string& variantName)
{
    if (m_variants.find(variantName) != m_variants.end())
    {
        m_activeVariant = variantName;
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have variant '" +
                                                       variantName + "'");
    }
}

bool Material::SaveToFile(const std::string& filePath) const
{
    try
    {
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            Spark::SimpleConsole::GetInstance().LogError("Cannot open file for writing: " + filePath);
            return false;
        }

        if (m_name.empty())
        {
            Spark::SimpleConsole::GetInstance().LogError("Cannot save material with empty name");
            return false;
        }

        // Write file header with version for future compatibility
        file << "# Spark Engine Material File\n";
        file << "# Version: 1.0\n";
        file << "# Generated: "
             << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count()
             << "\n";
        file << "\n";

        // Write basic material info
        file << "[Material]\n";
        file << "Name=" << m_name << "\n";
        file << "ActiveVariant=" << m_activeVariant << "\n";
        file << "\n";

        // Write PBR properties with full precision
        file << "[PBR]\n";
        file << "AlbedoColor=" << m_pbrProperties.albedoColor.x << "," << m_pbrProperties.albedoColor.y << ","
             << m_pbrProperties.albedoColor.z << "," << m_pbrProperties.albedoColor.w << "\n";
        file << "MetallicFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.metallicFactor << "\n";
        file << "RoughnessFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.roughnessFactor << "\n";
        file << "NormalScale=" << std::fixed << std::setprecision(6) << m_pbrProperties.normalScale << "\n";
        file << "OcclusionStrength=" << std::fixed << std::setprecision(6) << m_pbrProperties.occlusionStrength << "\n";
        file << "EmissiveColor=" << m_pbrProperties.emissiveColor.x << "," << m_pbrProperties.emissiveColor.y << ","
             << m_pbrProperties.emissiveColor.z << "\n";
        file << "EmissiveFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.emissiveFactor << "\n";
        file << "AlphaCutoff=" << std::fixed << std::setprecision(6) << m_pbrProperties.alphaCutoff << "\n";
        file << "IndexOfRefraction=" << std::fixed << std::setprecision(6) << m_pbrProperties.indexOfRefraction << "\n";
        file << "\n";

        // Write advanced properties
        file << "[Advanced]\n";
        file << "SubsurfaceEnabled=" << (m_advancedProperties.subsurfaceEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.subsurfaceEnabled)
        {
            file << "SubsurfaceColor=" << m_advancedProperties.subsurfaceColor.x << ","
                 << m_advancedProperties.subsurfaceColor.y << "," << m_advancedProperties.subsurfaceColor.z << "\n";
            file << "SubsurfaceRadius=" << m_advancedProperties.subsurfaceRadius << "\n";
        }

        file << "ClearcoatEnabled=" << (m_advancedProperties.clearcoatEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.clearcoatEnabled)
        {
            file << "ClearcoatFactor=" << m_advancedProperties.clearcoatFactor << "\n";
            file << "ClearcoatRoughness=" << m_advancedProperties.clearcoatRoughness << "\n";
        }

        file << "AnisotropyEnabled=" << (m_advancedProperties.anisotropyEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.anisotropyEnabled)
        {
            file << "AnisotropyFactor=" << m_advancedProperties.anisotropyFactor << "\n";
            file << "AnisotropyDirection=" << m_advancedProperties.anisotropyDirection.x << ","
                 << m_advancedProperties.anisotropyDirection.y << "\n";
        }

        file << "TransmissionEnabled=" << (m_advancedProperties.transmissionEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.transmissionEnabled)
        {
            file << "TransmissionFactor=" << m_advancedProperties.transmissionFactor << "\n";
            file << "TransmissionColor=" << m_advancedProperties.transmissionColor.x << ","
                 << m_advancedProperties.transmissionColor.y << "," << m_advancedProperties.transmissionColor.z << "\n";
        }

        file << "SheenEnabled=" << (m_advancedProperties.sheenEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.sheenEnabled)
        {
            file << "SheenColor=" << m_advancedProperties.sheenColor.x << "," << m_advancedProperties.sheenColor.y
                 << "," << m_advancedProperties.sheenColor.z << "\n";
            file << "SheenRoughness=" << m_advancedProperties.sheenRoughness << "\n";
        }

        file << "IridescenceEnabled=" << (m_advancedProperties.iridescenceEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.iridescenceEnabled)
        {
            file << "IridescenceFactor=" << m_advancedProperties.iridescenceFactor << "\n";
            file << "IridescenceIOR=" << m_advancedProperties.iridescenceIOR << "\n";
            file << "IridescenceThickness=" << m_advancedProperties.iridescenceThickness << "\n";
        }
        file << "\n";

        // Write render state
        file << "[RenderState]\n";
        file << "BlendMode=" << static_cast<int>(m_renderState.blendMode) << "\n";
        file << "CullMode=" << static_cast<int>(m_renderState.cullMode) << "\n";
        file << "DepthTest=" << (m_renderState.depthTest ? "true" : "false") << "\n";
        file << "DepthWrite=" << (m_renderState.depthWrite ? "true" : "false") << "\n";
        file << "CastShadows=" << (m_renderState.castShadows ? "true" : "false") << "\n";
        file << "ReceiveShadows=" << (m_renderState.receiveShadows ? "true" : "false") << "\n";
        file << "RenderQueue=" << m_renderState.renderQueue << "\n";
        file << "DoubleSided=" << (m_renderState.doubleSided ? "true" : "false") << "\n";
        file << "\n";

        // Write textures with full parameters
        file << "[Textures]\n";
        for (const auto& pair : m_textures)
        {
            if (!pair.second.filePath.empty())
            {
                file << "Texture" << static_cast<int>(pair.first) << "=" << pair.second.filePath << "\n";
                file << "Texture" << static_cast<int>(pair.first)
                     << "_Enabled=" << (pair.second.enabled ? "true" : "false") << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Intensity=" << pair.second.intensity << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Tiling=" << pair.second.tiling.x << ","
                     << pair.second.tiling.y << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Offset=" << pair.second.offset.x << ","
                     << pair.second.offset.y << "\n";
            }
        }
        file << "\n";

        // Write material variants
        if (!m_variants.empty())
        {
            file << "[Variants]\n";
            for (const auto& variantPair : m_variants)
            {
                file << "Variant_" << variantPair.first << "=";
                for (size_t i = 0; i < variantPair.second.size(); ++i)
                {
                    if (i > 0)
                        file << ",";
                    file << variantPair.second[i];
                }
                file << "\n";
            }
            file << "\n";
        }

        file.close();

        if (m_fileCache)
        {
            m_fileCache->Invalidate(filePath);
        }

        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' saved to: " + filePath);
        return true;
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Exception while saving material '" + m_name +
                                                     "': " + std::string(e.what()));
        return false;
    }
}

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* device)
{
    try
    {
        std::string fileContent;

        if (m_fileCache)
        {
            auto result = m_fileCache->ReadText(filePath);
            if (result.IsOk())
            {
                fileContent = result.Value();
            }
        }

        if (fileContent.empty())
        {
            std::ifstream file(filePath);
            if (!file.is_open())
            {
                Spark::SimpleConsole::GetInstance().LogError("Cannot open file for reading: " + filePath);
                return false;
            }
            fileContent.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        std::istringstream fileStream(fileContent);
        std::string line;
        std::string currentSection = "";
        int lineNumber = 0;

        // Helper lambda to safely parse a single float value
        auto safeStof = [](const std::string& value, float defaultVal = 0.0f) -> float
        {
            try
            {
                return std::stof(value);
            }
            catch (const std::exception&)
            {
                return defaultVal;
            }
        };

        // Helper lambda to parse comma-separated values
        auto parseFloatArray = [](const std::string& value, std::vector<float>& output)
        {
            output.clear();
            std::stringstream ss(value);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                try
                {
                    output.push_back(std::stof(item));
                }
                catch (const std::exception&)
                {
                    return false;
                }
            }
            return true;
        };

        // Helper lambda to parse boolean values
        auto parseBool = [](const std::string& value) -> bool
        { return value == "true" || value == "1" || value == "yes"; };

        // Helper lambda to trim whitespace
        auto trim = [](const std::string& str) -> std::string
        {
            size_t start = str.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return "";
            size_t end = str.find_last_not_of(" \t\r\n");
            return str.substr(start, end - start + 1);
        };

        while (std::getline(fileStream, line))
        {
            lineNumber++;
            line = trim(line);

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            // Check for section headers
            if (line[0] == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

            // Parse key=value pairs
            size_t equalPos = line.find('=');
            if (equalPos == std::string::npos)
            {
                Spark::SimpleConsole::GetInstance().LogWarning("Invalid line format at line " +
                                                               std::to_string(lineNumber) + " in: " + filePath);
                continue;
            }

            std::string key = trim(line.substr(0, equalPos));
            std::string value = trim(line.substr(equalPos + 1));

            try
            {
                // Parse based on current section
                if (currentSection == "Material")
                {
                    if (key == "Name")
                    {
                        m_name = value;
                    }
                    else if (key == "ActiveVariant")
                    {
                        m_activeVariant = value;
                    }
                }
                else if (currentSection == "PBR")
                {
                    if (key == "AlbedoColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_pbrProperties.albedoColor.x = components[0];
                            m_pbrProperties.albedoColor.y = components[1];
                            m_pbrProperties.albedoColor.z = components[2];
                            m_pbrProperties.albedoColor.w = components.size() > 3 ? components[3] : 1.0f;
                        }
                    }
                    else if (key == "MetallicFactor")
                    {
                        m_pbrProperties.metallicFactor = safeStof(value);
                    }
                    else if (key == "RoughnessFactor")
                    {
                        m_pbrProperties.roughnessFactor = safeStof(value);
                    }
                    else if (key == "NormalScale")
                    {
                        m_pbrProperties.normalScale = safeStof(value);
                    }
                    else if (key == "OcclusionStrength")
                    {
                        m_pbrProperties.occlusionStrength = safeStof(value);
                    }
                    else if (key == "EmissiveColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_pbrProperties.emissiveColor.x = components[0];
                            m_pbrProperties.emissiveColor.y = components[1];
                            m_pbrProperties.emissiveColor.z = components[2];
                        }
                    }
                    else if (key == "EmissiveFactor")
                    {
                        m_pbrProperties.emissiveFactor = safeStof(value);
                    }
                    else if (key == "AlphaCutoff")
                    {
                        m_pbrProperties.alphaCutoff = safeStof(value);
                    }
                    else if (key == "IndexOfRefraction")
                    {
                        m_pbrProperties.indexOfRefraction = safeStof(value);
                    }
                }
                else if (currentSection == "Advanced")
                {
                    if (key == "SubsurfaceEnabled")
                    {
                        m_advancedProperties.subsurfaceEnabled = parseBool(value);
                    }
                    else if (key == "SubsurfaceColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_advancedProperties.subsurfaceColor.x = components[0];
                            m_advancedProperties.subsurfaceColor.y = components[1];
                            m_advancedProperties.subsurfaceColor.z = components[2];
                        }
                    }
                    else if (key == "SubsurfaceRadius")
                    {
                        m_advancedProperties.subsurfaceRadius = safeStof(value);
                    }
                    else if (key == "ClearcoatEnabled")
                    {
                        m_advancedProperties.clearcoatEnabled = parseBool(value);
                    }
                    else if (key == "ClearcoatFactor")
                    {
                        m_advancedProperties.clearcoatFactor = safeStof(value);
                    }
                    else if (key == "ClearcoatRoughness")
                    {
                        m_advancedProperties.clearcoatRoughness = safeStof(value);
                    }
                    else if (key == "AnisotropyEnabled")
                    {
                        m_advancedProperties.anisotropyEnabled = parseBool(value);
                    }
                    else if (key == "AnisotropyFactor")
                    {
                        m_advancedProperties.anisotropyFactor = safeStof(value);
                    }
                    else if (key == "AnisotropyDirection")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 2)
                        {
                            m_advancedProperties.anisotropyDirection.x = components[0];
                            m_advancedProperties.anisotropyDirection.y = components[1];
                        }
                    }
                    else if (key == "TransmissionEnabled")
                    {
                        m_advancedProperties.transmissionEnabled = parseBool(value);
                    }
                    else if (key == "TransmissionFactor")
                    {
                        m_advancedProperties.transmissionFactor = safeStof(value);
                    }
                    else if (key == "TransmissionColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_advancedProperties.transmissionColor.x = components[0];
                            m_advancedProperties.transmissionColor.y = components[1];
                            m_advancedProperties.transmissionColor.z = components[2];
                        }
                    }
                    else if (key == "SheenEnabled")
                    {
                        m_advancedProperties.sheenEnabled = parseBool(value);
                    }
                    else if (key == "SheenColor")
                    {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3)
                        {
                            m_advancedProperties.sheenColor.x = components[0];
                            m_advancedProperties.sheenColor.y = components[1];
                            m_advancedProperties.sheenColor.z = components[2];
                        }
                    }
                    else if (key == "SheenRoughness")
                    {
                        m_advancedProperties.sheenRoughness = safeStof(value);
                    }
                    else if (key == "IridescenceEnabled")
                    {
                        m_advancedProperties.iridescenceEnabled = parseBool(value);
                    }
                    else if (key == "IridescenceFactor")
                    {
                        m_advancedProperties.iridescenceFactor = safeStof(value);
                    }
                    else if (key == "IridescenceIOR")
                    {
                        m_advancedProperties.iridescenceIOR = safeStof(value);
                    }
                    else if (key == "IridescenceThickness")
                    {
                        m_advancedProperties.iridescenceThickness = safeStof(value);
                    }
                }
                else if (currentSection == "RenderState")
                {
                    if (key == "BlendMode")
                    {
                        m_renderState.blendMode = static_cast<BlendMode>(std::stoi(value));
                    }
                    else if (key == "CullMode")
                    {
                        m_renderState.cullMode = static_cast<CullMode>(std::stoi(value));
                    }
                    else if (key == "DepthTest")
                    {
                        m_renderState.depthTest = parseBool(value);
                    }
                    else if (key == "DepthWrite")
                    {
                        m_renderState.depthWrite = parseBool(value);
                    }
                    else if (key == "CastShadows")
                    {
                        m_renderState.castShadows = parseBool(value);
                    }
                    else if (key == "ReceiveShadows")
                    {
                        m_renderState.receiveShadows = parseBool(value);
                    }
                    else if (key == "RenderQueue")
                    {
                        m_renderState.renderQueue = std::stoi(value);
                    }
                    else if (key == "DoubleSided")
                    {
                        m_renderState.doubleSided = parseBool(value);
                    }
                }
                else if (currentSection == "Textures")
                {
                    // Parse texture entries
                    if (key.substr(0, 7) == "Texture" && !key.contains("_"))
                    {
                        // Main texture path
                        int textureType = std::stoi(key.substr(7));
                        LoadTexture(static_cast<MaterialTextureType>(textureType), value, device);
                    }
                    // Parse texture properties (enabled, intensity, tiling, offset)
                    else if (key.contains("_Enabled"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Enabled"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                it->second.enabled = parseBool(value);
                            }
                        }
                    }
                    else if (key.contains("_Intensity"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Intensity"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                it->second.intensity = safeStof(value);
                            }
                        }
                    }
                    else if (key.contains("_Tiling"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Tiling"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                std::vector<float> components;
                                if (parseFloatArray(value, components) && components.size() >= 2)
                                {
                                    it->second.tiling.x = components[0];
                                    it->second.tiling.y = components[1];
                                }
                            }
                        }
                    }
                    else if (key.contains("_Offset"))
                    {
                        std::string baseKey = key.substr(0, key.find("_Offset"));
                        if (baseKey.substr(0, 7) == "Texture")
                        {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end())
                            {
                                std::vector<float> components;
                                if (parseFloatArray(value, components) && components.size() >= 2)
                                {
                                    it->second.offset.x = components[0];
                                    it->second.offset.y = components[1];
                                }
                            }
                        }
                    }
                }
                else if (currentSection == "Variants")
                {
                    if (key.substr(0, 8) == "Variant_")
                    {
                        std::string variantName = key.substr(8);
                        std::vector<std::string> defines;
                        std::stringstream ss(value);
                        std::string define;
                        while (std::getline(ss, define, ','))
                        {
                            defines.push_back(trim(define));
                        }
                        m_variants[variantName] = defines;
                    }
                }
            }
            catch (const std::exception& e)
            {
                Spark::SimpleConsole::GetInstance().LogError("Error parsing line " + std::to_string(lineNumber) +
                                                             " in " + filePath + ": " + std::string(e.what()));
                continue;
            }
        }

        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' loaded from: " + filePath +
                                                       " (textures: " + std::to_string(m_textures.size()) +
                                                       ", variants: " + std::to_string(m_variants.size()) + ")");

        return true;
    }
    catch (const std::exception& e)
    {
        Spark::SimpleConsole::GetInstance().LogError("Exception while loading material from " + filePath + ": " +
                                                     std::string(e.what()));
        return false;
    }
}

std::string Material::GetDetailedInfo() const
{
    std::stringstream ss;
    ss << "Material: " << m_name << "\n";
    ss << "Albedo: (" << m_pbrProperties.albedoColor.x << ", " << m_pbrProperties.albedoColor.y << ", "
       << m_pbrProperties.albedoColor.z << ")\n";
    ss << "Metallic: " << m_pbrProperties.metallicFactor << "\n";
    ss << "Roughness: " << m_pbrProperties.roughnessFactor << "\n";
    ss << "Normal Scale: " << m_pbrProperties.normalScale << "\n";
    ss << "Occlusion Strength: " << m_pbrProperties.occlusionStrength << "\n";
    ss << "Emissive: (" << m_pbrProperties.emissiveColor.x << ", " << m_pbrProperties.emissiveColor.y << ", "
       << m_pbrProperties.emissiveColor.z << ")\n";
    ss << "Emissive Factor: " << m_pbrProperties.emissiveFactor << "\n";
    ss << "Alpha Cutoff: " << m_pbrProperties.alphaCutoff << "\n";
    ss << "IOR: " << m_pbrProperties.indexOfRefraction << "\n";
    ss << "Blend Mode: " << static_cast<int>(m_renderState.blendMode) << "\n";
    ss << "Cull Mode: " << static_cast<int>(m_renderState.cullMode) << "\n";
    ss << "Depth Test: " << (m_renderState.depthTest ? "Yes" : "No") << "\n";
    ss << "Depth Write: " << (m_renderState.depthWrite ? "Yes" : "No") << "\n";
    ss << "Cast Shadows: " << (m_renderState.castShadows ? "Yes" : "No") << "\n";
    ss << "Receive Shadows: " << (m_renderState.receiveShadows ? "Yes" : "No") << "\n";
    ss << "Textures: " << m_textures.size() << "\n";

    // List textures
    for (const auto& pair : m_textures)
    {
        if (pair.second.enabled)
        {
            ss << "  - Type" << static_cast<int>(pair.first) << ": " << pair.second.filePath << "\n";
        }
    }

    ss << "Variants: " << m_variants.size() << "\n";
    if (!m_activeVariant.empty())
    {
        ss << "Active Variant: " << m_activeVariant << "\n";
    }

    return ss.str();
}

void Material::Console_SetProperty(const std::string& property, float value)
{
    if (property == "metallic")
    {
        m_pbrProperties.metallicFactor = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "roughness")
    {
        m_pbrProperties.roughnessFactor = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "normal")
    {
        m_pbrProperties.normalScale = std::max(0.0f, value);
    }
    else if (property == "occlusion")
    {
        m_pbrProperties.occlusionStrength = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "emissive_factor")
    {
        m_pbrProperties.emissiveFactor = std::max(0.0f, value);
    }
    else if (property == "alpha_cutoff")
    {
        m_pbrProperties.alphaCutoff = std::clamp(value, 0.0f, 1.0f);
    }
    else if (property == "ior")
    {
        m_pbrProperties.indexOfRefraction = std::max(1.0f, value);
    }
}

void Material::Console_SetColor(const std::string& property, float r, float g, float b)
{
    if (property == "albedo")
    {
        m_pbrProperties.albedoColor = {std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f),
                                       m_pbrProperties.albedoColor.w};
    }
    else if (property == "emissive")
    {
        m_pbrProperties.emissiveColor = {std::max(0.0f, r), std::max(0.0f, g), std::max(0.0f, b)};
    }
}

void Material::Console_ReloadTextures(ID3D11Device* device)
{
    if (!device)
        return;

    for (auto& pair : m_textures)
    {
        if (!pair.second.filePath.empty())
        {
            // Reload the texture from file
            LoadTexture(pair.first, pair.second.filePath, device);
        }
    }
}

const std::string& Material::GetActiveVariant() const
{
    return m_activeVariant;
}

std::vector<std::string> Material::GetAvailableVariants() const
{
    std::vector<std::string> variants;
    for (const auto& pair : m_variants)
    {
        variants.push_back(pair.first);
    }
    return variants;
}

HRESULT Material::CompileMaterial(ID3D11Device* device)
{
    if (!device)
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: device is null for material '" + m_name + "'");
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;

    // ---- Blend state ----
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    auto& rt = blendDesc.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    switch (m_renderState.blendMode)
    {
    case BlendMode::Opaque:
        rt.BlendEnable = FALSE;
        break;
    case BlendMode::AlphaTest:
        rt.BlendEnable = FALSE; // Alpha test handled in pixel shader via alphaCutoff
        break;
    case BlendMode::Transparent:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    case BlendMode::Additive:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_ONE;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    case BlendMode::Multiply:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_DEST_COLOR;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    case BlendMode::Screen:
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        break;
    }

    hr = device->CreateBlendState(&blendDesc, &m_blendState);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create blend state for '" + m_name +
                                                     "'");
        return hr;
    }

    // ---- Depth stencil state ----
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = m_renderState.depthTest ? TRUE : FALSE;
    dsDesc.DepthWriteMask = m_renderState.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
    dsDesc.StencilEnable = FALSE;

    hr = device->CreateDepthStencilState(&dsDesc, &m_depthStencilState);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create depth stencil state for '" +
                                                     m_name + "'");
        return hr;
    }

    // ---- Rasterizer state ----
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.FrontCounterClockwise = FALSE;
    rsDesc.DepthBias = 0;
    rsDesc.DepthBiasClamp = 0.0f;
    rsDesc.SlopeScaledDepthBias = 0.0f;
    rsDesc.DepthClipEnable = TRUE;
    rsDesc.ScissorEnable = FALSE;
    rsDesc.MultisampleEnable = FALSE;
    rsDesc.AntialiasedLineEnable = FALSE;

    if (m_renderState.doubleSided || m_renderState.cullMode == CullMode::None)
    {
        rsDesc.CullMode = D3D11_CULL_NONE;
    }
    else if (m_renderState.cullMode == CullMode::Front)
    {
        rsDesc.CullMode = D3D11_CULL_FRONT;
    }
    else
    {
        rsDesc.CullMode = D3D11_CULL_BACK;
    }

    hr = device->CreateRasterizerState(&rsDesc, &m_rasterizerState);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create rasterizer state for '" +
                                                     m_name + "'");
        return hr;
    }

    // ---- Material constant buffer ----
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(MaterialConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    MaterialConstants constants = {};
    constants.albedoColor = m_pbrProperties.albedoColor;
    constants.metallicFactor = m_pbrProperties.metallicFactor;
    constants.roughnessFactor = m_pbrProperties.roughnessFactor;
    constants.normalScale = m_pbrProperties.normalScale;
    constants.occlusionStrength = m_pbrProperties.occlusionStrength;
    constants.emissiveColor = m_pbrProperties.emissiveColor;
    constants.emissiveFactor = m_pbrProperties.emissiveFactor;
    constants.alphaCutoff = m_pbrProperties.alphaCutoff;
    constants.indexOfRefraction = m_pbrProperties.indexOfRefraction;
    constants.pad0 = 0.0f;
    constants.pad1 = 0.0f;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = &constants;

    hr = device->CreateBuffer(&cbDesc, &initData, &m_constantBuffer);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("CompileMaterial: failed to create constant buffer for '" +
                                                     m_name + "'");
        return hr;
    }

    m_compiled = true;
    Spark::SimpleConsole::GetInstance().LogInfo("Material '" + m_name + "' compiled successfully");
    return S_OK;
}

std::shared_ptr<Material> Material::CreateInstance(const std::string& instanceName) const
{
    auto instance = std::make_shared<Material>(instanceName);
    instance->m_pbrProperties = m_pbrProperties;
    instance->m_advancedProperties = m_advancedProperties;
    instance->m_renderState = m_renderState;
    instance->m_textures = m_textures;
    instance->m_variants = m_variants;
    instance->m_activeVariant = m_activeVariant;

    // Share compiled pipeline states from the template (read-only, safe to share)
    instance->m_blendState = m_blendState;
    instance->m_depthStencilState = m_depthStencilState;
    instance->m_rasterizerState = m_rasterizerState;
    // Constant buffer is NOT shared; the instance gets its own so properties can diverge
    instance->m_compiled = false;

    Spark::SimpleConsole::GetInstance().LogInfo("Created material instance '" + instanceName + "' from template '" +
                                                m_name + "'");
    return instance;
}

bool Material::ReloadMaterial(ID3D11Device* device)
{
    if (!device)
    {
        Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: device is null for material '" + m_name + "'");
        return false;
    }

    bool allSucceeded = true;

    // Collect texture entries to reload (cannot erase while iterating)
    std::vector<std::pair<MaterialTextureType, std::string>> texturesToReload;
    for (const auto& [type, matTexture] : m_textures)
    {
        if (!matTexture.filePath.empty())
        {
            texturesToReload.emplace_back(type, matTexture.filePath);
        }
    }

    // Clear all existing textures, then reload from disk
    m_textures.clear();

    for (const auto& [type, filePath] : texturesToReload)
    {
        if (!std::filesystem::exists(filePath))
        {
            Spark::SimpleConsole::GetInstance().LogWarning("ReloadMaterial: texture file missing for '" + m_name +
                                                           "': " + filePath);
            allSucceeded = false;
            continue;
        }

        if (!LoadTexture(type, filePath, device))
        {
            Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: failed to reload texture '" + filePath +
                                                         "' for material '" + m_name + "'");
            allSucceeded = false;
        }
    }

    // Recompile pipeline state
    m_blendState.Reset();
    m_depthStencilState.Reset();
    m_rasterizerState.Reset();
    m_constantBuffer.Reset();
    m_compiled = false;

    HRESULT hr = CompileMaterial(device);
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().LogError("ReloadMaterial: failed to recompile material '" + m_name + "'");
        allSucceeded = false;
    }

    if (allSucceeded)
    {
        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' reloaded successfully");
    }

    return allSucceeded;
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include "RHI/RHI.h"
#include "../Utils/Hash.h"
#include "../Utils/Validate.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <sys/stat.h>

// ============================================================================
// Material (Linux full implementation)
// ============================================================================

Material::Material(const std::string& name) : m_name(name)
{
    // Initialize PBR defaults: white albedo, dielectric, medium roughness
    m_pbrProperties.albedoColor = {1.0f, 1.0f, 1.0f, 1.0f};
    m_pbrProperties.metallicFactor = 0.0f;
    m_pbrProperties.roughnessFactor = 0.5f;
    m_pbrProperties.normalScale = 1.0f;
    m_pbrProperties.occlusionStrength = 1.0f;
    m_pbrProperties.emissiveColor = {0.0f, 0.0f, 0.0f};
    m_pbrProperties.emissiveFactor = 0.0f;
    m_pbrProperties.alphaCutoff = 0.5f;
    m_pbrProperties.indexOfRefraction = 1.5f;

    m_advancedProperties = {};
    m_renderState = {};
    m_activeVariant = "";
}

const MaterialTexture& Material::GetTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    if (it != m_textures.end())
    {
        return it->second;
    }
    static MaterialTexture defaultTexture;
    return defaultTexture;
}

const std::string& Material::GetActiveVariant() const
{
    return m_activeVariant;
}

std::vector<std::string> Material::GetAvailableVariants() const
{
    std::vector<std::string> variants;
    variants.reserve(m_variants.size());
    for (const auto& pair : m_variants)
    {
        variants.push_back(pair.first);
    }
    return variants;
}

void Material::SetTexture(MaterialTextureType type, const MaterialTexture& texture)
{
    m_textures[type] = texture;
}

bool Material::LoadTexture(MaterialTextureType type, const std::string& filePath, ID3D11Device* /*device*/)
{
    // On Linux we store CPU-side data only; no GPU texture creation
    MaterialTexture tex;
    tex.filePath = filePath;
    tex.enabled = true;
    tex.intensity = 1.0f;
    tex.tiling = {1.0f, 1.0f};
    tex.offset = {0.0f, 0.0f};
    m_textures[type] = tex;
    return true;
}

void Material::UnloadTexture(MaterialTextureType type)
{
    m_textures.erase(type);
}

bool Material::HasTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    return it != m_textures.end() && it->second.enabled;
}

void Material::BindToShader(ID3D11DeviceContext* /*context*/) const
{
    // No-op on Linux - no GPU binding available
}

void Material::CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
{
    m_variants[variantName] = defines;
}

void Material::SetActiveVariant(const std::string& variantName)
{
    if (m_variants.find(variantName) != m_variants.end())
    {
        m_activeVariant = variantName;
    }
}

bool Material::SaveToFile(const std::string& filePath) const
{
    std::ofstream file(filePath);
    if (!file.is_open())
    {
        return false;
    }

    if (m_name.empty())
    {
        return false;
    }

    // Use same INI format as Windows for cross-platform compatibility
    file << "# Spark Engine Material File\n";
    file << "# Version: 1.0\n\n";

    file << "[Material]\n";
    file << "Name=" << m_name << "\n";
    file << "ActiveVariant=" << m_activeVariant << "\n\n";

    file << "[PBR]\n";
    file << "AlbedoColor=" << m_pbrProperties.albedoColor.x << "," << m_pbrProperties.albedoColor.y << ","
         << m_pbrProperties.albedoColor.z << "," << m_pbrProperties.albedoColor.w << "\n";
    file << "MetallicFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.metallicFactor << "\n";
    file << "RoughnessFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.roughnessFactor << "\n";
    file << "NormalScale=" << std::fixed << std::setprecision(6) << m_pbrProperties.normalScale << "\n";
    file << "OcclusionStrength=" << std::fixed << std::setprecision(6) << m_pbrProperties.occlusionStrength << "\n";
    file << "EmissiveColor=" << m_pbrProperties.emissiveColor.x << "," << m_pbrProperties.emissiveColor.y << ","
         << m_pbrProperties.emissiveColor.z << "\n";
    file << "EmissiveFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.emissiveFactor << "\n";
    file << "AlphaCutoff=" << std::fixed << std::setprecision(6) << m_pbrProperties.alphaCutoff << "\n";
    file << "IndexOfRefraction=" << std::fixed << std::setprecision(6) << m_pbrProperties.indexOfRefraction << "\n\n";

    file << "[Advanced]\n";
    file << "SubsurfaceEnabled=" << (m_advancedProperties.subsurfaceEnabled ? "true" : "false") << "\n";
    if (m_advancedProperties.subsurfaceEnabled)
    {
        file << "SubsurfaceColor=" << m_advancedProperties.subsurfaceColor.x << ","
             << m_advancedProperties.subsurfaceColor.y << "," << m_advancedProperties.subsurfaceColor.z << "\n";
        file << "SubsurfaceRadius=" << m_advancedProperties.subsurfaceRadius << "\n";
    }
    file << "ClearcoatEnabled=" << (m_advancedProperties.clearcoatEnabled ? "true" : "false") << "\n";
    if (m_advancedProperties.clearcoatEnabled)
    {
        file << "ClearcoatFactor=" << m_advancedProperties.clearcoatFactor << "\n";
        file << "ClearcoatRoughness=" << m_advancedProperties.clearcoatRoughness << "\n";
    }
    file << "AnisotropyEnabled=" << (m_advancedProperties.anisotropyEnabled ? "true" : "false") << "\n";
    if (m_advancedProperties.anisotropyEnabled)
    {
        file << "AnisotropyFactor=" << m_advancedProperties.anisotropyFactor << "\n";
        file << "AnisotropyDirection=" << m_advancedProperties.anisotropyDirection.x << ","
             << m_advancedProperties.anisotropyDirection.y << "\n";
    }
    file << "TransmissionEnabled=" << (m_advancedProperties.transmissionEnabled ? "true" : "false") << "\n";
    if (m_advancedProperties.transmissionEnabled)
    {
        file << "TransmissionFactor=" << m_advancedProperties.transmissionFactor << "\n";
        file << "TransmissionColor=" << m_advancedProperties.transmissionColor.x << ","
             << m_advancedProperties.transmissionColor.y << "," << m_advancedProperties.transmissionColor.z << "\n";
    }
    file << "SheenEnabled=" << (m_advancedProperties.sheenEnabled ? "true" : "false") << "\n";
    if (m_advancedProperties.sheenEnabled)
    {
        file << "SheenColor=" << m_advancedProperties.sheenColor.x << "," << m_advancedProperties.sheenColor.y << ","
             << m_advancedProperties.sheenColor.z << "\n";
        file << "SheenRoughness=" << m_advancedProperties.sheenRoughness << "\n";
    }
    file << "IridescenceEnabled=" << (m_advancedProperties.iridescenceEnabled ? "true" : "false") << "\n";
    if (m_advancedProperties.iridescenceEnabled)
    {
        file << "IridescenceFactor=" << m_advancedProperties.iridescenceFactor << "\n";
        file << "IridescenceIOR=" << m_advancedProperties.iridescenceIOR << "\n";
        file << "IridescenceThickness=" << m_advancedProperties.iridescenceThickness << "\n";
    }
    file << "\n";

    file << "[RenderState]\n";
    file << "BlendMode=" << static_cast<int>(m_renderState.blendMode) << "\n";
    file << "CullMode=" << static_cast<int>(m_renderState.cullMode) << "\n";
    file << "DepthTest=" << (m_renderState.depthTest ? "true" : "false") << "\n";
    file << "DepthWrite=" << (m_renderState.depthWrite ? "true" : "false") << "\n";
    file << "CastShadows=" << (m_renderState.castShadows ? "true" : "false") << "\n";
    file << "ReceiveShadows=" << (m_renderState.receiveShadows ? "true" : "false") << "\n";
    file << "RenderQueue=" << m_renderState.renderQueue << "\n";
    file << "DoubleSided=" << (m_renderState.doubleSided ? "true" : "false") << "\n\n";

    file << "[Textures]\n";
    for (const auto& pair : m_textures)
    {
        if (!pair.second.filePath.empty())
        {
            file << "Texture" << static_cast<int>(pair.first) << "=" << pair.second.filePath << "\n";
            file << "Texture" << static_cast<int>(pair.first) << "_Enabled=" << (pair.second.enabled ? "true" : "false")
                 << "\n";
            file << "Texture" << static_cast<int>(pair.first) << "_Intensity=" << pair.second.intensity << "\n";
            file << "Texture" << static_cast<int>(pair.first) << "_Tiling=" << pair.second.tiling.x << ","
                 << pair.second.tiling.y << "\n";
            file << "Texture" << static_cast<int>(pair.first) << "_Offset=" << pair.second.offset.x << ","
                 << pair.second.offset.y << "\n";
        }
    }
    file << "\n";

    if (!m_variants.empty())
    {
        file << "[Variants]\n";
        for (const auto& variantPair : m_variants)
        {
            file << "Variant_" << variantPair.first << "=";
            for (size_t i = 0; i < variantPair.second.size(); ++i)
            {
                if (i > 0)
                    file << ",";
                file << variantPair.second[i];
            }
            file << "\n";
        }
    }

    file.close();
    return true;
}

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* /*device*/)
{
    // On Linux, full material file parsing is not supported.
    // Materials should be created programmatically or loaded via platform tools.
    fprintf(stderr,
            "[MaterialSystem] LoadFromFile: Loading from file '%s' is not "
            "supported on Linux. Use CreateMaterial() and set properties manually.\n",
            filePath.c_str());
    return false;
}

std::string Material::GetDetailedInfo() const
{
    std::stringstream ss;
    ss << "=== Material: " << m_name << " ===\n";

    // PBR Properties
    ss << "\n--- PBR Properties ---\n";
    ss << "  Albedo:      (" << m_pbrProperties.albedoColor.x << ", " << m_pbrProperties.albedoColor.y << ", "
       << m_pbrProperties.albedoColor.z << ", " << m_pbrProperties.albedoColor.w << ")\n";
    ss << "  Metallic:    " << m_pbrProperties.metallicFactor << "\n";
    ss << "  Roughness:   " << m_pbrProperties.roughnessFactor << "\n";
    ss << "  Normal Scale:" << m_pbrProperties.normalScale << "\n";
    ss << "  Occlusion:   " << m_pbrProperties.occlusionStrength << "\n";
    ss << "  Emissive:    (" << m_pbrProperties.emissiveColor.x << ", " << m_pbrProperties.emissiveColor.y << ", "
       << m_pbrProperties.emissiveColor.z << ") x " << m_pbrProperties.emissiveFactor << "\n";
    ss << "  Alpha Cutoff:" << m_pbrProperties.alphaCutoff << "\n";
    ss << "  IOR:         " << m_pbrProperties.indexOfRefraction << "\n";

    // Advanced Properties
    ss << "\n--- Advanced Properties ---\n";
    ss << "  Subsurface:    " << (m_advancedProperties.subsurfaceEnabled ? "ON" : "OFF");
    if (m_advancedProperties.subsurfaceEnabled)
    {
        ss << " (radius=" << m_advancedProperties.subsurfaceRadius << ")";
    }
    ss << "\n";
    ss << "  Clearcoat:     " << (m_advancedProperties.clearcoatEnabled ? "ON" : "OFF");
    if (m_advancedProperties.clearcoatEnabled)
    {
        ss << " (factor=" << m_advancedProperties.clearcoatFactor
           << ", roughness=" << m_advancedProperties.clearcoatRoughness << ")";
    }
    ss << "\n";
    ss << "  Anisotropy:    " << (m_advancedProperties.anisotropyEnabled ? "ON" : "OFF");
    if (m_advancedProperties.anisotropyEnabled)
    {
        ss << " (factor=" << m_advancedProperties.anisotropyFactor << ")";
    }
    ss << "\n";
    ss << "  Transmission:  " << (m_advancedProperties.transmissionEnabled ? "ON" : "OFF");
    if (m_advancedProperties.transmissionEnabled)
    {
        ss << " (factor=" << m_advancedProperties.transmissionFactor << ")";
    }
    ss << "\n";
    ss << "  Sheen:         " << (m_advancedProperties.sheenEnabled ? "ON" : "OFF") << "\n";
    ss << "  Iridescence:   " << (m_advancedProperties.iridescenceEnabled ? "ON" : "OFF") << "\n";

    // Render State
    ss << "\n--- Render State ---\n";
    const char* blendNames[] = {"Opaque", "AlphaTest", "Transparent", "Additive", "Multiply", "Screen"};
    const char* cullNames[] = {"None", "Front", "Back"};
    ss << "  Blend Mode:    " << blendNames[static_cast<int>(m_renderState.blendMode)] << "\n";
    ss << "  Cull Mode:     " << cullNames[static_cast<int>(m_renderState.cullMode)] << "\n";
    ss << "  Depth Test:    " << (m_renderState.depthTest ? "ON" : "OFF") << "\n";
    ss << "  Depth Write:   " << (m_renderState.depthWrite ? "ON" : "OFF") << "\n";
    ss << "  Cast Shadows:  " << (m_renderState.castShadows ? "ON" : "OFF") << "\n";
    ss << "  Recv Shadows:  " << (m_renderState.receiveShadows ? "ON" : "OFF") << "\n";
    ss << "  Render Queue:  " << m_renderState.renderQueue << "\n";
    ss << "  Double Sided:  " << (m_renderState.doubleSided ? "ON" : "OFF") << "\n";

    // Textures
    ss << "\n--- Textures (" << m_textures.size() << " slots) ---\n";
    for (const auto& pair : m_textures)
    {
        ss << "  [" << static_cast<int>(pair.first) << "] " << (pair.second.enabled ? "ACTIVE" : "INACTIVE")
           << " path=\"" << pair.second.filePath << "\"" << " intensity=" << pair.second.intensity << "\n";
    }

    // Variants
    ss << "\n--- Variants (" << m_variants.size() << ") ---\n";
    for (const auto& pair : m_variants)
    {
        ss << "  " << pair.first;
        if (pair.first == m_activeVariant)
            ss << " (ACTIVE)";
        ss << " [";
        for (size_t i = 0; i < pair.second.size(); ++i)
        {
            if (i > 0)
                ss << ", ";
            ss << pair.second[i];
        }
        ss << "]\n";
    }

    return ss.str();
}

void Material::Console_SetProperty(const std::string& property, float value)
{
    if (property == "metallic")
        m_pbrProperties.metallicFactor = std::clamp(value, 0.0f, 1.0f);
    else if (property == "roughness")
        m_pbrProperties.roughnessFactor = std::clamp(value, 0.0f, 1.0f);
    else if (property == "normalscale")
        m_pbrProperties.normalScale = value;
    else if (property == "occlusion")
        m_pbrProperties.occlusionStrength = std::clamp(value, 0.0f, 1.0f);
    else if (property == "emissive")
        m_pbrProperties.emissiveFactor = std::fmax(value, 0.0f);
    else if (property == "alphacutoff")
        m_pbrProperties.alphaCutoff = std::clamp(value, 0.0f, 1.0f);
    else if (property == "ior")
        m_pbrProperties.indexOfRefraction = std::fmax(value, 1.0f);
    else if (property == "clearcoat")
        m_advancedProperties.clearcoatFactor = std::clamp(value, 0.0f, 1.0f);
    else if (property == "clearcoatroughness")
        m_advancedProperties.clearcoatRoughness = std::clamp(value, 0.0f, 1.0f);
    else if (property == "anisotropy")
        m_advancedProperties.anisotropyFactor = std::clamp(value, -1.0f, 1.0f);
    else if (property == "transmission")
        m_advancedProperties.transmissionFactor = std::clamp(value, 0.0f, 1.0f);
    else
    {
        fprintf(stderr, "[Material] Unknown property: '%s'\n", property.c_str());
    }
}

void Material::Console_SetColor(const std::string& property, float r, float g, float b)
{
    if (property == "albedo")
    {
        m_pbrProperties.albedoColor = {r, g, b, m_pbrProperties.albedoColor.w};
    }
    else if (property == "emissive")
    {
        m_pbrProperties.emissiveColor = {r, g, b};
    }
    else if (property == "subsurface")
    {
        m_advancedProperties.subsurfaceColor = {r, g, b};
    }
    else if (property == "transmission")
    {
        m_advancedProperties.transmissionColor = {r, g, b};
    }
    else if (property == "sheen")
    {
        m_advancedProperties.sheenColor = {r, g, b};
    }
    else
    {
        fprintf(stderr, "[Material] Unknown color property: '%s'\n", property.c_str());
    }
}

void Material::Console_ReloadTextures(ID3D11Device* /*device*/)
{
    // No-op on Linux - GPU textures not available
    fprintf(stderr, "[Material] Console_ReloadTextures: No-op on Linux (no GPU textures)\n");
}

HRESULT Material::CompileMaterial(ID3D11Device* /*device*/)
{
    // No GPU pipeline state on Linux
    m_compiled = true;
    return S_OK;
}

std::shared_ptr<Material> Material::CreateInstance(const std::string& instanceName) const
{
    auto instance = std::make_shared<Material>(instanceName);
    instance->m_pbrProperties = m_pbrProperties;
    instance->m_advancedProperties = m_advancedProperties;
    instance->m_renderState = m_renderState;
    instance->m_textures = m_textures;
    instance->m_variants = m_variants;
    instance->m_activeVariant = m_activeVariant;
    instance->m_compiled = false;
    return instance;
}

bool Material::ReloadMaterial(ID3D11Device* device)
{
    // On Linux, re-load texture metadata from disk
    for (auto& [type, matTexture] : m_textures)
    {
        if (!matTexture.filePath.empty())
        {
            LoadTexture(type, matTexture.filePath, device);
        }
    }

    m_compiled = false;
    CompileMaterial(device);
    return true;
}

#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// PLATFORM-INDEPENDENT IMPLEMENTATIONS
// ============================================================================

std::vector<std::string> Material::GetShaderPermutation() const
{
    std::vector<std::string> defines;

    if (HasTexture(MaterialTextureType::Albedo))
        defines.push_back("HAS_ALBEDO_MAP");
    if (HasTexture(MaterialTextureType::Normal))
        defines.push_back("HAS_NORMAL_MAP");
    if (HasTexture(MaterialTextureType::Metallic))
        defines.push_back("HAS_METALLIC_MAP");
    if (HasTexture(MaterialTextureType::Roughness))
        defines.push_back("HAS_ROUGHNESS_MAP");
    if (HasTexture(MaterialTextureType::Occlusion))
        defines.push_back("HAS_OCCLUSION_MAP");
    if (HasTexture(MaterialTextureType::Emissive))
        defines.push_back("HAS_EMISSIVE_MAP");
    if (HasTexture(MaterialTextureType::Height))
        defines.push_back("HAS_HEIGHT_MAP");
    if (HasTexture(MaterialTextureType::DetailAlbedo))
        defines.push_back("HAS_DETAIL_ALBEDO_MAP");
    if (HasTexture(MaterialTextureType::DetailNormal))
        defines.push_back("HAS_DETAIL_NORMAL_MAP");
    if (HasTexture(MaterialTextureType::Subsurface))
        defines.push_back("HAS_SUBSURFACE_MAP");
    if (HasTexture(MaterialTextureType::Transmission))
        defines.push_back("HAS_TRANSMISSION_MAP");
    if (HasTexture(MaterialTextureType::Clearcoat))
        defines.push_back("HAS_CLEARCOAT_MAP");
    if (HasTexture(MaterialTextureType::ClearcoatRoughness))
        defines.push_back("HAS_CLEARCOAT_ROUGHNESS_MAP");
    if (HasTexture(MaterialTextureType::Anisotropy))
        defines.push_back("HAS_ANISOTROPY_MAP");

    switch (m_renderState.blendMode)
    {
    case BlendMode::AlphaTest:
        defines.push_back("ALPHA_TEST");
        break;
    case BlendMode::Transparent:
        defines.push_back("ALPHA_BLEND");
        break;
    case BlendMode::Additive:
        defines.push_back("BLEND_ADDITIVE");
        break;
    case BlendMode::Multiply:
        defines.push_back("BLEND_MULTIPLY");
        break;
    case BlendMode::Screen:
        defines.push_back("BLEND_SCREEN");
        break;
    default:
        break;
    }

    if (m_advancedProperties.subsurfaceEnabled)
        defines.push_back("ENABLE_SUBSURFACE");
    if (m_advancedProperties.clearcoatEnabled)
        defines.push_back("ENABLE_CLEARCOAT");
    if (m_advancedProperties.anisotropyEnabled)
        defines.push_back("ENABLE_ANISOTROPY");
    if (m_advancedProperties.transmissionEnabled)
        defines.push_back("ENABLE_TRANSMISSION");
    if (m_advancedProperties.sheenEnabled)
        defines.push_back("ENABLE_SHEEN");
    if (m_advancedProperties.iridescenceEnabled)
        defines.push_back("ENABLE_IRIDESCENCE");

    if (m_renderState.doubleSided)
        defines.push_back("DOUBLE_SIDED");

    if (!m_activeVariant.empty())
    {
        auto it = m_variants.find(m_activeVariant);
        if (it != m_variants.end())
        {
            for (const auto& define : it->second)
            {
                defines.push_back(define);
            }
        }
    }

    return defines;
}
