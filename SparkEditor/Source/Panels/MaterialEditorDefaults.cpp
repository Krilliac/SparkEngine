/**
 * @file MaterialEditorDefaults.cpp
 * @brief Shared standard PBR schema used by authoring, decoding, and fuzzing.
 */
#include "MaterialEditorMaterialParser.h"
#include "MaterialEditorPanel.h"
namespace SparkEditor
{
    void InitializeStandardPBRParameters(MaterialDefinition& material)
    {
        // ---- Surface group ----
        {
            ShaderParameter albedoColor;
            albedoColor.name = "albedoColor";
            albedoColor.displayName = "Albedo Color";
            albedoColor.type = ShaderParamType::Color;
            albedoColor.group = "Surface";
            albedoColor.floatValues[0] = 0.8f;
            albedoColor.floatValues[1] = 0.8f;
            albedoColor.floatValues[2] = 0.8f;
            albedoColor.floatValues[3] = 1.0f;
            albedoColor.tooltip = "Base color of the surface (linear space)";
            material.parameters.push_back(albedoColor);
        }
        {
            ShaderParameter metallic;
            metallic.name = "metallic";
            metallic.displayName = "Metallic";
            metallic.type = ShaderParamType::Float;
            metallic.group = "Surface";
            metallic.floatValues[0] = 0.0f;
            metallic.minValue = 0.0f;
            metallic.maxValue = 1.0f;
            metallic.step = 0.01f;
            metallic.tooltip = "0 = dielectric, 1 = metallic";
            material.parameters.push_back(metallic);
        }
        {
            ShaderParameter roughness;
            roughness.name = "roughness";
            roughness.displayName = "Roughness";
            roughness.type = ShaderParamType::Float;
            roughness.group = "Surface";
            roughness.floatValues[0] = 0.5f;
            roughness.minValue = 0.0f;
            roughness.maxValue = 1.0f;
            roughness.step = 0.01f;
            roughness.tooltip = "0 = mirror-smooth, 1 = fully rough";
            material.parameters.push_back(roughness);
        }
        {
            ShaderParameter opacity;
            opacity.name = "opacity";
            opacity.displayName = "Opacity";
            opacity.type = ShaderParamType::Float;
            opacity.group = "Surface";
            opacity.floatValues[0] = 1.0f;
            opacity.minValue = 0.0f;
            opacity.maxValue = 1.0f;
            opacity.step = 0.01f;
            opacity.tooltip = "Overall opacity of the material";
            material.parameters.push_back(opacity);
        }

        // ---- Normal group ----
        {
            ShaderParameter normalStrength;
            normalStrength.name = "normalStrength";
            normalStrength.displayName = "Normal Strength";
            normalStrength.type = ShaderParamType::Float;
            normalStrength.group = "Normal";
            normalStrength.floatValues[0] = 1.0f;
            normalStrength.minValue = 0.0f;
            normalStrength.maxValue = 2.0f;
            normalStrength.step = 0.01f;
            normalStrength.tooltip = "Intensity of the normal map effect";
            material.parameters.push_back(normalStrength);
        }

        // ---- Emission group ----
        {
            ShaderParameter emissionColor;
            emissionColor.name = "emissionColor";
            emissionColor.displayName = "Emission Color";
            emissionColor.type = ShaderParamType::Color;
            emissionColor.group = "Emission";
            emissionColor.floatValues[0] = 0.0f;
            emissionColor.floatValues[1] = 0.0f;
            emissionColor.floatValues[2] = 0.0f;
            emissionColor.floatValues[3] = 1.0f;
            emissionColor.isHDR = true;
            emissionColor.tooltip = "Emissive color (HDR values allowed)";
            material.parameters.push_back(emissionColor);
        }
        {
            ShaderParameter emissionIntensity;
            emissionIntensity.name = "emissionIntensity";
            emissionIntensity.displayName = "Emission Intensity";
            emissionIntensity.type = ShaderParamType::Float;
            emissionIntensity.group = "Emission";
            emissionIntensity.floatValues[0] = 0.0f;
            emissionIntensity.minValue = 0.0f;
            emissionIntensity.maxValue = 20.0f;
            emissionIntensity.step = 0.1f;
            emissionIntensity.tooltip = "Brightness multiplier for emission";
            material.parameters.push_back(emissionIntensity);
        }

        // ---- Ambient Occlusion group ----
        {
            ShaderParameter aoStrength;
            aoStrength.name = "aoStrength";
            aoStrength.displayName = "AO Strength";
            aoStrength.type = ShaderParamType::Float;
            aoStrength.group = "Ambient Occlusion";
            aoStrength.floatValues[0] = 1.0f;
            aoStrength.minValue = 0.0f;
            aoStrength.maxValue = 1.0f;
            aoStrength.step = 0.01f;
            aoStrength.tooltip = "Strength of the ambient occlusion effect";
            material.parameters.push_back(aoStrength);
        }

        // ---- Detail group ----
        {
            ShaderParameter detailTiling;
            detailTiling.name = "detailTiling";
            detailTiling.displayName = "Detail Tiling";
            detailTiling.type = ShaderParamType::Float2;
            detailTiling.group = "Detail";
            detailTiling.floatValues[0] = 1.0f;
            detailTiling.floatValues[1] = 1.0f;
            detailTiling.minValue = 0.01f;
            detailTiling.maxValue = 50.0f;
            detailTiling.tooltip = "UV tiling for detail textures";
            material.parameters.push_back(detailTiling);
        }
        {
            ShaderParameter useDetailMap;
            useDetailMap.name = "useDetailMap";
            useDetailMap.displayName = "Use Detail Map";
            useDetailMap.type = ShaderParamType::Bool;
            useDetailMap.group = "Detail";
            useDetailMap.boolValue = false;
            useDetailMap.tooltip = "Enable detail texture overlay";
            material.parameters.push_back(useDetailMap);
        }

        // ---- Texture Slots ----
        {
            TextureSlot albedo;
            albedo.name = "Albedo";
            albedo.bindSlot = 0;
            albedo.filter = TextureSlot::FilterMode::Trilinear;
            material.textureSlots.push_back(albedo);
        }
        {
            TextureSlot normal;
            normal.name = "Normal";
            normal.bindSlot = 1;
            normal.filter = TextureSlot::FilterMode::Trilinear;
            material.textureSlots.push_back(normal);
        }
        {
            TextureSlot metallic;
            metallic.name = "Metallic";
            metallic.bindSlot = 2;
            metallic.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(metallic);
        }
        {
            TextureSlot roughness;
            roughness.name = "Roughness";
            roughness.bindSlot = 3;
            roughness.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(roughness);
        }
        {
            TextureSlot ao;
            ao.name = "Ambient Occlusion";
            ao.bindSlot = 4;
            ao.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(ao);
        }
        {
            TextureSlot emission;
            emission.name = "Emission";
            emission.bindSlot = 5;
            emission.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(emission);
        }
        {
            TextureSlot height;
            height.name = "Height";
            height.bindSlot = 6;
            height.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(height);
        }
        {
            TextureSlot detail;
            detail.name = "Detail";
            detail.bindSlot = 7;
            detail.filter = TextureSlot::FilterMode::Trilinear;
            material.textureSlots.push_back(detail);
        }
    }

} // namespace SparkEditor
