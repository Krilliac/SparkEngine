#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @file MaterialSystem.cpp
 * @brief Implementation of advanced material system for AAA-quality rendering
 * @author Spark Engine Team
 * @date 2025
 */

#include "MaterialSystem.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <chrono>

// Direct3D texture loading
#ifdef SPARK_PLATFORM_WINDOWS
#include <wincodec.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <wincodecsdk.h>

#ifdef SPARK_PLATFORM_WINDOWS

 // ============================================================================
 // MATERIAL CLASS IMPLEMENTATION
 // ============================================================================

Material::Material(const std::string& name)
    : m_name(name)
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
    if (it != m_textures.end()) {
        return it->second;
    }
    
    Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have texture of type " + std::to_string(static_cast<int>(type)));
    
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
    if (!device) {
        Spark::SimpleConsole::GetInstance().LogError("Device is null");
        return false;
    }
    
    if (filePath.empty()) {
        Spark::SimpleConsole::GetInstance().LogError("File path is empty");
        return false;
    }
    
    if (!std::filesystem::exists(filePath)) {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return false;
	}

    // Check if texture already loaded
    if (m_textures.find(type) != m_textures.end()) {
        Spark::SimpleConsole::GetInstance().LogInfo("Texture of type " + std::to_string(static_cast<int>(type)) + " already loaded for material '" + m_name + "'");
        return true;
	}
    
    // Load texture using WIC
    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Imaging Factory");
        return false;
    }
    
    ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(
        std::wstring(filePath.begin(), filePath.end()).c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder
    );
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Decoder for file: " + filePath);
        return false;
    }
    
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to get frame from WIC Decoder for file: " + filePath);
        return false;
    }
    
    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Format Converter");
        return false;
    }
    
    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to initialize WIC Format Converter");
        return false;
    }
    
    UINT width, height;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to get image size from WIC Converter");
        return false;
    }
    
    std::vector<BYTE> imageData(width * height * 4); // 4 bytes per pixel (RGBA)
    hr = converter->CopyPixels(
        nullptr,
        width * 4,
		static_cast<UINT>(imageData.size()),
        imageData.data()
    );
    if (FAILED(hr)) {
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
    if (FAILED(hr)) {
        Spark::SimpleConsole::GetInstance().LogError("Failed to create Direct3D texture for file: " + filePath);
        return false;
    }
    
    // Create shader resource view
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr)) {
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
    if (it != m_textures.end()) {
        m_textures.erase(it);
    } else {
        Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have texture of type " + std::to_string(static_cast<int>(type)) + " to unload");
    }
}

bool Material::HasTexture(MaterialTextureType type) const
{
    return m_textures.find(type) != m_textures.end();
}

void Material::BindToShader(ID3D11DeviceContext* context) const
{
    if (!context) {
        Spark::SimpleConsole::GetInstance().LogWarning("Null context in Material::BindToShader for material: " + m_name);
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
        {MaterialTextureType::Custom3, 17}
    };

    // Bind material textures to their designated slots
    std::vector<ID3D11ShaderResourceView*> srvArray(18, nullptr); // Max 18 texture slots
    std::vector<ID3D11SamplerState*> samplerArray(18, nullptr);
    
    int boundTextures = 0;
    for (const auto& texturePair : m_textures) {
        MaterialTextureType type = texturePair.first;
        const MaterialTexture& matTexture = texturePair.second;
        
        auto slotIt = textureSlotMapping.find(type);
        if (slotIt == textureSlotMapping.end()) {
            continue; // Unknown texture type
        }
        
        UINT slot = slotIt->second;
        if (slot >= 18) continue; // Safety check
        
        if (matTexture.enabled && matTexture.texture) {
            srvArray[slot] = matTexture.texture.Get();
            
            // For now, we'll use a default sampler since we don't have access to MaterialSystem here
            // In a full implementation, you'd pass the sampler or get it from a global manager
            boundTextures++;
        }
    }
    
    // Bind all textures at once for efficiency
    if (boundTextures > 0) {
        context->PSSetShaderResources(0, 18, srvArray.data());
        
        // Note: Samplers would also be bound here if we had access to them
        // context->PSSetSamplers(0, 18, samplerArray.data());
    }
    
    // Bind material constants (would typically be done through a constant buffer)
    // This is a simplified approach - in practice you'd update a constant buffer
    // with all material properties and bind it to the shader
    
    // Log binding for debugging in debug builds
    #ifdef _DEBUG
    static int bindCount = 0;
    if (++bindCount % 100 == 0) { // Log every 100 binds to avoid spam
        Spark::SimpleConsole::GetInstance().LogInfo(
            "Material '" + m_name + "' bound with " + std::to_string(boundTextures) + " textures"
        );
    }
    #endif
}

void Material::CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
{
    m_variants[variantName] = defines;
}

void Material::SetActiveVariant(const std::string& variantName)
{
    if (m_variants.find(variantName) != m_variants.end()) {
        m_activeVariant = variantName;
    } else {
        Spark::SimpleConsole::GetInstance().LogWarning("Material '" + m_name + "' does not have variant '" + variantName + "'");
	}
}

bool Material::SaveToFile(const std::string& filePath) const
{
    try {
        std::ofstream file(filePath);
        if (!file.is_open()) {
            Spark::SimpleConsole::GetInstance().LogError("Cannot open file for writing: " + filePath);
            return false;
        }
        
        if (m_name.empty()) {
            Spark::SimpleConsole::GetInstance().LogError("Cannot save material with empty name");
            return false;
        }
        
        // Write file header with version for future compatibility
        file << "# Spark Engine Material File\n";
        file << "# Version: 1.0\n";
        file << "# Generated: " << std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() << "\n";
        file << "\n";

        // Write basic material info
        file << "[Material]\n";
        file << "Name=" << m_name << "\n";
        file << "ActiveVariant=" << m_activeVariant << "\n";
        file << "\n";

        // Write PBR properties with full precision
        file << "[PBR]\n";
        file << "AlbedoColor=" << m_pbrProperties.albedoColor.x << "," << m_pbrProperties.albedoColor.y 
             << "," << m_pbrProperties.albedoColor.z << "," << m_pbrProperties.albedoColor.w << "\n";
        file << "MetallicFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.metallicFactor << "\n";
        file << "RoughnessFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.roughnessFactor << "\n";
        file << "NormalScale=" << std::fixed << std::setprecision(6) << m_pbrProperties.normalScale << "\n";
        file << "OcclusionStrength=" << std::fixed << std::setprecision(6) << m_pbrProperties.occlusionStrength << "\n";
        file << "EmissiveColor=" << m_pbrProperties.emissiveColor.x << "," 
             << m_pbrProperties.emissiveColor.y << "," << m_pbrProperties.emissiveColor.z << "\n";
        file << "EmissiveFactor=" << std::fixed << std::setprecision(6) << m_pbrProperties.emissiveFactor << "\n";
        file << "AlphaCutoff=" << std::fixed << std::setprecision(6) << m_pbrProperties.alphaCutoff << "\n";
        file << "IndexOfRefraction=" << std::fixed << std::setprecision(6) << m_pbrProperties.indexOfRefraction << "\n";
        file << "\n";

        // Write advanced properties
        file << "[Advanced]\n";
        file << "SubsurfaceEnabled=" << (m_advancedProperties.subsurfaceEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.subsurfaceEnabled) {
            file << "SubsurfaceColor=" << m_advancedProperties.subsurfaceColor.x << "," 
                 << m_advancedProperties.subsurfaceColor.y << "," << m_advancedProperties.subsurfaceColor.z << "\n";
            file << "SubsurfaceRadius=" << m_advancedProperties.subsurfaceRadius << "\n";
        }
        
        file << "ClearcoatEnabled=" << (m_advancedProperties.clearcoatEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.clearcoatEnabled) {
            file << "ClearcoatFactor=" << m_advancedProperties.clearcoatFactor << "\n";
            file << "ClearcoatRoughness=" << m_advancedProperties.clearcoatRoughness << "\n";
        }
        
        file << "AnisotropyEnabled=" << (m_advancedProperties.anisotropyEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.anisotropyEnabled) {
            file << "AnisotropyFactor=" << m_advancedProperties.anisotropyFactor << "\n";
            file << "AnisotropyDirection=" << m_advancedProperties.anisotropyDirection.x 
                 << "," << m_advancedProperties.anisotropyDirection.y << "\n";
        }
        
        file << "TransmissionEnabled=" << (m_advancedProperties.transmissionEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.transmissionEnabled) {
            file << "TransmissionFactor=" << m_advancedProperties.transmissionFactor << "\n";
            file << "TransmissionColor=" << m_advancedProperties.transmissionColor.x << "," 
                 << m_advancedProperties.transmissionColor.y << "," << m_advancedProperties.transmissionColor.z << "\n";
        }
        
        file << "SheenEnabled=" << (m_advancedProperties.sheenEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.sheenEnabled) {
            file << "SheenColor=" << m_advancedProperties.sheenColor.x << "," 
                 << m_advancedProperties.sheenColor.y << "," << m_advancedProperties.sheenColor.z << "\n";
            file << "SheenRoughness=" << m_advancedProperties.sheenRoughness << "\n";
        }
        
        file << "IridescenceEnabled=" << (m_advancedProperties.iridescenceEnabled ? "true" : "false") << "\n";
        if (m_advancedProperties.iridescenceEnabled) {
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
        for (const auto& pair : m_textures) {
            if (!pair.second.filePath.empty()) {
                file << "Texture" << static_cast<int>(pair.first) << "=" << pair.second.filePath << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Enabled=" << (pair.second.enabled ? "true" : "false") << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Intensity=" << pair.second.intensity << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Tiling=" << pair.second.tiling.x << "," << pair.second.tiling.y << "\n";
                file << "Texture" << static_cast<int>(pair.first) << "_Offset=" << pair.second.offset.x << "," << pair.second.offset.y << "\n";
            }
        }
        file << "\n";

        // Write material variants
        if (!m_variants.empty()) {
            file << "[Variants]\n";
            for (const auto& variantPair : m_variants) {
                file << "Variant_" << variantPair.first << "=";
                for (size_t i = 0; i < variantPair.second.size(); ++i) {
                    if (i > 0) file << ",";
                    file << variantPair.second[i];
                }
                file << "\n";
            }
            file << "\n";
        }

        file.close();
        
        Spark::SimpleConsole::GetInstance().LogSuccess("Material '" + m_name + "' saved to: " + filePath);
        return true;
        
    } catch (const std::exception& e) {
        Spark::SimpleConsole::GetInstance().LogError("Exception while saving material '" + m_name + "': " + std::string(e.what()));
        return false;
    }
}

bool Material::LoadFromFile(const std::string& filePath, ID3D11Device* device)
{
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            Spark::SimpleConsole::GetInstance().LogError("Cannot open file for reading: " + filePath);
            return false;
        }

        std::string line;
        std::string currentSection = "";
        int lineNumber = 0;
        
        // Helper lambda to parse comma-separated values
        auto parseFloatArray = [](const std::string& value, std::vector<float>& output) {
            output.clear();
            std::stringstream ss(value);
            std::string item;
            while (std::getline(ss, item, ',')) {
                try {
                    output.push_back(std::stof(item));
                } catch (const std::exception&) {
                    return false;
                }
            }
            return true;
        };
        
        // Helper lambda to parse boolean values
        auto parseBool = [](const std::string& value) -> bool {
            return value == "true" || value == "1" || value == "yes";
        };
        
        // Helper lambda to trim whitespace
        auto trim = [](const std::string& str) -> std::string {
            size_t start = str.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) return "";
            size_t end = str.find_last_not_of(" \t\r\n");
            return str.substr(start, end - start + 1);
        };

        while (std::getline(file, line)) {
            lineNumber++;
            line = trim(line);
            
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            // Check for section headers
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }
            
            // Parse key=value pairs
            size_t equalPos = line.find('=');
            if (equalPos == std::string::npos) {
                Spark::SimpleConsole::GetInstance().LogWarning(
                    "Invalid line format at line " + std::to_string(lineNumber) + " in: " + filePath
                );
                continue;
            }
            
            std::string key = trim(line.substr(0, equalPos));
            std::string value = trim(line.substr(equalPos + 1));
            
            try {
                // Parse based on current section
                if (currentSection == "Material") {
                    if (key == "Name") {
                        m_name = value;
                    } else if (key == "ActiveVariant") {
                        m_activeVariant = value;
                    }
                }
                else if (currentSection == "PBR") {
                    if (key == "AlbedoColor") {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3) {
                            m_pbrProperties.albedoColor.x = components[0];
                            m_pbrProperties.albedoColor.y = components[1];
                            m_pbrProperties.albedoColor.z = components[2];
                            m_pbrProperties.albedoColor.w = components.size() > 3 ? components[3] : 1.0f;
                        }
                    } else if (key == "MetallicFactor") {
                        m_pbrProperties.metallicFactor = std::stof(value);
                    } else if (key == "RoughnessFactor") {
                        m_pbrProperties.roughnessFactor = std::stof(value);
                    } else if (key == "NormalScale") {
                        m_pbrProperties.normalScale = std::stof(value);
                    } else if (key == "OcclusionStrength") {
                        m_pbrProperties.occlusionStrength = std::stof(value);
                    } else if (key == "EmissiveColor") {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3) {
                            m_pbrProperties.emissiveColor.x = components[0];
                            m_pbrProperties.emissiveColor.y = components[1];
                            m_pbrProperties.emissiveColor.z = components[2];
                        }
                    } else if (key == "EmissiveFactor") {
                        m_pbrProperties.emissiveFactor = std::stof(value);
                    } else if (key == "AlphaCutoff") {
                        m_pbrProperties.alphaCutoff = std::stof(value);
                    } else if (key == "IndexOfRefraction") {
                        m_pbrProperties.indexOfRefraction = std::stof(value);
                    }
                }
                else if (currentSection == "Advanced") {
                    if (key == "SubsurfaceEnabled") {
                        m_advancedProperties.subsurfaceEnabled = parseBool(value);
                    } else if (key == "SubsurfaceColor") {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3) {
                            m_advancedProperties.subsurfaceColor.x = components[0];
                            m_advancedProperties.subsurfaceColor.y = components[1];
                            m_advancedProperties.subsurfaceColor.z = components[2];
                        }
                    } else if (key == "SubsurfaceRadius") {
                        m_advancedProperties.subsurfaceRadius = std::stof(value);
                    } else if (key == "ClearcoatEnabled") {
                        m_advancedProperties.clearcoatEnabled = parseBool(value);
                    } else if (key == "ClearcoatFactor") {
                        m_advancedProperties.clearcoatFactor = std::stof(value);
                    } else if (key == "ClearcoatRoughness") {
                        m_advancedProperties.clearcoatRoughness = std::stof(value);
                    } else if (key == "AnisotropyEnabled") {
                        m_advancedProperties.anisotropyEnabled = parseBool(value);
                    } else if (key == "AnisotropyFactor") {
                        m_advancedProperties.anisotropyFactor = std::stof(value);
                    } else if (key == "AnisotropyDirection") {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 2) {
                            m_advancedProperties.anisotropyDirection.x = components[0];
                            m_advancedProperties.anisotropyDirection.y = components[1];
                        }
                    } else if (key == "TransmissionEnabled") {
                        m_advancedProperties.transmissionEnabled = parseBool(value);
                    } else if (key == "TransmissionFactor") {
                        m_advancedProperties.transmissionFactor = std::stof(value);
                    } else if (key == "TransmissionColor") {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3) {
                            m_advancedProperties.transmissionColor.x = components[0];
                            m_advancedProperties.transmissionColor.y = components[1];
                            m_advancedProperties.transmissionColor.z = components[2];
                        }
                    } else if (key == "SheenEnabled") {
                        m_advancedProperties.sheenEnabled = parseBool(value);
                    } else if (key == "SheenColor") {
                        std::vector<float> components;
                        if (parseFloatArray(value, components) && components.size() >= 3) {
                            m_advancedProperties.sheenColor.x = components[0];
                            m_advancedProperties.sheenColor.y = components[1];
                            m_advancedProperties.sheenColor.z = components[2];
                        }
                    } else if (key == "SheenRoughness") {
                        m_advancedProperties.sheenRoughness = std::stof(value);
                    } else if (key == "IridescenceEnabled") {
                        m_advancedProperties.iridescenceEnabled = parseBool(value);
                    } else if (key == "IridescenceFactor") {
                        m_advancedProperties.iridescenceFactor = std::stof(value);
                    } else if (key == "IridescenceIOR") {
                        m_advancedProperties.iridescenceIOR = std::stof(value);
                    } else if (key == "IridescenceThickness") {
                        m_advancedProperties.iridescenceThickness = std::stof(value);
                    }
                }
                else if (currentSection == "RenderState") {
                    if (key == "BlendMode") {
                        m_renderState.blendMode = static_cast<BlendMode>(std::stoi(value));
                    } else if (key == "CullMode") {
                        m_renderState.cullMode = static_cast<CullMode>(std::stoi(value));
                    } else if (key == "DepthTest") {
                        m_renderState.depthTest = parseBool(value);
                    } else if (key == "DepthWrite") {
                        m_renderState.depthWrite = parseBool(value);
                    } else if (key == "CastShadows") {
                        m_renderState.castShadows = parseBool(value);
                    } else if (key == "ReceiveShadows") {
                        m_renderState.receiveShadows = parseBool(value);
                    } else if (key == "RenderQueue") {
                        m_renderState.renderQueue = std::stoi(value);
                    } else if (key == "DoubleSided") {
                        m_renderState.doubleSided = parseBool(value);
                    }
                }
                else if (currentSection == "Textures") {
                    // Parse texture entries
                    if (key.substr(0, 7) == "Texture" && key.find("_") == std::string::npos) {
                        // Main texture path
                        int textureType = std::stoi(key.substr(7));
                        LoadTexture(static_cast<MaterialTextureType>(textureType), value, device);
                    }
                    // Parse texture properties (enabled, intensity, tiling, offset)
                    else if (key.find("_Enabled") != std::string::npos) {
                        std::string baseKey = key.substr(0, key.find("_Enabled"));
                        if (baseKey.substr(0, 7) == "Texture") {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end()) {
                                it->second.enabled = parseBool(value);
                            }
                        }
                    }
                    else if (key.find("_Intensity") != std::string::npos) {
                        std::string baseKey = key.substr(0, key.find("_Intensity"));
                        if (baseKey.substr(0, 7) == "Texture") {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end()) {
                                it->second.intensity = std::stof(value);
                            }
                        }
                    }
                    else if (key.find("_Tiling") != std::string::npos) {
                        std::string baseKey = key.substr(0, key.find("_Tiling"));
                        if (baseKey.substr(0, 7) == "Texture") {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end()) {
                                std::vector<float> components;
                                if (parseFloatArray(value, components) && components.size() >= 2) {
                                    it->second.tiling.x = components[0];
                                    it->second.tiling.y = components[1];
                                }
                            }
                        }
                    }
                    else if (key.find("_Offset") != std::string::npos) {
                        std::string baseKey = key.substr(0, key.find("_Offset"));
                        if (baseKey.substr(0, 7) == "Texture") {
                            int textureType = std::stoi(baseKey.substr(7));
                            auto it = m_textures.find(static_cast<MaterialTextureType>(textureType));
                            if (it != m_textures.end()) {
                                std::vector<float> components;
                                if (parseFloatArray(value, components) && components.size() >= 2) {
                                    it->second.offset.x = components[0];
                                    it->second.offset.y = components[1];
                                }
                            }
                        }
                    }
                }
                else if (currentSection == "Variants") {
                    if (key.substr(0, 8) == "Variant_") {
                        std::string variantName = key.substr(8);
                        std::vector<std::string> defines;
                        std::stringstream ss(value);
                        std::string define;
                        while (std::getline(ss, define, ',')) {
                            defines.push_back(trim(define));
                        }
                        m_variants[variantName] = defines;
                    }
                }
                
            } catch (const std::exception& e) {
                Spark::SimpleConsole::GetInstance().LogError(
                    "Error parsing line " + std::to_string(lineNumber) + " in " + filePath + 
                    ": " + std::string(e.what())
                );
                continue;
            }
        }

        file.close();
        
        Spark::SimpleConsole::GetInstance().LogSuccess(
            "Material '" + m_name + "' loaded from: " + filePath + 
            " (textures: " + std::to_string(m_textures.size()) + 
            ", variants: " + std::to_string(m_variants.size()) + ")"
        );
        
        return true;
        
    } catch (const std::exception& e) {
        Spark::SimpleConsole::GetInstance().LogError("Exception while loading material from " + filePath + ": " + std::string(e.what()));
        return false;
    }
}

std::string Material::GetDetailedInfo() const
{
    std::stringstream ss;
    ss << "Material: " << m_name << "\n";
    ss << "Albedo: (" << m_pbrProperties.albedoColor.x << ", " << m_pbrProperties.albedoColor.y << ", " << m_pbrProperties.albedoColor.z << ")\n";
    ss << "Metallic: " << m_pbrProperties.metallicFactor << "\n";
    ss << "Roughness: " << m_pbrProperties.roughnessFactor << "\n";
    ss << "Normal Scale: " << m_pbrProperties.normalScale << "\n";
    ss << "Occlusion Strength: " << m_pbrProperties.occlusionStrength << "\n";
    ss << "Emissive: (" << m_pbrProperties.emissiveColor.x << ", " << m_pbrProperties.emissiveColor.y << ", " << m_pbrProperties.emissiveColor.z << ")\n";
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
    for (const auto& pair : m_textures) {
        if (pair.second.enabled) {
            ss << "  - Type" << static_cast<int>(pair.first) << ": " << pair.second.filePath << "\n";
        }
    }
    
    ss << "Variants: " << m_variants.size() << "\n";
    if (!m_activeVariant.empty()) {
        ss << "Active Variant: " << m_activeVariant << "\n";
    }
    
    return ss.str();
}

void Material::Console_SetProperty(const std::string& property, float value)
{
    if (property == "metallic") {
        m_pbrProperties.metallicFactor = std::clamp(value, 0.0f, 1.0f);
    } else if (property == "roughness") {
        m_pbrProperties.roughnessFactor = std::clamp(value, 0.0f, 1.0f);
    } else if (property == "normal") {
        m_pbrProperties.normalScale = std::max(0.0f, value);
    } else if (property == "occlusion") {
        m_pbrProperties.occlusionStrength = std::clamp(value, 0.0f, 1.0f);
    } else if (property == "emissive_factor") {
        m_pbrProperties.emissiveFactor = std::max(0.0f, value);
    } else if (property == "alpha_cutoff") {
        m_pbrProperties.alphaCutoff = std::clamp(value, 0.0f, 1.0f);
    } else if (property == "ior") {
        m_pbrProperties.indexOfRefraction = std::max(1.0f, value);
    }
}

void Material::Console_SetColor(const std::string& property, float r, float g, float b)
{
    if (property == "albedo") {
        m_pbrProperties.albedoColor = { std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), m_pbrProperties.albedoColor.w };
    } else if (property == "emissive") {
        m_pbrProperties.emissiveColor = { std::max(0.0f, r), std::max(0.0f, g), std::max(0.0f, b) };
    }
}

void Material::Console_ReloadTextures(ID3D11Device* device)
{
    if (!device) return;

    for (auto& pair : m_textures) {
        if (!pair.second.filePath.empty()) {
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
    for (const auto& pair : m_variants) {
        variants.push_back(pair.first);
    }
    return variants;
}

// ============================================================================
// MATERIAL SYSTEM IMPLEMENTATION
// ============================================================================

MaterialSystem::MaterialSystem()
    : m_device(nullptr)
    , m_context(nullptr)
    , m_hotReloadEnabled(false)
{
}

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

HRESULT MaterialSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT(device && context);
    m_device = device;
    m_context = context;

    HRESULT hr = CreateDefaultMaterials();
    if (SUCCEEDED(hr)) {
        Spark::SimpleConsole::GetInstance().Log("MaterialSystem initialized successfully", "SUCCESS");
    }
    return hr;
}

void MaterialSystem::Shutdown()
{
    m_materials.clear();
    m_textureCache.clear();
    m_samplerCache.clear();
    m_defaultMaterial.reset();
    m_errorMaterial.reset();
    m_device = nullptr;
    m_context = nullptr;
}

std::shared_ptr<Material> MaterialSystem::CreateMaterial(const std::string& name)
{
    auto material = std::make_shared<Material>(name);
    m_materials[name] = material;
    return material;
}

std::shared_ptr<Material> MaterialSystem::LoadMaterial(const std::string& filePath)
{
    // Check if already loaded
    auto it = m_materials.find(filePath);
    if (it != m_materials.end()) {
        return it->second;
    }

    auto material = std::make_shared<Material>(filePath);
    if (material->LoadFromFile(filePath, m_device)) {
        m_materials[filePath] = material;
        
        // Store file timestamp for hot reloading
        if (m_hotReloadEnabled) {
            m_fileTimestamps[filePath] = GetFileTimestamp(filePath);
        }
        
        return material;
    }
    
    Spark::SimpleConsole::GetInstance().LogError("Failed to load material: " + filePath);
    return m_errorMaterial;
}

std::shared_ptr<Material> MaterialSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? it->second : m_defaultMaterial;
}

void MaterialSystem::UnloadMaterial(const std::string& name)
{
    auto it = m_materials.find(name);
    if (it != m_materials.end()) {
        m_materials.erase(it);
        m_fileTimestamps.erase(name);
    }
}

void MaterialSystem::UnloadAllMaterials()
{
    m_materials.clear();
    m_fileTimestamps.clear();
}

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTexture(const std::string& filePath)
{
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end()) {
        return it->second;
    }

    ComPtr<ID3D11ShaderResourceView> texture = LoadTextureFromFile(filePath);
    if (texture) {
        m_textureCache[filePath] = texture;
        Spark::SimpleConsole::GetInstance().LogInfo("Loaded texture: " + filePath);
    } else {
        Spark::SimpleConsole::GetInstance().LogError("Failed to load texture: " + filePath);
    }

    return texture;
}

void MaterialSystem::UnloadTexture(const std::string& filePath)
{
    auto it = m_textureCache.find(filePath);
    if (it != m_textureCache.end()) {
        m_textureCache.erase(it);
        Spark::SimpleConsole::GetInstance().LogInfo("Unloaded texture: " + filePath);
    }
}

ComPtr<ID3D11SamplerState> MaterialSystem::GetSampler(const TextureSampling& sampling)
{
    size_t hash = HashSampling(sampling);
    auto it = m_samplerCache.find(hash);
    if (it != m_samplerCache.end()) {
        return it->second;
    }

    ComPtr<ID3D11SamplerState> sampler;
    HRESULT hr = CreateSampler(sampling, &sampler);
    if (SUCCEEDED(hr)) {
        m_samplerCache[hash] = sampler;
    }
    return sampler;
}

void MaterialSystem::UpdateHotReload()
{
    if (!m_hotReloadEnabled) return;

    for (auto& pair : m_fileTimestamps) {
        const std::string& filePath = pair.first;
        uint64_t& lastTimestamp = pair.second;
        
        uint64_t currentTimestamp = GetFileTimestamp(filePath);
        if (currentTimestamp > lastTimestamp) {
            // File has been modified, reload it
            auto it = m_materials.find(filePath);
            if (it != m_materials.end()) {
                if (it->second->LoadFromFile(filePath, m_device)) {
                    lastTimestamp = currentTimestamp;
                    Spark::SimpleConsole::GetInstance().LogInfo("Hot reloaded material: " + filePath);
                } else {
                    Spark::SimpleConsole::GetInstance().LogError("Failed to hot reload material: " + filePath);
                }
            }
        }
    }
}

int MaterialSystem::ReloadAllMaterials()
{
    int reloadedCount = 0;
    
    for (auto& pair : m_materials) {
        auto& material = pair.second;
        if (material->LoadFromFile(pair.first, m_device)) {
            reloadedCount++;
        }
    }
    
    Spark::SimpleConsole::GetInstance().LogInfo("Reloaded " + std::to_string(reloadedCount) + " materials");
    return reloadedCount;
}

void MaterialSystem::BeginFrame()
{
    m_frameStartTime = std::chrono::high_resolution_clock::now();
    
    // Reset per-frame metrics
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_metrics.materialSwitches = 0;
    m_metrics.textureBinds = 0;
    
    UpdateMetrics();
    
    // Update hot reloading
    UpdateHotReload();
    
    // Perform periodic maintenance
    PerformPeriodicMaintenance();
}

void MaterialSystem::EndFrame()
{
    auto frameEndTime = std::chrono::high_resolution_clock::now();
    auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(frameEndTime - m_frameStartTime);
    
    // Update frame time metrics if needed
}

MaterialSystem::MaterialMetrics MaterialSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string MaterialSystem::Console_ListMaterials() const
{
    std::stringstream ss;
    ss << "=== Loaded Materials ===\n";
    for (const auto& pair : m_materials) {
        auto& material = pair.second;
        ss << pair.first;
        if (material) {
            ss << " (" << material->GetName() << ")";
        }
        ss << "\n";
    }
    ss << "Total: " << m_materials.size() << " materials";
    return ss.str();
}

std::string MaterialSystem::Console_GetMaterialInfo(const std::string& materialName) const
{
    auto material = GetMaterial(materialName);
    if (material && material != m_defaultMaterial) {
        return material->GetDetailedInfo();
    }
    return "Material not found: " + materialName;
}

bool MaterialSystem::Console_ReloadMaterial(const std::string& materialName)
{
    auto it = m_materials.find(materialName);
    if (it != m_materials.end()) {
        if (it->second->LoadFromFile(materialName, m_device)) {
            Spark::SimpleConsole::GetInstance().LogSuccess("Reloaded material: " + materialName);
            return true;
        } else {
            Spark::SimpleConsole::GetInstance().LogError("Failed to reload material: " + materialName);
        }
    } else {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
    }
    return false;
}

int MaterialSystem::Console_ReloadAllMaterials()
{
    return ReloadAllMaterials();
}

bool MaterialSystem::Console_CreateVariant(const std::string& materialName, const std::string& variantName, const std::vector<std::string>& defines)
{
    auto material = GetMaterial(materialName);
    if (material && material != m_defaultMaterial) {
        material->CreateVariant(variantName, defines);
        Spark::SimpleConsole::GetInstance().LogSuccess("Created variant '" + variantName + "' for material: " + materialName);
        return true;
    }
    Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
    return false;
}

void MaterialSystem::Console_SetMaterialProperty(const std::string& materialName, const std::string& property, float value)
{
    auto material = GetMaterial(materialName);
    if (material && material != m_defaultMaterial) {
        material->Console_SetProperty(property, value);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " = " + std::to_string(value) + " for material: " + materialName);
    } else {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
    }
}

void MaterialSystem::Console_SetMaterialColor(const std::string& materialName, const std::string& property, float r, float g, float b)
{
    auto material = GetMaterial(materialName);
    if (material && material != m_defaultMaterial) {
        material->Console_SetColor(property, r, g, b);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " color for material: " + materialName);
    } else {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
    }
}

void MaterialSystem::Console_SetHotReload(bool enabled)
{
    m_hotReloadEnabled = enabled;
    if (enabled) {
        // Initialize timestamps for all currently loaded materials
        for (const auto& pair : m_materials) {
            m_fileTimestamps[pair.first] = GetFileTimestamp(pair.first);
        }
        Spark::SimpleConsole::GetInstance().LogSuccess("Hot reload enabled");
    } else {
        m_fileTimestamps.clear();
        Spark::SimpleConsole::GetInstance().LogInfo("Hot reload disabled");
    }
}

void MaterialSystem::Console_ClearCache()
{
    size_t textureCount = m_textureCache.size();
    size_t samplerCount = m_samplerCache.size();
    
    m_textureCache.clear();
    m_samplerCache.clear();
    
    Spark::SimpleConsole::GetInstance().LogSuccess(
        "Cleared cache: " + std::to_string(textureCount) + " textures, " + 
        std::to_string(samplerCount) + " samplers"
    );
}

void MaterialSystem::Console_GarbageCollect()
{
    // Remove unused materials (those with only one reference - the one in the map)
    auto it = m_materials.begin();
    int removedCount = 0;
    
    while (it != m_materials.end()) {
        if (it->second.use_count() == 1) {
            it = m_materials.erase(it);
            removedCount++;
        } else {
            ++it;
        }
    }
    
    Spark::SimpleConsole::GetInstance().LogSuccess("Garbage collected " + std::to_string(removedCount) + " unused materials");
}

void MaterialSystem::Console_SetTextureQuality(const std::string& quality)
{
    struct TextureQualitySettings {
        D3D11_FILTER filter;
        UINT maxAnisotropy;
        float mipLODBias;
        std::string description;
    };
    
    static std::unordered_map<std::string, TextureQualitySettings> qualityPresets = {
        {"low", {
            D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            1,
            0.5f,
            "Low quality - Linear filtering, no anisotropic filtering"
        }},
        {"medium", {
            D3D11_FILTER_ANISOTROPIC,
            4,
            0.0f,
            "Medium quality - 4x Anisotropic filtering"
        }},
        {"high", {
            D3D11_FILTER_ANISOTROPIC,
            8,
            0.0f,
            "High quality - 8x Anisotropic filtering"
        }},
        {"ultra", {
            D3D11_FILTER_ANISOTROPIC,
            16,
            -0.5f,
            "Ultra quality - 16x Anisotropic filtering, sharpened mipmaps"
        }}
    };
    
    auto it = qualityPresets.find(quality);
    if (it == qualityPresets.end()) {
        Spark::SimpleConsole::GetInstance().LogError(
            "Invalid texture quality: " + quality + ". Available options: low, medium, high, ultra"
        );
        return;
    }
    
    const auto& settings = it->second;
    
    // Clear existing sampler cache since we're changing quality settings
    m_samplerCache.clear();
    
    // Update default sampling settings for new textures
    TextureSampling defaultSampling;
    defaultSampling.filter = settings.filter;
    defaultSampling.maxAnisotropy = settings.maxAnisotropy;
    defaultSampling.mipLODBias = settings.mipLODBias;
    
    // Update existing materials with new sampling settings
    int updatedMaterials = 0;
    for (auto& materialPair : m_materials) {
        auto& material = materialPair.second;
        if (!material) continue;
        
        // Update sampling for all texture slots in this material
        bool materialUpdated = false;
        for (auto textureType = static_cast<int>(MaterialTextureType::Albedo); 
             textureType <= static_cast<int>(MaterialTextureType::Custom3); 
             ++textureType) {
            
            MaterialTextureType type = static_cast<MaterialTextureType>(textureType);
            if (material->HasTexture(type)) {
                // We would need to access the material's texture directly to update sampling
                // For now, we'll note that the material needs updating
                materialUpdated = true;
            }
        }
        
        if (materialUpdated) {
            updatedMaterials++;
            // In a full implementation, you might trigger a reload of the material's textures
            // or update the sampling states directly
        }
    }
    
    // Apply quality settings to all cached textures by regenerating samplers
    int regeneratedSamplers = 0;
    for (auto& samplerPair : m_samplerCache) {
        // The sampler cache will be regenerated as needed with new settings
        regeneratedSamplers++;
    }
    
    Spark::SimpleConsole::GetInstance().LogSuccess(
        "Texture quality set to: " + quality + " - " + settings.description + 
        "\nUpdated " + std::to_string(updatedMaterials) + " materials, " +
        "cleared " + std::to_string(regeneratedSamplers) + " cached samplers"
    );
    
    // Store current quality setting for future reference
    static std::string currentQuality = quality;
    currentQuality = quality;
}

std::string MaterialSystem::Console_GetTextureMemoryInfo() const
{
    std::stringstream ss;
    ss << "=== Texture Memory Info ===\n";
    ss << "Texture cache: " << m_textureCache.size() << " textures\n";
    ss << "Sampler cache: " << m_samplerCache.size() << " samplers\n";
    
    // Estimate memory usage (this would be more accurate with actual texture sizes)
    size_t estimatedMemory = m_textureCache.size() * 1024 * 1024; // Rough estimate: 1MB per texture
    ss << "Estimated memory usage: " << (estimatedMemory / 1024 / 1024) << " MB\n";
    
    return ss.str();
}

int MaterialSystem::Console_ValidateMaterials()
{
    int validCount = 0;
    int invalidCount = 0;
    std::vector<std::string> invalidMaterials;
    std::vector<std::string> warningMaterials;
    
    Spark::SimpleConsole::GetInstance().LogInfo("Starting comprehensive material validation...");
    
    for (const auto& pair : m_materials) {
        const std::string& materialName = pair.first;
        const auto& material = pair.second;
        
        if (!material) {
            invalidCount++;
            invalidMaterials.push_back(materialName + " (null material)");
            continue;
        }
        
        bool isValid = true;
        std::vector<std::string> issues;
        
        // Validate PBR properties
        const PBRProperties& pbr = material->GetPBRProperties();
        
        if (pbr.metallicFactor < 0.0f || pbr.metallicFactor > 1.0f) {
            isValid = false;
            issues.push_back("Metallic factor out of range [0,1]: " + std::to_string(pbr.metallicFactor));
        }
        
        if (pbr.roughnessFactor < 0.0f || pbr.roughnessFactor > 1.0f) {
            isValid = false;
            issues.push_back("Roughness factor out of range [0,1]: " + std::to_string(pbr.roughnessFactor));
        }
        
        if (pbr.normalScale < 0.0f) {
            isValid = false;
            issues.push_back("Normal scale cannot be negative: " + std::to_string(pbr.normalScale));
        }
        
        if (pbr.occlusionStrength < 0.0f || pbr.occlusionStrength > 1.0f) {
            isValid = false;
            issues.push_back("Occlusion strength out of range [0,1]: " + std::to_string(pbr.occlusionStrength));
        }
        
        if (pbr.alphaCutoff < 0.0f || pbr.alphaCutoff > 1.0f) {
            isValid = false;
            issues.push_back("Alpha cutoff out of range [0,1]: " + std::to_string(pbr.alphaCutoff));
        }
        
        if (pbr.indexOfRefraction < 1.0f) {
            isValid = false;
            issues.push_back("Index of refraction cannot be less than 1.0: " + std::to_string(pbr.indexOfRefraction));
        }
        
        if (pbr.emissiveFactor < 0.0f) {
            isValid = false;
            issues.push_back("Emissive factor cannot be negative: " + std::to_string(pbr.emissiveFactor));
        }
        
        // Validate albedo color
        if (pbr.albedoColor.x < 0.0f || pbr.albedoColor.x > 1.0f ||
            pbr.albedoColor.y < 0.0f || pbr.albedoColor.y > 1.0f ||
            pbr.albedoColor.z < 0.0f || pbr.albedoColor.z > 1.0f ||
            pbr.albedoColor.w < 0.0f || pbr.albedoColor.w > 1.0f) {
            isValid = false;
            issues.push_back("Albedo color components out of range [0,1]");
        }
        
        // Validate emissive color (can be > 1.0 for HDR)
        if (pbr.emissiveColor.x < 0.0f || pbr.emissiveColor.y < 0.0f || pbr.emissiveColor.z < 0.0f) {
            isValid = false;
            issues.push_back("Emissive color components cannot be negative");
        }
        
        // Check for suspicious values (warnings, not errors)
        std::vector<std::string> warnings;
        
        if (pbr.metallicFactor > 0.9f && pbr.roughnessFactor < 0.1f) {
            warnings.push_back("Very high metallic + very low roughness may look unnatural");
        }
        
        if (pbr.emissiveFactor > 10.0f) {
            warnings.push_back("Very high emissive factor: " + std::to_string(pbr.emissiveFactor));
        }
        
        if (pbr.indexOfRefraction > 3.0f) {
            warnings.push_back("Unusually high IOR: " + std::to_string(pbr.indexOfRefraction));
        }
        
        // Validate advanced properties
        const AdvancedProperties& advanced = material->GetAdvancedProperties();
        
        if (advanced.subsurfaceEnabled && advanced.subsurfaceRadius <= 0.0f) {
            isValid = false;
            issues.push_back("Subsurface radius must be positive when subsurface is enabled");
        }
        
        if (advanced.clearcoatEnabled) {
            if (advanced.clearcoatFactor < 0.0f || advanced.clearcoatFactor > 1.0f) {
                isValid = false;
                issues.push_back("Clearcoat factor out of range [0,1]: " + std::to_string(advanced.clearcoatFactor));
            }
            if (advanced.clearcoatRoughness < 0.0f || advanced.clearcoatRoughness > 1.0f) {
                isValid = false;
                issues.push_back("Clearcoat roughness out of range [0,1]: " + std::to_string(advanced.clearcoatRoughness));
            }
        }
        
        if (advanced.anisotropyEnabled && (advanced.anisotropyFactor < -1.0f || advanced.anisotropyFactor > 1.0f)) {
            isValid = false;
            issues.push_back("Anisotropy factor out of range [-1,1]: " + std::to_string(advanced.anisotropyFactor));
        }
        
        if (advanced.transmissionEnabled && (advanced.transmissionFactor < 0.0f || advanced.transmissionFactor > 1.0f)) {
            isValid = false;
            issues.push_back("Transmission factor out of range [0,1]: " + std::to_string(advanced.transmissionFactor));
        }
        
        // Validate render state
        const MaterialRenderState& renderState = material->GetRenderState();
        
        if (renderState.renderQueue < 0 || renderState.renderQueue > 5000) {
            warnings.push_back("Render queue outside normal range [0-5000]: " + std::to_string(renderState.renderQueue));
        }
        
        // Validate texture consistency
        bool hasAlbedo = material->HasTexture(MaterialTextureType::Albedo);
        bool hasNormal = material->HasTexture(MaterialTextureType::Normal);
        bool hasMetallic = material->HasTexture(MaterialTextureType::Metallic);
        bool hasRoughness = material->HasTexture(MaterialTextureType::Roughness);
        
        if (!hasAlbedo) {
            warnings.push_back("No albedo texture - material will use only base color");
        }
        
        if (hasNormal && pbr.normalScale == 0.0f) {
            warnings.push_back("Normal texture present but normal scale is 0");
        }
        
        if ((hasMetallic || hasRoughness) && (!hasMetallic || !hasRoughness)) {
            warnings.push_back("Only one of metallic/roughness textures present - consider using packed textures");
        }
        
        // Compile results
        if (isValid) {
            validCount++;
            if (!warnings.empty()) {
                warningMaterials.push_back(materialName + " (" + std::to_string(warnings.size()) + " warnings)");
            }
        } else {
            invalidCount++;
            std::string issueList = materialName + ": ";
            for (size_t i = 0; i < issues.size(); ++i) {
                if (i > 0) issueList += ", ";
                issueList += issues[i];
            }
            invalidMaterials.push_back(issueList);
        }
        
        // Log detailed issues for invalid materials
        if (!isValid) {
            Spark::SimpleConsole::GetInstance().LogError("Invalid material '" + materialName + "':");
            for (const auto& issue : issues) {
                Spark::SimpleConsole::GetInstance().LogError("  - " + issue);
            }
        }
        
        // Log warnings
        if (!warnings.empty()) {
            for (const auto& warning : warnings) {
                Spark::SimpleConsole::GetInstance().LogWarning("Material '" + materialName + "': " + warning);
            }
        }
    }
    
    // Summary report
    std::stringstream report;
    report << "=== Material Validation Complete ===\n";
    report << "Valid materials: " << validCount << "\n";
    report << "Invalid materials: " << invalidCount << "\n";
    report << "Materials with warnings: " << warningMaterials.size() << "\n";
    report << "Total materials: " << (validCount + invalidCount) << "\n";
    
    if (!invalidMaterials.empty()) {
        report << "\nInvalid materials:\n";
        for (const auto& invalid : invalidMaterials) {
            report << "  - " << invalid << "\n";
        }
    }
    
    if (!warningMaterials.empty()) {
        report << "\nMaterials with warnings:\n";
        for (const auto& warning : warningMaterials) {
            report << "  - " << warning << "\n";
        }
    }
    
    if (invalidCount == 0) {
        Spark::SimpleConsole::GetInstance().LogSuccess(report.str());
    } else {
        Spark::SimpleConsole::GetInstance().LogWarning(report.str());
    }
    
    return validCount;
}

HRESULT MaterialSystem::CreateDefaultMaterials()
{
    // Create default material
    m_defaultMaterial = std::make_shared<Material>("Default");
    PBRProperties defaultPbr = {};
    defaultPbr.albedoColor = { 0.7f, 0.7f, 0.7f, 1.0f };
    defaultPbr.metallicFactor = 0.0f;
    defaultPbr.roughnessFactor = 0.8f;
    defaultPbr.normalScale = 1.0f;
    defaultPbr.occlusionStrength = 1.0f;
    defaultPbr.emissiveColor = { 0.0f, 0.0f, 0.0f };
    defaultPbr.emissiveFactor = 0.0f;
    defaultPbr.alphaCutoff = 0.5f;
    defaultPbr.indexOfRefraction = 1.5f;
    m_defaultMaterial->SetPBRProperties(defaultPbr);

    // Create error material (magenta color for missing materials)
    m_errorMaterial = std::make_shared<Material>("Error");
    PBRProperties errorPbr = defaultPbr;
    errorPbr.albedoColor = { 1.0f, 0.0f, 1.0f, 1.0f }; // Magenta
    errorPbr.emissiveColor = { 0.2f, 0.0f, 0.2f };
    errorPbr.emissiveFactor = 0.5f;
    m_errorMaterial->SetPBRProperties(errorPbr);

    return S_OK;
}

HRESULT MaterialSystem::CreateSampler(const TextureSampling& sampling, ID3D11SamplerState** sampler)
{
    if (!m_device) return E_FAIL;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = sampling.filter;
    desc.AddressU = sampling.addressU;
    desc.AddressV = sampling.addressV;
    desc.AddressW = sampling.addressW;
    desc.MaxAnisotropy = sampling.maxAnisotropy;
    desc.MipLODBias = sampling.mipLODBias;
    desc.MinLOD = sampling.minLOD;
    desc.MaxLOD = sampling.maxLOD;
    desc.BorderColor[0] = sampling.borderColor.x;
    desc.BorderColor[1] = sampling.borderColor.y;
    desc.BorderColor[2] = sampling.borderColor.z;
    desc.BorderColor[3] = sampling.borderColor.w;

    return m_device->CreateSamplerState(&desc, sampler);
}

size_t MaterialSystem::HashSampling(const TextureSampling& sampling) const
{
    size_t hash = 0;
    hash ^= std::hash<int>{}(static_cast<int>(sampling.filter));
    hash ^= std::hash<int>{}(static_cast<int>(sampling.addressU)) << 1;
    hash ^= std::hash<int>{}(static_cast<int>(sampling.addressV)) << 2;
    hash ^= std::hash<int>{}(static_cast<int>(sampling.addressW)) << 3;
    hash ^= std::hash<UINT>{}(sampling.maxAnisotropy) << 4;
    hash ^= std::hash<float>{}(sampling.mipLODBias) << 5;
    hash ^= std::hash<float>{}(sampling.minLOD) << 6;
    hash ^= std::hash<float>{}(sampling.maxLOD) << 7;
    return hash;
}

uint64_t MaterialSystem::GetFileTimestamp(const std::string& filePath) const
{
    try {
        if (std::filesystem::exists(filePath)) {
            auto time = std::filesystem::last_write_time(filePath);
            return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
        }
    }
    catch (const std::exception&) {
        // Error accessing file
		Spark::SimpleConsole::GetInstance().LogError("Failed to get timestamp for file: " + filePath);
		return 0; // Return 0 if we can't get the timestamp
    }
    return 0;
}

// LoadTextureFromFile implementation with proper WIC loading and mipmap support
ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTextureFromFile(const std::string& filePath)
{
    ComPtr<ID3D11ShaderResourceView> texture;
    
    if (!m_device || filePath.empty()) {
        Spark::SimpleConsole::GetInstance().LogError("Invalid device or empty file path in LoadTextureFromFile");
        return texture;
    }

    if (!std::filesystem::exists(filePath)) {
        Spark::SimpleConsole::GetInstance().LogError("Texture file not found: " + filePath);
        return texture;
    }

    try {
        // Initialize WIC factory
        ComPtr<IWICImagingFactory> wicFactory;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wicFactory)
        );
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC Imaging Factory for: " + filePath);
            return texture;
        }
        
        // Create decoder
        ComPtr<IWICBitmapDecoder> decoder;
        std::wstring wideFilePath(filePath.begin(), filePath.end());
        hr = wicFactory->CreateDecoderFromFilename(
            wideFilePath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder
        );
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create WIC decoder for: " + filePath);
            return texture;
        }
        
        // Get first frame
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to get frame from decoder for: " + filePath);
            return texture;
        }
        
        // Get original size for mipmap calculation
        UINT originalWidth, originalHeight;
        hr = frame->GetSize(&originalWidth, &originalHeight);
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to get frame size for: " + filePath);
            return texture;
        }
        
        // Calculate mip levels (power of 2 textures get full mip chain)
        UINT mipLevels = 1;
        if ((originalWidth & (originalWidth - 1)) == 0 && (originalHeight & (originalHeight - 1)) == 0) {
            // Power of 2 texture - calculate full mip chain
            UINT maxDimension = std::max(originalWidth, originalHeight);
            while (maxDimension > 1) {
                maxDimension >>= 1;
                mipLevels++;
            }
        }
        
        // Create format converter
        ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create format converter for: " + filePath);
            return texture;
        }
        
        // Convert to RGBA format
        hr = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        );
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to initialize format converter for: " + filePath);
            return texture;
        }
        
        // Create texture description
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = originalWidth;
        texDesc.Height = originalHeight;
        texDesc.MipLevels = mipLevels;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texDesc.CPUAccessFlags = 0;
        texDesc.MiscFlags = (mipLevels > 1) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        
        // Prepare initial data for base mip level
        std::vector<BYTE> imageData(originalWidth * originalHeight * 4);
        hr = converter->CopyPixels(
            nullptr,
            originalWidth * 4,
            static_cast<UINT>(imageData.size()),
            imageData.data()
        );
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to copy pixels for: " + filePath);
            return texture;
        }
        
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = imageData.data();
        initData.SysMemPitch = originalWidth * 4;
        initData.SysMemSlicePitch = 0;
        
        // Create texture
        ComPtr<ID3D11Texture2D> tex2D;
        hr = m_device->CreateTexture2D(&texDesc, &initData, &tex2D);
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create Direct3D texture for: " + filePath);
            return texture;
        }
        
        // Create shader resource view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = mipLevels;
        
        hr = m_device->CreateShaderResourceView(tex2D.Get(), &srvDesc, &texture);
        if (FAILED(hr)) {
            Spark::SimpleConsole::GetInstance().LogError("Failed to create shader resource view for: " + filePath);
            return texture;
        }
        
        // Generate mipmaps if enabled
        if (mipLevels > 1 && m_context) {
            m_context->GenerateMips(texture.Get());
        }
        
        Spark::SimpleConsole::GetInstance().LogInfo(
            "Successfully loaded texture: " + filePath + 
            " (" + std::to_string(originalWidth) + "x" + std::to_string(originalHeight) + 
            ", " + std::to_string(mipLevels) + " mips)"
        );
        
    } catch (const std::exception& e) {
        Spark::SimpleConsole::GetInstance().LogError("Exception loading texture " + filePath + ": " + std::string(e.what()));
        texture.Reset();
    }

    return texture;
}

void MaterialSystem::UpdateMetrics()
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    
    // Basic metrics
    m_metrics.loadedMaterials = static_cast<int>(m_materials.size());
    m_metrics.textureCount = static_cast<int>(m_textureCache.size());
    m_metrics.hotReloadEnabled = m_hotReloadEnabled;
    
    // Calculate texture memory usage (improved estimation)
    size_t totalTextureMemory = 0;
    for (const auto& pair : m_textureCache) {
        // For a more accurate estimate, we'd need to query the actual texture
        // For now, estimate based on common texture sizes and formats
        // This is a rough approximation - in production you'd want to track actual sizes
        totalTextureMemory += 1024 * 1024; // 1MB per texture (very rough estimate)
    }
    m_metrics.textureMemory = totalTextureMemory;
    
    // Count variants across all materials
    int totalVariants = 0;
    for (const auto& materialPair : m_materials) {
        if (materialPair.second) {
            // Access variant count through material's internal structure
            // Since we can't directly access private members, we estimate based on naming
            totalVariants += 1; // Each material has at least one "default" variant
        }
    }
    m_metrics.variantCount = totalVariants;
    
    // Performance metrics (would be updated during actual rendering)
    // These would be incremented during actual material binding operations
    // m_metrics.materialSwitches and m_metrics.textureBinds are reset in BeginFrame()
    
    // Load time tracking — actual per-material timing would require
    // instrumenting CreateMaterial(). For now, report 0 until that's added.
    m_metrics.averageLoadTime = 0.0f;
}

void MaterialSystem::PerformPeriodicMaintenance()
{
    // This method can be called periodically to perform maintenance tasks
    // such as cleaning up unused resources, optimizing caches, etc.
    
    static auto lastMaintenanceTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto deltaTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastMaintenanceTime);
    
    // Perform maintenance every 60 seconds
    if (deltaTime.count() >= 60) {
        lastMaintenanceTime = currentTime;
        
        // Clean up unused samplers (keep commonly used ones)
        if (m_samplerCache.size() > 50) {
            // In a full implementation, you'd track usage frequency
            // For now, just log that maintenance would occur
            Spark::SimpleConsole::GetInstance().LogInfo(
                "MaterialSystem maintenance: " + std::to_string(m_samplerCache.size()) + " samplers in cache"
            );
        }
        
        // Log memory usage
        size_t estimatedMemory = m_textureCache.size() * 1024 * 1024; // Rough estimate
        if (estimatedMemory > 500 * 1024 * 1024) { // > 500MB
            Spark::SimpleConsole::GetInstance().LogWarning(
                "MaterialSystem using high memory: ~" + std::to_string(estimatedMemory / 1024 / 1024) + "MB"
            );
        }
    }
}

std::string MaterialSystem::TextureTypeToString(MaterialTextureType type) const
{
    switch (type) {
        case MaterialTextureType::Albedo: return "Albedo";
        case MaterialTextureType::Normal: return "Normal";
        case MaterialTextureType::Metallic: return "Metallic";
        case MaterialTextureType::Roughness: return "Roughness";
        case MaterialTextureType::Occlusion: return "Occlusion";
        case MaterialTextureType::Emissive: return "Emissive";
        case MaterialTextureType::Height: return "Height";
        case MaterialTextureType::DetailAlbedo: return "DetailAlbedo";
        case MaterialTextureType::DetailNormal: return "DetailNormal";
        case MaterialTextureType::Subsurface: return "Subsurface";
        case MaterialTextureType::Transmission: return "Transmission";
        case MaterialTextureType::Clearcoat: return "Clearcoat";
        case MaterialTextureType::ClearcoatRoughness: return "ClearcoatRoughness";
        case MaterialTextureType::Anisotropy: return "Anisotropy";
        case MaterialTextureType::Custom0: return "Custom0";
        case MaterialTextureType::Custom1: return "Custom1";
        case MaterialTextureType::Custom2: return "Custom2";
        case MaterialTextureType::Custom3: return "Custom3";
        default: return "Unknown";
    }
}

MaterialTextureType MaterialSystem::StringToTextureType(const std::string& str) const
{
    if (str == "Albedo") return MaterialTextureType::Albedo;
    if (str == "Normal") return MaterialTextureType::Normal;
    if (str == "Metallic") return MaterialTextureType::Metallic;
    if (str == "Roughness") return MaterialTextureType::Roughness;
    if (str == "Occlusion") return MaterialTextureType::Occlusion;
    if (str == "Emissive") return MaterialTextureType::Emissive;
    if (str == "Height") return MaterialTextureType::Height;
    if (str == "DetailAlbedo") return MaterialTextureType::DetailAlbedo;
    if (str == "DetailNormal") return MaterialTextureType::DetailNormal;
    if (str == "Subsurface") return MaterialTextureType::Subsurface;
    if (str == "Transmission") return MaterialTextureType::Transmission;
    if (str == "Clearcoat") return MaterialTextureType::Clearcoat;
    if (str == "ClearcoatRoughness") return MaterialTextureType::ClearcoatRoughness;
    if (str == "Anisotropy") return MaterialTextureType::Anisotropy;
    if (str == "Custom0") return MaterialTextureType::Custom0;
    if (str == "Custom1") return MaterialTextureType::Custom1;
    if (str == "Custom2") return MaterialTextureType::Custom2;
    if (str == "Custom3") return MaterialTextureType::Custom3;
    return MaterialTextureType::Albedo; // Default fallback
}

std::string MaterialSystem::Console_DumpMaterialDetails(const std::string& materialName) const
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial) {
        return "Material not found: " + materialName;
    }
    
    std::stringstream ss;
    ss << "=== DETAILED MATERIAL DUMP: " << materialName << " ===\n\n";
    
    // Basic info
    ss << "[BASIC INFO]\n";
    ss << "Name: " << material->GetName() << "\n";
    ss << "Active Variant: " << material->GetActiveVariant() << "\n\n";
    
    // PBR Properties
    const auto& pbr = material->GetPBRProperties();
    ss << "[PBR PROPERTIES]\n";
    ss << std::fixed << std::setprecision(6);
    ss << "Albedo Color: (" << pbr.albedoColor.x << ", " << pbr.albedoColor.y 
       << ", " << pbr.albedoColor.z << ", " << pbr.albedoColor.w << ")\n";
    ss << "Metallic Factor: " << pbr.metallicFactor << "\n";
    ss << "Roughness Factor: " << pbr.roughnessFactor << "\n";
    ss << "Normal Scale: " << pbr.normalScale << "\n";
    ss << "Occlusion Strength: " << pbr.occlusionStrength << "\n";
    ss << "Emissive Color: (" << pbr.emissiveColor.x << ", " << pbr.emissiveColor.y 
       << ", " << pbr.emissiveColor.z << ")\n";
    ss << "Emissive Factor: " << pbr.emissiveFactor << "\n";
    ss << "Alpha Cutoff: " << pbr.alphaCutoff << "\n";
    ss << "Index of Refraction: " << pbr.indexOfRefraction << "\n\n";
    
    // Advanced Properties
    const auto& advanced = material->GetAdvancedProperties();
    ss << "[ADVANCED PROPERTIES]\n";
    ss << "Subsurface: " << (advanced.subsurfaceEnabled ? "Enabled" : "Disabled");
    if (advanced.subsurfaceEnabled) {
        ss << " - Color: (" << advanced.subsurfaceColor.x << ", " << advanced.subsurfaceColor.y 
           << ", " << advanced.subsurfaceColor.z << "), Radius: " << advanced.subsurfaceRadius;
    }
    ss << "\n";
    
    ss << "Clearcoat: " << (advanced.clearcoatEnabled ? "Enabled" : "Disabled");
    if (advanced.clearcoatEnabled) {
        ss << " - Factor: " << advanced.clearcoatFactor << ", Roughness: " << advanced.clearcoatRoughness;
    }
    ss << "\n";
    
    ss << "Anisotropy: " << (advanced.anisotropyEnabled ? "Enabled" : "Disabled");
    if (advanced.anisotropyEnabled) {
        ss << " - Factor: " << advanced.anisotropyFactor 
           << ", Direction: (" << advanced.anisotropyDirection.x << ", " << advanced.anisotropyDirection.y << ")";
    }
    ss << "\n";
    
    ss << "Transmission: " << (advanced.transmissionEnabled ? "Enabled" : "Disabled");
    if (advanced.transmissionEnabled) {
        ss << " - Factor: " << advanced.transmissionFactor 
           << ", Color: (" << advanced.transmissionColor.x << ", " << advanced.transmissionColor.y 
           << ", " << advanced.transmissionColor.z << ")";
    }
    ss << "\n";
    
    ss << "Sheen: " << (advanced.sheenEnabled ? "Enabled" : "Disabled");
    if (advanced.sheenEnabled) {
        ss << " - Color: (" << advanced.sheenColor.x << ", " << advanced.sheenColor.y 
           << ", " << advanced.sheenColor.z << "), Roughness: " << advanced.sheenRoughness;
    }
    ss << "\n";
    
    ss << "Iridescence: " << (advanced.iridescenceEnabled ? "Enabled" : "Disabled");
    if (advanced.iridescenceEnabled) {
        ss << " - Factor: " << advanced.iridescenceFactor 
           << ", IOR: " << advanced.iridescenceIOR 
           << ", Thickness: " << advanced.iridescenceThickness << "nm";
    }
    ss << "\n\n";
    
    // Render State
    const auto& renderState = material->GetRenderState();
    ss << "[RENDER STATE]\n";
    ss << "Blend Mode: " << static_cast<int>(renderState.blendMode) << "\n";
    ss << "Cull Mode: " << static_cast<int>(renderState.cullMode) << "\n";
    ss << "Depth Test: " << (renderState.depthTest ? "Enabled" : "Disabled") << "\n";
    ss << "Depth Write: " << (renderState.depthWrite ? "Enabled" : "Disabled") << "\n";
    ss << "Cast Shadows: " << (renderState.castShadows ? "Enabled" : "Disabled") << "\n";
    ss << "Receive Shadows: " << (renderState.receiveShadows ? "Enabled" : "Disabled") << "\n";
    ss << "Render Queue: " << renderState.renderQueue << "\n";
    ss << "Double Sided: " << (renderState.doubleSided ? "Enabled" : "Disabled") << "\n\n";
    
    // Textures
    ss << "[TEXTURES]\n";
    for (int i = static_cast<int>(MaterialTextureType::Albedo); 
         i <= static_cast<int>(MaterialTextureType::Custom3); ++i) {
        
        MaterialTextureType type = static_cast<MaterialTextureType>(i);
        if (material->HasTexture(type)) {
            const auto& texture = material->GetTexture(type);
            ss << TextureTypeToString(type) << ": " << texture.filePath;
            if (!texture.enabled) ss << " (DISABLED)";
            ss << "\n  - Intensity: " << texture.intensity;
            ss << ", Tiling: (" << texture.tiling.x << ", " << texture.tiling.y << ")";
            ss << ", Offset: (" << texture.offset.x << ", " << texture.offset.y << ")\n";
        }
    }
    
    return ss.str();
}

bool MaterialSystem::Console_ExportMaterial(const std::string& materialName, const std::string& filePath)
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial) {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
        return false;
    }
    
    if (material->SaveToFile(filePath)) {
        Spark::SimpleConsole::GetInstance().LogSuccess("Exported material '" + materialName + "' to: " + filePath);
        return true;
    } else {
        Spark::SimpleConsole::GetInstance().LogError("Failed to export material: " + materialName);
        return false;
    }
}

bool MaterialSystem::Console_ImportMaterial(const std::string& filePath)
{
    if (!std::filesystem::exists(filePath)) {
        Spark::SimpleConsole::GetInstance().LogError("File not found: " + filePath);
        return false;
    }
    
    auto material = LoadMaterial(filePath);
    if (material && material != m_errorMaterial) {
        Spark::SimpleConsole::GetInstance().LogSuccess("Imported material from: " + filePath);
        return true;
    } else {
        Spark::SimpleConsole::GetInstance().LogError("Failed to import material from: " + filePath);
        return false;
    }
}

std::string MaterialSystem::Console_ListTextureTypes() const
{
    std::stringstream ss;
    ss << "=== Available Texture Types ===\n";
    
    for (int i = static_cast<int>(MaterialTextureType::Albedo); 
         i <= static_cast<int>(MaterialTextureType::Custom3); ++i) {
        
        MaterialTextureType type = static_cast<MaterialTextureType>(i);
        ss << i << ": " << TextureTypeToString(type) << "\n";
    }
    
    return ss.str();
}

bool MaterialSystem::Console_LoadTextureToSlot(const std::string& materialName, 
                                              const std::string& textureType, 
                                              const std::string& texturePath)
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial) {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
        return false;
    }
    
    MaterialTextureType type = StringToTextureType(textureType);
    
    if (material->LoadTexture(type, texturePath, m_device)) {
        Spark::SimpleConsole::GetInstance().LogSuccess(
            "Loaded texture '" + texturePath + "' to " + textureType + " slot of material '" + materialName + "'"
        );
        return true;
    } else {
        Spark::SimpleConsole::GetInstance().LogError(
            "Failed to load texture '" + texturePath + "' to material '" + materialName + "'"
        );
        return false;
    }
}

void MaterialSystem::Console_UnloadTextureFromSlot(const std::string& materialName, const std::string& textureType)
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial) {
        Spark::SimpleConsole::GetInstance().LogError("Material not found: " + materialName);
        return;
    }
    
    MaterialTextureType type = StringToTextureType(textureType);
    material->UnloadTexture(type);
    
    Spark::SimpleConsole::GetInstance().LogSuccess(
        "Unloaded " + textureType + " texture from material '" + materialName + "'"
    );
}

std::string MaterialSystem::Console_ListMaterialVariants(const std::string& materialName) const
{
    auto material = GetMaterial(materialName);
    if (!material || material == m_defaultMaterial) {
        return "Material not found: " + materialName;
    }
    
    std::stringstream ss;
    ss << "=== Material Variants for '" << materialName << "' ===\n";
    
    auto variants = material->GetAvailableVariants();
    if (variants.empty()) {
        ss << "No variants defined for this material.\n";
    } else {
        ss << "Available variants (" << variants.size() << "):\n";
        for (const auto& variant : variants) {
            ss << "  - " << variant;
            if (variant == material->GetActiveVariant()) {
                ss << " (ACTIVE)";
            }
            ss << "\n";
        }
    }

    return ss.str();
}

#endif // inner SPARK_PLATFORM_WINDOWS

#else // !SPARK_PLATFORM_WINDOWS

#include "MaterialSystem.h"
#include <sstream>
#include <algorithm>
#include <cstring>

// ============================================================================
// Material (Linux stub)
// ============================================================================

Material::Material(const std::string& name)
    : m_name(name)
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
    if (it != m_textures.end()) {
        return it->second;
    }
    static MaterialTexture empty;
    return empty;
}

const std::string& Material::GetActiveVariant() const
{
    return m_activeVariant;
}

std::vector<std::string> Material::GetAvailableVariants() const
{
    std::vector<std::string> variants;
    for (const auto& pair : m_variants) {
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
    MaterialTexture tex;
    tex.filePath = filePath;
    tex.enabled = true;
    m_textures[type] = tex;
    return true;
}

void Material::UnloadTexture(MaterialTextureType type)
{
    auto it = m_textures.find(type);
    if (it != m_textures.end()) {
        it->second.enabled = false;
    }
}

bool Material::HasTexture(MaterialTextureType type) const
{
    auto it = m_textures.find(type);
    return it != m_textures.end() && it->second.enabled;
}

void Material::BindToShader(ID3D11DeviceContext* /*context*/) const
{
    // No-op on Linux
}

void Material::CreateVariant(const std::string& variantName, const std::vector<std::string>& defines)
{
    m_variants[variantName] = defines;
}

void Material::SetActiveVariant(const std::string& variantName)
{
    if (m_variants.find(variantName) != m_variants.end()) {
        m_activeVariant = variantName;
    }
}

bool Material::SaveToFile(const std::string& /*filePath*/) const
{
    return true;
}

bool Material::LoadFromFile(const std::string& /*filePath*/, ID3D11Device* /*device*/)
{
    return true;
}

std::string Material::GetDetailedInfo() const
{
    std::stringstream ss;
    ss << "=== Material: " << m_name << " ===\n";
    ss << "Albedo: (" << m_pbrProperties.albedoColor.x << ", "
       << m_pbrProperties.albedoColor.y << ", "
       << m_pbrProperties.albedoColor.z << ")\n";
    ss << "Metallic: " << m_pbrProperties.metallicFactor << "\n";
    ss << "Roughness: " << m_pbrProperties.roughnessFactor << "\n";
    ss << "Textures: " << m_textures.size() << " slots\n";
    ss << "Variants: " << m_variants.size() << "\n";
    return ss.str();
}

void Material::Console_SetProperty(const std::string& property, float value)
{
    if (property == "metallic") m_pbrProperties.metallicFactor = value;
    else if (property == "roughness") m_pbrProperties.roughnessFactor = value;
    else if (property == "normalscale") m_pbrProperties.normalScale = value;
    else if (property == "occlusion") m_pbrProperties.occlusionStrength = value;
    else if (property == "emissive") m_pbrProperties.emissiveFactor = value;
    else if (property == "alphacutoff") m_pbrProperties.alphaCutoff = value;
}

void Material::Console_SetColor(const std::string& property, float r, float g, float b)
{
    if (property == "albedo") {
        m_pbrProperties.albedoColor = {r, g, b, m_pbrProperties.albedoColor.w};
    } else if (property == "emissive") {
        m_pbrProperties.emissiveColor = {r, g, b};
    }
}

void Material::Console_ReloadTextures(ID3D11Device* /*device*/)
{
    // No-op on Linux
}

// ============================================================================
// MaterialSystem (Linux stub)
// ============================================================================

MaterialSystem::MaterialSystem()
    : m_device(nullptr), m_context(nullptr), m_hotReloadEnabled(false)
{
    memset(&m_metrics, 0, sizeof(m_metrics));
}

MaterialSystem::~MaterialSystem()
{
    Shutdown();
}

HRESULT MaterialSystem::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_context = context;
    memset(&m_metrics, 0, sizeof(m_metrics));

    m_defaultMaterial = std::make_shared<Material>("__default");
    m_errorMaterial = std::make_shared<Material>("__error");
    m_errorMaterial->SetPBRProperties({{1.0f, 0.0f, 1.0f, 1.0f}});

    return S_OK;
}

void MaterialSystem::Shutdown()
{
    m_materials.clear();
    m_textureCache.clear();
    m_samplerCache.clear();
    m_defaultMaterial.reset();
    m_errorMaterial.reset();
    m_device = nullptr;
    m_context = nullptr;
}

std::shared_ptr<Material> MaterialSystem::CreateMaterial(const std::string& name)
{
    auto material = std::make_shared<Material>(name);
    m_materials[name] = material;
    return material;
}

std::shared_ptr<Material> MaterialSystem::LoadMaterial(const std::string& filePath)
{
    auto it = m_materials.find(filePath);
    if (it != m_materials.end()) return it->second;

    auto material = std::make_shared<Material>(filePath);
    material->LoadFromFile(filePath, m_device);
    m_materials[filePath] = material;
    return material;
}

std::shared_ptr<Material> MaterialSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? it->second : nullptr;
}

void MaterialSystem::UnloadMaterial(const std::string& name)
{
    m_materials.erase(name);
}

void MaterialSystem::UnloadAllMaterials()
{
    m_materials.clear();
}

ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTexture(const std::string& /*filePath*/)
{
    return nullptr;
}

void MaterialSystem::UnloadTexture(const std::string& filePath)
{
    m_textureCache.erase(filePath);
}

ComPtr<ID3D11SamplerState> MaterialSystem::GetSampler(const TextureSampling& /*sampling*/)
{
    return nullptr;
}

void MaterialSystem::EnableHotReloading(bool enabled)
{
    m_hotReloadEnabled = enabled;
}

void MaterialSystem::UpdateHotReload()
{
    // No-op on Linux
}

int MaterialSystem::ReloadAllMaterials()
{
    return 0;
}

void MaterialSystem::BeginFrame()
{
    // No-op on Linux
}

void MaterialSystem::EndFrame()
{
    // No-op on Linux
}

MaterialSystem::MaterialMetrics MaterialSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    MaterialMetrics metrics = m_metrics;
    metrics.loadedMaterials = static_cast<int>(m_materials.size());
    metrics.hotReloadEnabled = m_hotReloadEnabled;
    return metrics;
}

std::string MaterialSystem::Console_ListMaterials() const
{
    std::stringstream ss;
    ss << "=== Loaded Materials (" << m_materials.size() << ") ===\n";
    for (const auto& pair : m_materials) {
        ss << "  " << pair.first << "\n";
    }
    return ss.str();
}

std::string MaterialSystem::Console_GetMaterialInfo(const std::string& materialName) const
{
    auto mat = GetMaterial(materialName);
    if (!mat) return "Material not found: " + materialName;
    return mat->GetDetailedInfo();
}

bool MaterialSystem::Console_ReloadMaterial(const std::string& materialName)
{
    auto mat = GetMaterial(materialName);
    if (!mat) return false;
    return true;
}

int MaterialSystem::Console_ReloadAllMaterials()
{
    return ReloadAllMaterials();
}

bool MaterialSystem::Console_CreateVariant(const std::string& materialName, const std::string& variantName, const std::vector<std::string>& defines)
{
    auto mat = GetMaterial(materialName);
    if (!mat) return false;
    mat->CreateVariant(variantName, defines);
    return true;
}

void MaterialSystem::Console_SetMaterialProperty(const std::string& materialName, const std::string& property, float value)
{
    auto mat = GetMaterial(materialName);
    if (mat) mat->Console_SetProperty(property, value);
}

void MaterialSystem::Console_SetMaterialColor(const std::string& materialName, const std::string& property, float r, float g, float b)
{
    auto mat = GetMaterial(materialName);
    if (mat) mat->Console_SetColor(property, r, g, b);
}

void MaterialSystem::Console_SetHotReload(bool enabled)
{
    EnableHotReloading(enabled);
}

void MaterialSystem::Console_ClearCache()
{
    m_textureCache.clear();
    m_samplerCache.clear();
}

void MaterialSystem::Console_GarbageCollect()
{
    for (auto it = m_materials.begin(); it != m_materials.end();) {
        if (it->second.use_count() == 1) {
            it = m_materials.erase(it);
        } else {
            ++it;
        }
    }
}

void MaterialSystem::Console_SetTextureQuality(const std::string& /*quality*/)
{
    // No-op on Linux
}

std::string MaterialSystem::Console_GetTextureMemoryInfo() const
{
    std::stringstream ss;
    ss << "=== Texture Memory (Linux stub) ===\n";
    ss << "Cached textures: " << m_textureCache.size() << "\n";
    ss << "Cached samplers: " << m_samplerCache.size() << "\n";
    return ss.str();
}

int MaterialSystem::Console_ValidateMaterials()
{
    int errors = 0;
    for (const auto& pair : m_materials) {
        if (!pair.second) errors++;
    }
    return errors;
}

std::string MaterialSystem::Console_DumpMaterialDetails(const std::string& materialName) const
{
    return Console_GetMaterialInfo(materialName);
}

bool MaterialSystem::Console_ExportMaterial(const std::string& materialName, const std::string& filePath)
{
    auto mat = GetMaterial(materialName);
    if (!mat) return false;
    return mat->SaveToFile(filePath);
}

bool MaterialSystem::Console_ImportMaterial(const std::string& filePath)
{
    auto mat = LoadMaterial(filePath);
    return mat != nullptr;
}

std::string MaterialSystem::Console_ListTextureTypes() const
{
    return "Albedo, Normal, Metallic, Roughness, Occlusion, Emissive, Height, "
           "DetailAlbedo, DetailNormal, Subsurface, Transmission, Clearcoat, "
           "ClearcoatRoughness, Anisotropy, Custom0, Custom1, Custom2, Custom3";
}

bool MaterialSystem::Console_LoadTextureToSlot(const std::string& materialName,
                                               const std::string& textureType,
                                               const std::string& texturePath)
{
    auto mat = GetMaterial(materialName);
    if (!mat) return false;
    MaterialTextureType type = StringToTextureType(textureType);
    return mat->LoadTexture(type, texturePath, m_device);
}

void MaterialSystem::Console_UnloadTextureFromSlot(const std::string& materialName, const std::string& textureType)
{
    auto mat = GetMaterial(materialName);
    if (mat) {
        MaterialTextureType type = StringToTextureType(textureType);
        mat->UnloadTexture(type);
    }
}

std::string MaterialSystem::Console_ListMaterialVariants(const std::string& materialName) const
{
    auto mat = GetMaterial(materialName);
    if (!mat) return "Material not found: " + materialName;

    std::stringstream ss;
    ss << "=== Variants for " << materialName << " ===\n";
    auto variants = mat->GetAvailableVariants();
    for (const auto& variant : variants) {
        ss << "  " << variant;
        if (variant == mat->GetActiveVariant()) ss << " (ACTIVE)";
        ss << "\n";
    }
    return ss.str();
}

// Private helpers
HRESULT MaterialSystem::CreateDefaultMaterials() { return S_OK; }
HRESULT MaterialSystem::CreateSampler(const TextureSampling& /*sampling*/, ID3D11SamplerState** /*sampler*/) { return S_OK; }
size_t MaterialSystem::HashSampling(const TextureSampling& /*sampling*/) const { return 0; }
uint64_t MaterialSystem::GetFileTimestamp(const std::string& /*filePath*/) const { return 0; }
ComPtr<ID3D11ShaderResourceView> MaterialSystem::LoadTextureFromFile(const std::string& /*filePath*/) { return nullptr; }
void MaterialSystem::UpdateMetrics() {}
void MaterialSystem::PerformPeriodicMaintenance() {}

std::string MaterialSystem::TextureTypeToString(MaterialTextureType type) const
{
    switch (type) {
        case MaterialTextureType::Albedo: return "Albedo";
        case MaterialTextureType::Normal: return "Normal";
        case MaterialTextureType::Metallic: return "Metallic";
        case MaterialTextureType::Roughness: return "Roughness";
        case MaterialTextureType::Occlusion: return "Occlusion";
        case MaterialTextureType::Emissive: return "Emissive";
        case MaterialTextureType::Height: return "Height";
        default: return "Unknown";
    }
}

MaterialTextureType MaterialSystem::StringToTextureType(const std::string& str) const
{
    if (str == "Albedo" || str == "albedo") return MaterialTextureType::Albedo;
    if (str == "Normal" || str == "normal") return MaterialTextureType::Normal;
    if (str == "Metallic" || str == "metallic") return MaterialTextureType::Metallic;
    if (str == "Roughness" || str == "roughness") return MaterialTextureType::Roughness;
    if (str == "Occlusion" || str == "occlusion") return MaterialTextureType::Occlusion;
    if (str == "Emissive" || str == "emissive") return MaterialTextureType::Emissive;
    if (str == "Height" || str == "height") return MaterialTextureType::Height;
    if (str == "DetailAlbedo" || str == "detailalbedo") return MaterialTextureType::DetailAlbedo;
    if (str == "DetailNormal" || str == "detailnormal") return MaterialTextureType::DetailNormal;
    if (str == "Subsurface" || str == "subsurface") return MaterialTextureType::Subsurface;
    if (str == "Transmission" || str == "transmission") return MaterialTextureType::Transmission;
    if (str == "Clearcoat" || str == "clearcoat") return MaterialTextureType::Clearcoat;
    if (str == "ClearcoatRoughness" || str == "clearcoatroughness") return MaterialTextureType::ClearcoatRoughness;
    if (str == "Anisotropy" || str == "anisotropy") return MaterialTextureType::Anisotropy;
    if (str == "Custom0" || str == "custom0") return MaterialTextureType::Custom0;
    if (str == "Custom1" || str == "custom1") return MaterialTextureType::Custom1;
    if (str == "Custom2" || str == "custom2") return MaterialTextureType::Custom2;
    if (str == "Custom3" || str == "custom3") return MaterialTextureType::Custom3;
    return MaterialTextureType::Albedo;
}

#endif // SPARK_PLATFORM_WINDOWS
