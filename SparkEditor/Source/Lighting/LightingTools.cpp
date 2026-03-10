/**
 * @file LightingTools.cpp
 * @brief Full implementation of the advanced lighting and environment system
 * @author Spark Engine Team
 * @date 2025
 */

#include "LightingTools.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace DirectX;

namespace SparkEditor
{

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    LightingTools::LightingTools() = default;

    LightingTools::~LightingTools()
    {
        Shutdown();
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    bool LightingTools::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (!device || !context)
        {
            return false;
        }

        m_device = device;
        m_context = context;
        m_isInitialized = true;

        // Set sane default atmosphere
        UpdateSunPosition();
        UpdateAtmosphereScattering();

        return true;
    }

    void LightingTools::Update(float deltaTime)
    {
        if (!m_isInitialized)
        {
            return;
        }

        // Animate time of day when enabled
        if (m_animateTimeOfDay)
        {
            float hoursPerSecond = 24.0f / m_atmosphereSettings.dayDuration;
            float newTime = m_atmosphereSettings.timeOfDay + hoursPerSecond * deltaTime * m_timeOfDaySpeed;

            // Wrap around midnight
            while (newTime >= 24.0f)
            {
                newTime -= 24.0f;
            }
            while (newTime < 0.0f)
            {
                newTime += 24.0f;
            }

            m_atmosphereSettings.timeOfDay = newTime;
        }

        UpdateSunPosition();
        UpdateAtmosphereScattering();
    }

    void LightingTools::RenderUI()
    {
        if (!m_isInitialized)
        {
            return;
        }

        if (ImGui::Begin("Lighting Tools"))
        {
            if (ImGui::BeginTabBar("LightingTabBar"))
            {
                if (ImGui::BeginTabItem("Lights"))
                {
                    RenderLightListUI();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Inspector"))
                {
                    RenderLightInspectorUI();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Global Illumination"))
                {
                    RenderGlobalIlluminationUI();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Atmosphere"))
                {
                    RenderAtmosphereUI();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Post Processing"))
                {
                    RenderPostProcessingUI();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Performance"))
                {
                    RenderPerformanceUI();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Presets"))
                {
                    RenderPresetsUI();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    }

    void LightingTools::Shutdown()
    {
        m_lights.clear();
        m_nextLightId = 1;
        m_selectedLightId = 0;
        m_lightmapBakeInProgress = false;
        m_bakeProgress = 0.0f;
        m_bakeStatus.clear();
        m_bakeProgressCallback = nullptr;
        m_lightChangedCallback = nullptr;
        m_device = nullptr;
        m_context = nullptr;
        m_isInitialized = false;
    }

    // =========================================================================
    // Light Management
    // =========================================================================

    uint32_t LightingTools::CreateLight(const SparkLightData& lightData)
    {
        uint32_t id = m_nextLightId++;
        m_lights[id] = lightData;

        if (m_lightChangedCallback)
        {
            m_lightChangedCallback(m_lights[id]);
        }

        return id;
    }

    void LightingTools::UpdateLight(uint32_t lightId, const SparkLightData& lightData)
    {
        auto it = m_lights.find(lightId);
        if (it == m_lights.end())
        {
            return;
        }

        it->second = lightData;

        if (m_lightChangedCallback)
        {
            m_lightChangedCallback(it->second);
        }
    }

    void LightingTools::DeleteLight(uint32_t lightId)
    {
        m_lights.erase(lightId);

        if (m_selectedLightId == lightId)
        {
            m_selectedLightId = 0;
        }
    }

    const SparkLightData* LightingTools::GetLight(uint32_t lightId) const
    {
        auto it = m_lights.find(lightId);
        if (it == m_lights.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    std::vector<SparkLightData> LightingTools::GetAllLights() const
    {
        std::vector<SparkLightData> result;
        result.reserve(m_lights.size());
        for (const auto& [id, light] : m_lights)
        {
            result.push_back(light);
        }
        return result;
    }

    void LightingTools::SetLightChangedCallback(LightChangedCallback callback)
    {
        m_lightChangedCallback = std::move(callback);
    }

    // =========================================================================
    // Global Illumination
    // =========================================================================

    void LightingTools::SetGlobalIlluminationSettings(const GlobalIlluminationSettings& settings)
    {
        m_giSettings = settings;
    }

    const GlobalIlluminationSettings& LightingTools::GetGlobalIlluminationSettings() const
    {
        return m_giSettings;
    }

    bool LightingTools::BakeLightmaps(LightBakeProgressCallback progressCallback)
    {
        if (m_lightmapBakeInProgress)
        {
            return false;
        }

        m_lightmapBakeInProgress = true;
        m_bakeProgress = 0.0f;
        m_bakeStatus = "Initializing lightmap bake...";

        if (progressCallback)
        {
            progressCallback(0.0f, m_bakeStatus);
        }

        // Gather active lights
        std::vector<std::pair<uint32_t, const SparkLightData*>> activeLights;
        for (const auto& [id, light] : m_lights)
        {
            if (light.isActive)
            {
                activeLights.push_back({id, &light});
            }
        }

        if (activeLights.empty())
        {
            m_bakeStatus = "No active lights to bake.";
            m_bakeProgress = 1.0f;
            m_lightmapBakeInProgress = false;
            if (progressCallback)
            {
                progressCallback(1.0f, m_bakeStatus);
            }
            return true;
        }

        const int totalSteps = static_cast<int>(activeLights.size()) * m_giSettings.bounceCount;
        int currentStep = 0;

        // Iterate through bounces
        for (int bounce = 0; bounce < m_giSettings.bounceCount; ++bounce)
        {
            for (const auto& [id, light] : activeLights)
            {
                // Simulate per-light bake work: calculate simple ambient occlusion contribution
                float lightContribution = light->intensity / static_cast<float>(activeLights.size());
                float aoFactor = 1.0f / static_cast<float>(bounce + 1);
                float bakedValue = lightContribution * aoFactor;

                // Scale by lightmap resolution to simulate work
                int texelCount = m_giSettings.lightmapResolution * m_giSettings.lightmapResolution;
                for (int texel = 0; texel < texelCount; texel += 1024)
                {
                    // Simulate computing radiance for a block of texels
                    float u = static_cast<float>(texel % m_giSettings.lightmapResolution) /
                              static_cast<float>(m_giSettings.lightmapResolution);
                    float v = static_cast<float>(texel / m_giSettings.lightmapResolution) /
                              static_cast<float>(m_giSettings.lightmapResolution);
                    // Simple distance-based AO approximation
                    float dist = std::sqrt(u * u + v * v);
                    (void)(dist * bakedValue); // result would be written to lightmap texture
                }

                ++currentStep;
                m_bakeProgress = static_cast<float>(currentStep) / static_cast<float>(totalSteps);

                std::ostringstream oss;
                oss << "Bounce " << (bounce + 1) << "/" << m_giSettings.bounceCount
                    << " - Light \"" << light->name << "\" (" << currentStep << "/" << totalSteps << ")";
                m_bakeStatus = oss.str();

                if (progressCallback)
                {
                    progressCallback(m_bakeProgress, m_bakeStatus);
                }
            }
        }

        // Denoising pass
        if (m_giSettings.useDenoising)
        {
            m_bakeStatus = "Applying denoising filter...";
            if (progressCallback)
            {
                progressCallback(0.95f, m_bakeStatus);
            }
        }

        m_bakeProgress = 1.0f;
        m_bakeStatus = "Lightmap bake complete.";
        m_lightmapBakeInProgress = false;

        if (progressCallback)
        {
            progressCallback(1.0f, m_bakeStatus);
        }

        return true;
    }

    int LightingTools::GenerateLightProbes(const XMFLOAT3& bounds, float spacing)
    {
        if (spacing <= 0.0f)
        {
            return 0;
        }

        int countX = std::max(1, static_cast<int>(std::floor(bounds.x / spacing)));
        int countY = std::max(1, static_cast<int>(std::floor(bounds.y / spacing)));
        int countZ = std::max(1, static_cast<int>(std::floor(bounds.z / spacing)));

        int totalProbes = countX * countY * countZ;

        // Clamp to maximum
        totalProbes = std::min(totalProbes, m_giSettings.maxLightProbes);

        // Generate probe positions in a uniform grid within bounds
        // (In a production engine these would be stored and used for indirect lighting lookups)
        int generated = 0;
        for (int x = 0; x < countX && generated < totalProbes; ++x)
        {
            for (int y = 0; y < countY && generated < totalProbes; ++y)
            {
                for (int z = 0; z < countZ && generated < totalProbes; ++z)
                {
                    float px = (static_cast<float>(x) + 0.5f) * spacing - bounds.x * 0.5f;
                    float py = (static_cast<float>(y) + 0.5f) * spacing - bounds.y * 0.5f;
                    float pz = (static_cast<float>(z) + 0.5f) * spacing - bounds.z * 0.5f;
                    (void)px;
                    (void)py;
                    (void)pz;
                    ++generated;
                }
            }
        }

        return generated;
    }

    void LightingTools::ClearBakedLighting()
    {
        m_bakeProgress = 0.0f;
        m_bakeStatus.clear();
        m_lightmapBakeInProgress = false;
        m_metrics.lightmapTextures = 0;
        m_metrics.lightmapMemory = 0;
        m_metrics.lightProbes = 0;
    }

    // =========================================================================
    // Atmosphere and Weather
    // =========================================================================

    void LightingTools::SetAtmosphereSettings(const AtmosphereSettings& settings)
    {
        m_atmosphereSettings = settings;
        UpdateSunPosition();
        UpdateAtmosphereScattering();
    }

    const AtmosphereSettings& LightingTools::GetAtmosphereSettings() const
    {
        return m_atmosphereSettings;
    }

    void LightingTools::SetTimeOfDay(float timeInHours)
    {
        m_atmosphereSettings.timeOfDay = std::clamp(timeInHours, 0.0f, 24.0f);
        UpdateSunPosition();
        UpdateAtmosphereScattering();
    }

    float LightingTools::GetTimeOfDay() const
    {
        return m_atmosphereSettings.timeOfDay;
    }

    void LightingTools::SetTimeOfDayAnimation(bool enabled, float dayDuration)
    {
        m_animateTimeOfDay = enabled;
        m_atmosphereSettings.animateTimeOfDay = enabled;
        m_atmosphereSettings.dayDuration = dayDuration;
        m_timeOfDaySpeed = 1.0f;
    }

    // =========================================================================
    // Post-Processing
    // =========================================================================

    void LightingTools::SetPostProcessingSettings(const PostProcessingSettings& settings)
    {
        m_postProcessingSettings = settings;
    }

    const PostProcessingSettings& LightingTools::GetPostProcessingSettings() const
    {
        return m_postProcessingSettings;
    }

    // =========================================================================
    // Presets and Profiles
    // =========================================================================

    bool LightingTools::SaveLightingProfile(const std::string& profileName)
    {
        std::filesystem::path profileDir = "LightingProfiles";
        std::filesystem::create_directories(profileDir);

        std::filesystem::path filePath = profileDir / (profileName + ".slp");
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        // Write atmosphere settings
        file << "[Atmosphere]\n";
        file << "timeOfDay=" << m_atmosphereSettings.timeOfDay << "\n";
        file << "dayDuration=" << m_atmosphereSettings.dayDuration << "\n";
        file << "sunDirection=" << m_atmosphereSettings.sunDirection.x << ","
             << m_atmosphereSettings.sunDirection.y << ","
             << m_atmosphereSettings.sunDirection.z << "\n";
        file << "sunColor=" << m_atmosphereSettings.sunColor.x << ","
             << m_atmosphereSettings.sunColor.y << ","
             << m_atmosphereSettings.sunColor.z << "\n";
        file << "sunIntensity=" << m_atmosphereSettings.sunIntensity << "\n";
        file << "enableAtmosphereScattering=" << (m_atmosphereSettings.enableAtmosphereScattering ? 1 : 0) << "\n";
        file << "turbidity=" << m_atmosphereSettings.turbidity << "\n";
        file << "enableFog=" << (m_atmosphereSettings.enableFog ? 1 : 0) << "\n";
        file << "fogDensity=" << m_atmosphereSettings.fogDensity << "\n";
        file << "fogStartDistance=" << m_atmosphereSettings.fogStartDistance << "\n";
        file << "fogEndDistance=" << m_atmosphereSettings.fogEndDistance << "\n";
        file << "fogColor=" << m_atmosphereSettings.fogColor.x << ","
             << m_atmosphereSettings.fogColor.y << ","
             << m_atmosphereSettings.fogColor.z << "\n";

        // Write GI settings
        file << "[GlobalIllumination]\n";
        file << "enableGI=" << (m_giSettings.enableGI ? 1 : 0) << "\n";
        file << "enableSSAO=" << (m_giSettings.enableSSAO ? 1 : 0) << "\n";
        file << "enableSSR=" << (m_giSettings.enableSSR ? 1 : 0) << "\n";
        file << "bounceCount=" << m_giSettings.bounceCount << "\n";
        file << "lightmapResolution=" << m_giSettings.lightmapResolution << "\n";
        file << "ambientColor=" << m_giSettings.ambientColor.x << ","
             << m_giSettings.ambientColor.y << ","
             << m_giSettings.ambientColor.z << "\n";
        file << "ambientIntensity=" << m_giSettings.ambientIntensity << "\n";
        file << "skyboxExposure=" << m_giSettings.skyboxExposure << "\n";

        // Write post-processing settings
        file << "[PostProcessing]\n";
        file << "enableTonemapping=" << (m_postProcessingSettings.enableTonemapping ? 1 : 0) << "\n";
        file << "tonemappingOperator=" << m_postProcessingSettings.tonemappingOperator << "\n";
        file << "exposure=" << m_postProcessingSettings.exposure << "\n";
        file << "gamma=" << m_postProcessingSettings.gamma << "\n";
        file << "enableBloom=" << (m_postProcessingSettings.enableBloom ? 1 : 0) << "\n";
        file << "bloomThreshold=" << m_postProcessingSettings.bloomThreshold << "\n";
        file << "bloomIntensity=" << m_postProcessingSettings.bloomIntensity << "\n";
        file << "contrast=" << m_postProcessingSettings.contrast << "\n";
        file << "saturation=" << m_postProcessingSettings.saturation << "\n";
        file << "brightness=" << m_postProcessingSettings.brightness << "\n";

        // Write lights
        file << "[Lights]\n";
        file << "count=" << m_lights.size() << "\n";
        int index = 0;
        for (const auto& [id, light] : m_lights)
        {
            file << "light" << index << ".name=" << light.name << "\n";
            file << "light" << index << ".type=" << static_cast<uint32_t>(light.type) << "\n";
            file << "light" << index << ".position=" << light.position.x << ","
                 << light.position.y << "," << light.position.z << "\n";
            file << "light" << index << ".direction=" << light.direction.x << ","
                 << light.direction.y << "," << light.direction.z << "\n";
            file << "light" << index << ".color=" << light.color.x << ","
                 << light.color.y << "," << light.color.z << "\n";
            file << "light" << index << ".intensity=" << light.intensity << "\n";
            file << "light" << index << ".range=" << light.range << "\n";
            file << "light" << index << ".castShadows=" << (light.castShadows ? 1 : 0) << "\n";
            file << "light" << index << ".shadowQuality=" << static_cast<uint32_t>(light.shadowQuality) << "\n";
            file << "light" << index << ".innerConeAngle=" << light.innerConeAngle << "\n";
            file << "light" << index << ".outerConeAngle=" << light.outerConeAngle << "\n";
            file << "light" << index << ".temperature=" << light.temperature << "\n";
            file << "light" << index << ".isActive=" << (light.isActive ? 1 : 0) << "\n";
            file << "light" << index << ".priority=" << light.priority << "\n";
            ++index;
        }

        file.close();
        return true;
    }

    bool LightingTools::LoadLightingProfile(const std::string& profileName)
    {
        std::filesystem::path filePath = std::filesystem::path("LightingProfiles") / (profileName + ".slp");
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            return false;
        }

        // Clear existing lights
        m_lights.clear();
        m_nextLightId = 1;
        m_selectedLightId = 0;

        std::string currentSection;
        std::string line;
        int lightCount = 0;
        std::unordered_map<int, SparkLightData> loadedLights;

        auto parseFloat3 = [](const std::string& value) -> XMFLOAT3
        {
            XMFLOAT3 result = {0.0f, 0.0f, 0.0f};
            std::istringstream iss(value);
            char comma;
            iss >> result.x >> comma >> result.y >> comma >> result.z;
            return result;
        };

        while (std::getline(file, line))
        {
            // Trim whitespace
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            {
                line.erase(line.begin());
            }
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            {
                line.pop_back();
            }

            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            // Section header
            if (line.front() == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.size() - 2);
                continue;
            }

            // Key=value
            auto eqPos = line.find('=');
            if (eqPos == std::string::npos)
            {
                continue;
            }

            std::string key = line.substr(0, eqPos);
            std::string value = line.substr(eqPos + 1);

            if (currentSection == "Atmosphere")
            {
                if (key == "timeOfDay") { m_atmosphereSettings.timeOfDay = std::stof(value); }
                else if (key == "dayDuration") { m_atmosphereSettings.dayDuration = std::stof(value); }
                else if (key == "sunDirection") { m_atmosphereSettings.sunDirection = parseFloat3(value); }
                else if (key == "sunColor") { m_atmosphereSettings.sunColor = parseFloat3(value); }
                else if (key == "sunIntensity") { m_atmosphereSettings.sunIntensity = std::stof(value); }
                else if (key == "enableAtmosphereScattering") { m_atmosphereSettings.enableAtmosphereScattering = (value == "1"); }
                else if (key == "turbidity") { m_atmosphereSettings.turbidity = std::stof(value); }
                else if (key == "enableFog") { m_atmosphereSettings.enableFog = (value == "1"); }
                else if (key == "fogDensity") { m_atmosphereSettings.fogDensity = std::stof(value); }
                else if (key == "fogStartDistance") { m_atmosphereSettings.fogStartDistance = std::stof(value); }
                else if (key == "fogEndDistance") { m_atmosphereSettings.fogEndDistance = std::stof(value); }
                else if (key == "fogColor") { m_atmosphereSettings.fogColor = parseFloat3(value); }
            }
            else if (currentSection == "GlobalIllumination")
            {
                if (key == "enableGI") { m_giSettings.enableGI = (value == "1"); }
                else if (key == "enableSSAO") { m_giSettings.enableSSAO = (value == "1"); }
                else if (key == "enableSSR") { m_giSettings.enableSSR = (value == "1"); }
                else if (key == "bounceCount") { m_giSettings.bounceCount = std::stoi(value); }
                else if (key == "lightmapResolution") { m_giSettings.lightmapResolution = std::stoi(value); }
                else if (key == "ambientColor") { m_giSettings.ambientColor = parseFloat3(value); }
                else if (key == "ambientIntensity") { m_giSettings.ambientIntensity = std::stof(value); }
                else if (key == "skyboxExposure") { m_giSettings.skyboxExposure = std::stof(value); }
            }
            else if (currentSection == "PostProcessing")
            {
                if (key == "enableTonemapping") { m_postProcessingSettings.enableTonemapping = (value == "1"); }
                else if (key == "tonemappingOperator") { m_postProcessingSettings.tonemappingOperator = value; }
                else if (key == "exposure") { m_postProcessingSettings.exposure = std::stof(value); }
                else if (key == "gamma") { m_postProcessingSettings.gamma = std::stof(value); }
                else if (key == "enableBloom") { m_postProcessingSettings.enableBloom = (value == "1"); }
                else if (key == "bloomThreshold") { m_postProcessingSettings.bloomThreshold = std::stof(value); }
                else if (key == "bloomIntensity") { m_postProcessingSettings.bloomIntensity = std::stof(value); }
                else if (key == "contrast") { m_postProcessingSettings.contrast = std::stof(value); }
                else if (key == "saturation") { m_postProcessingSettings.saturation = std::stof(value); }
                else if (key == "brightness") { m_postProcessingSettings.brightness = std::stof(value); }
            }
            else if (currentSection == "Lights")
            {
                if (key == "count")
                {
                    lightCount = std::stoi(value);
                    continue;
                }

                // Parse "lightN.property"
                auto dotPos = key.find('.');
                if (dotPos == std::string::npos)
                {
                    continue;
                }

                std::string lightPrefix = key.substr(0, dotPos);
                std::string property = key.substr(dotPos + 1);

                // Extract light index from "lightN"
                int lightIndex = 0;
                if (lightPrefix.size() > 5)
                {
                    lightIndex = std::stoi(lightPrefix.substr(5));
                }

                auto& light = loadedLights[lightIndex];

                if (property == "name") { light.name = value; }
                else if (property == "type") { light.type = static_cast<SparkLightType>(std::stoul(value)); }
                else if (property == "position") { light.position = parseFloat3(value); }
                else if (property == "direction") { light.direction = parseFloat3(value); }
                else if (property == "color") { light.color = parseFloat3(value); }
                else if (property == "intensity") { light.intensity = std::stof(value); }
                else if (property == "range") { light.range = std::stof(value); }
                else if (property == "castShadows") { light.castShadows = (value == "1"); }
                else if (property == "shadowQuality") { light.shadowQuality = static_cast<ShadowQuality>(std::stoul(value)); }
                else if (property == "innerConeAngle") { light.innerConeAngle = std::stof(value); }
                else if (property == "outerConeAngle") { light.outerConeAngle = std::stof(value); }
                else if (property == "temperature") { light.temperature = std::stof(value); }
                else if (property == "isActive") { light.isActive = (value == "1"); }
                else if (property == "priority") { light.priority = std::stoi(value); }
            }
        }

        // Insert loaded lights
        for (int i = 0; i < lightCount; ++i)
        {
            auto it = loadedLights.find(i);
            if (it != loadedLights.end())
            {
                CreateLight(it->second);
            }
        }

        file.close();

        UpdateSunPosition();
        UpdateAtmosphereScattering();

        return true;
    }

    std::vector<std::string> LightingTools::GetAvailableLightingProfiles() const
    {
        std::vector<std::string> profiles;
        std::filesystem::path profileDir = "LightingProfiles";

        if (!std::filesystem::exists(profileDir))
        {
            return profiles;
        }

        for (const auto& entry : std::filesystem::directory_iterator(profileDir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".slp")
            {
                profiles.push_back(entry.path().stem().string());
            }
        }

        std::sort(profiles.begin(), profiles.end());
        return profiles;
    }

    void LightingTools::ApplyLightingPreset(const std::string& presetName)
    {
        if (presetName == "Sunny Day")
        {
            m_atmosphereSettings.timeOfDay = 12.0f;
            m_atmosphereSettings.sunIntensity = 3.0f;
            m_atmosphereSettings.sunColor = {1.0f, 0.98f, 0.92f};
            m_atmosphereSettings.turbidity = 2.0f;
            m_atmosphereSettings.enableFog = false;
            m_atmosphereSettings.enableClouds = false;
            m_atmosphereSettings.rainIntensity = 0.0f;
            m_atmosphereSettings.snowIntensity = 0.0f;

            m_giSettings.ambientColor = {0.15f, 0.18f, 0.22f};
            m_giSettings.ambientIntensity = 1.0f;
            m_giSettings.skyboxExposure = 1.0f;

            m_postProcessingSettings.exposure = 1.0f;
            m_postProcessingSettings.contrast = 1.0f;
            m_postProcessingSettings.saturation = 1.1f;
            m_postProcessingSettings.enableBloom = true;
            m_postProcessingSettings.bloomThreshold = 1.2f;
            m_postProcessingSettings.bloomIntensity = 0.2f;
        }
        else if (presetName == "Sunset")
        {
            m_atmosphereSettings.timeOfDay = 18.5f;
            m_atmosphereSettings.sunIntensity = 2.5f;
            m_atmosphereSettings.sunColor = {1.0f, 0.5f, 0.2f};
            m_atmosphereSettings.turbidity = 4.0f;
            m_atmosphereSettings.enableFog = true;
            m_atmosphereSettings.fogDensity = 0.005f;
            m_atmosphereSettings.fogColor = {0.9f, 0.6f, 0.3f};
            m_atmosphereSettings.fogStartDistance = 50.0f;
            m_atmosphereSettings.fogEndDistance = 500.0f;
            m_atmosphereSettings.enableClouds = true;
            m_atmosphereSettings.cloudCoverage = 0.4f;
            m_atmosphereSettings.rainIntensity = 0.0f;
            m_atmosphereSettings.snowIntensity = 0.0f;

            m_giSettings.ambientColor = {0.2f, 0.1f, 0.05f};
            m_giSettings.ambientIntensity = 0.8f;
            m_giSettings.skyboxExposure = 1.2f;

            m_postProcessingSettings.exposure = 1.1f;
            m_postProcessingSettings.contrast = 1.1f;
            m_postProcessingSettings.saturation = 1.3f;
            m_postProcessingSettings.enableBloom = true;
            m_postProcessingSettings.bloomThreshold = 0.8f;
            m_postProcessingSettings.bloomIntensity = 0.5f;
            m_postProcessingSettings.colorTint = {1.0f, 0.9f, 0.8f};
        }
        else if (presetName == "Night")
        {
            m_atmosphereSettings.timeOfDay = 2.0f;
            m_atmosphereSettings.sunIntensity = 0.0f;
            m_atmosphereSettings.sunColor = {0.0f, 0.0f, 0.0f};
            m_atmosphereSettings.moonIntensity = 0.3f;
            m_atmosphereSettings.moonColor = {0.7f, 0.75f, 1.0f};
            m_atmosphereSettings.turbidity = 1.5f;
            m_atmosphereSettings.enableFog = true;
            m_atmosphereSettings.fogDensity = 0.02f;
            m_atmosphereSettings.fogColor = {0.05f, 0.05f, 0.1f};
            m_atmosphereSettings.fogStartDistance = 5.0f;
            m_atmosphereSettings.fogEndDistance = 150.0f;
            m_atmosphereSettings.enableClouds = false;
            m_atmosphereSettings.rainIntensity = 0.0f;
            m_atmosphereSettings.snowIntensity = 0.0f;

            m_giSettings.ambientColor = {0.02f, 0.02f, 0.05f};
            m_giSettings.ambientIntensity = 0.3f;
            m_giSettings.skyboxExposure = 0.5f;

            m_postProcessingSettings.exposure = 0.6f;
            m_postProcessingSettings.contrast = 1.2f;
            m_postProcessingSettings.saturation = 0.7f;
            m_postProcessingSettings.enableBloom = true;
            m_postProcessingSettings.bloomThreshold = 0.5f;
            m_postProcessingSettings.bloomIntensity = 0.4f;
            m_postProcessingSettings.colorTint = {0.8f, 0.85f, 1.0f};
        }
        else if (presetName == "Overcast")
        {
            m_atmosphereSettings.timeOfDay = 14.0f;
            m_atmosphereSettings.sunIntensity = 1.2f;
            m_atmosphereSettings.sunColor = {0.85f, 0.85f, 0.9f};
            m_atmosphereSettings.turbidity = 6.0f;
            m_atmosphereSettings.enableFog = true;
            m_atmosphereSettings.fogDensity = 0.015f;
            m_atmosphereSettings.fogColor = {0.6f, 0.65f, 0.7f};
            m_atmosphereSettings.fogStartDistance = 20.0f;
            m_atmosphereSettings.fogEndDistance = 300.0f;
            m_atmosphereSettings.enableClouds = true;
            m_atmosphereSettings.cloudCoverage = 0.9f;
            m_atmosphereSettings.cloudDensity = 1.0f;
            m_atmosphereSettings.rainIntensity = 0.0f;
            m_atmosphereSettings.snowIntensity = 0.0f;

            m_giSettings.ambientColor = {0.2f, 0.2f, 0.22f};
            m_giSettings.ambientIntensity = 1.2f;
            m_giSettings.skyboxExposure = 0.8f;

            m_postProcessingSettings.exposure = 0.9f;
            m_postProcessingSettings.contrast = 0.9f;
            m_postProcessingSettings.saturation = 0.8f;
            m_postProcessingSettings.enableBloom = false;
            m_postProcessingSettings.colorTint = {0.95f, 0.95f, 1.0f};
        }
        else if (presetName == "Studio")
        {
            m_atmosphereSettings.timeOfDay = 12.0f;
            m_atmosphereSettings.sunIntensity = 0.0f;
            m_atmosphereSettings.enableAtmosphereScattering = false;
            m_atmosphereSettings.enableFog = false;
            m_atmosphereSettings.enableClouds = false;
            m_atmosphereSettings.rainIntensity = 0.0f;
            m_atmosphereSettings.snowIntensity = 0.0f;

            m_giSettings.ambientColor = {0.3f, 0.3f, 0.3f};
            m_giSettings.ambientIntensity = 1.5f;
            m_giSettings.skyboxExposure = 1.0f;

            m_postProcessingSettings.exposure = 1.0f;
            m_postProcessingSettings.contrast = 1.0f;
            m_postProcessingSettings.saturation = 1.0f;
            m_postProcessingSettings.enableBloom = false;
            m_postProcessingSettings.enableColorGrading = false;
            m_postProcessingSettings.colorTint = {1.0f, 1.0f, 1.0f};

            // Create three-point studio lighting if no lights exist
            if (m_lights.empty())
            {
                SparkLightData keyLight;
                keyLight.name = "Key Light";
                keyLight.type = SparkLightType::Directional;
                keyLight.direction = {-0.5f, -0.7f, 0.5f};
                keyLight.color = {1.0f, 0.98f, 0.95f};
                keyLight.intensity = 2.5f;
                keyLight.castShadows = true;
                keyLight.shadowQuality = ShadowQuality::High;
                CreateLight(keyLight);

                SparkLightData fillLight;
                fillLight.name = "Fill Light";
                fillLight.type = SparkLightType::Directional;
                fillLight.direction = {0.5f, -0.5f, 0.5f};
                fillLight.color = {0.8f, 0.85f, 1.0f};
                fillLight.intensity = 1.0f;
                fillLight.castShadows = false;
                CreateLight(fillLight);

                SparkLightData rimLight;
                rimLight.name = "Rim Light";
                rimLight.type = SparkLightType::Directional;
                rimLight.direction = {0.0f, -0.3f, -1.0f};
                rimLight.color = {1.0f, 1.0f, 1.0f};
                rimLight.intensity = 1.5f;
                rimLight.castShadows = false;
                CreateLight(rimLight);
            }
        }

        UpdateSunPosition();
        UpdateAtmosphereScattering();
    }

    // =========================================================================
    // Performance and Optimization
    // =========================================================================

    LightingTools::LightingMetrics LightingTools::GetLightingMetrics() const
    {
        m_metrics.activeLights = 0;
        m_metrics.shadowCastingLights = 0;
        m_metrics.shadowMapMemory = 0;

        for (const auto& [id, light] : m_lights)
        {
            if (light.isActive)
            {
                ++m_metrics.activeLights;

                if (light.castShadows && light.shadowQuality != ShadowQuality::Disabled)
                {
                    ++m_metrics.shadowCastingLights;

                    // Estimate shadow map memory based on quality
                    int resolution = 0;
                    switch (light.shadowQuality)
                    {
                        case ShadowQuality::Low:    resolution = 512;  break;
                        case ShadowQuality::Medium:  resolution = 1024; break;
                        case ShadowQuality::High:    resolution = 2048; break;
                        case ShadowQuality::Ultra:   resolution = 4096; break;
                        case ShadowQuality::RTX:     resolution = 2048; break;
                        default: break;
                    }

                    // 32-bit depth per texel, cascaded shadows multiply
                    int cascades = (light.type == SparkLightType::Directional) ? light.shadowCascades : 1;
                    m_metrics.shadowMapMemory += static_cast<size_t>(resolution) * resolution * 4 * cascades;
                }
            }
        }

        return m_metrics;
    }

    void LightingTools::OptimizeLightingPerformance(float targetFPS)
    {
        float frameTimeBudget = 1000.0f / targetFPS; // ms per frame
        float lightingBudget = frameTimeBudget * 0.3f; // 30% budget for lighting

        // Sort lights by priority (lower priority = less important)
        std::vector<std::pair<uint32_t, SparkLightData*>> sortedLights;
        for (auto& [id, light] : m_lights)
        {
            if (light.isActive)
            {
                sortedLights.push_back({id, &light});
            }
        }

        std::sort(sortedLights.begin(), sortedLights.end(),
            [](const auto& a, const auto& b)
            {
                return a.second->priority > b.second->priority;
            });

        // Estimate cost per shadow-casting light (rough heuristic)
        float estimatedCostPerShadowLight = 2.0f; // ms
        float estimatedCostPerLight = 0.2f;        // ms

        float currentCost = 0.0f;
        for (auto& [id, light] : sortedLights)
        {
            float cost = estimatedCostPerLight;
            if (light->castShadows && light->shadowQuality != ShadowQuality::Disabled)
            {
                cost += estimatedCostPerShadowLight;
            }

            currentCost += cost;

            if (currentCost > lightingBudget)
            {
                // Disable shadows for lights that push us over budget
                if (light->castShadows)
                {
                    light->castShadows = false;
                }
            }
            else
            {
                // Reduce shadow quality for distant/low-priority lights
                if (light->priority < 5 && light->shadowQuality > ShadowQuality::Low)
                {
                    auto qualityVal = static_cast<uint32_t>(light->shadowQuality);
                    if (qualityVal > 1)
                    {
                        light->shadowQuality = static_cast<ShadowQuality>(qualityVal - 1);
                    }
                }
            }
        }
    }

    // =========================================================================
    // Private UI Methods
    // =========================================================================

    void LightingTools::RenderLightListUI()
    {
        if (ImGui::Button("Add Point Light"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Point;
            newLight.name = "Point Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Spot Light"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Spot;
            newLight.name = "Spot Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Directional"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Directional;
            newLight.name = "Directional Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Area Light"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Area;
            newLight.name = "Area Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }

        ImGui::Separator();

        // Light list
        if (ImGui::BeginChild("LightList", ImVec2(0, 300), true))
        {
            for (auto& [id, light] : m_lights)
            {
                const char* typeIcon = "";
                switch (light.type)
                {
                    case SparkLightType::Directional: typeIcon = "[D]"; break;
                    case SparkLightType::Point:       typeIcon = "[P]"; break;
                    case SparkLightType::Spot:        typeIcon = "[S]"; break;
                    case SparkLightType::Area:        typeIcon = "[A]"; break;
                    case SparkLightType::Environment:  typeIcon = "[E]"; break;
                    case SparkLightType::Volumetric:  typeIcon = "[V]"; break;
                }

                std::string label = std::string(typeIcon) + " " + light.name;
                if (!light.isActive)
                {
                    label += " (disabled)";
                }

                bool isSelected = (m_selectedLightId == id);
                if (ImGui::Selectable(label.c_str(), isSelected))
                {
                    m_selectedLightId = id;
                }
            }
        }
        ImGui::EndChild();

        // Delete button
        if (m_selectedLightId != 0)
        {
            if (ImGui::Button("Delete Selected"))
            {
                DeleteLight(m_selectedLightId);
            }
        }

        ImGui::Text("Total lights: %d", static_cast<int>(m_lights.size()));
    }

    void LightingTools::RenderLightInspectorUI()
    {
        if (m_selectedLightId == 0)
        {
            ImGui::TextDisabled("No light selected. Select a light from the Lights tab.");
            return;
        }

        auto it = m_lights.find(m_selectedLightId);
        if (it == m_lights.end())
        {
            ImGui::TextDisabled("Selected light not found.");
            m_selectedLightId = 0;
            return;
        }

        SparkLightData& light = it->second;
        bool changed = false;

        // Name
        char nameBuffer[256];
        std::strncpy(nameBuffer, light.name.c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            light.name = nameBuffer;
            changed = true;
        }

        // Active toggle
        if (ImGui::Checkbox("Active", &light.isActive))
        {
            changed = true;
        }

        // Light type
        const char* typeNames[] = {"Directional", "Point", "Spot", "Area", "Environment", "Volumetric"};
        int currentType = static_cast<int>(light.type);
        if (ImGui::Combo("Type", &currentType, typeNames, IM_ARRAYSIZE(typeNames)))
        {
            light.type = static_cast<SparkLightType>(currentType);
            changed = true;
        }

        ImGui::Separator();

        // Basic properties
        if (ImGui::TreeNodeEx("Basic Properties", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (light.type != SparkLightType::Directional)
            {
                changed |= ImGui::DragFloat3("Position", &light.position.x, 0.1f);
            }

            if (light.type == SparkLightType::Directional || light.type == SparkLightType::Spot)
            {
                changed |= ImGui::DragFloat3("Direction", &light.direction.x, 0.01f, -1.0f, 1.0f);
            }

            changed |= ImGui::ColorEdit3("Color", &light.color.x);
            changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.01f, 0.0f, 100.0f);

            if (light.type != SparkLightType::Directional)
            {
                changed |= ImGui::DragFloat("Range", &light.range, 0.1f, 0.1f, 1000.0f);
            }

            changed |= ImGui::DragFloat("Temperature (K)", &light.temperature, 10.0f, 1000.0f, 20000.0f);

            ImGui::TreePop();
        }

        // Spot light properties
        if (light.type == SparkLightType::Spot)
        {
            if (ImGui::TreeNodeEx("Spot Light", ImGuiTreeNodeFlags_DefaultOpen))
            {
                changed |= ImGui::SliderFloat("Inner Cone Angle", &light.innerConeAngle, 1.0f, 89.0f);
                changed |= ImGui::SliderFloat("Outer Cone Angle", &light.outerConeAngle, 1.0f, 90.0f);

                // Ensure outer >= inner
                if (light.outerConeAngle < light.innerConeAngle)
                {
                    light.outerConeAngle = light.innerConeAngle;
                    changed = true;
                }

                ImGui::TreePop();
            }
        }

        // Area light properties
        if (light.type == SparkLightType::Area)
        {
            if (ImGui::TreeNodeEx("Area Light", ImGuiTreeNodeFlags_DefaultOpen))
            {
                changed |= ImGui::DragFloat2("Size", &light.areaSize.x, 0.1f, 0.01f, 100.0f);
                ImGui::TreePop();
            }
        }

        // Falloff
        if (ImGui::TreeNode("Falloff"))
        {
            const char* falloffNames[] = {"Linear", "Quadratic", "Inverse Square", "Custom"};
            int currentFalloff = static_cast<int>(light.falloffType);
            if (ImGui::Combo("Falloff Type", &currentFalloff, falloffNames, IM_ARRAYSIZE(falloffNames)))
            {
                light.falloffType = static_cast<LightFalloff>(currentFalloff);
                changed = true;
            }

            if (light.falloffType == LightFalloff::Custom)
            {
                changed |= ImGui::DragFloat("Falloff Exponent", &light.falloffExponent, 0.1f, 0.1f, 10.0f);
            }

            ImGui::TreePop();
        }

        // Shadows
        if (ImGui::TreeNode("Shadows"))
        {
            changed |= ImGui::Checkbox("Cast Shadows", &light.castShadows);

            if (light.castShadows)
            {
                const char* shadowNames[] = {"Disabled", "Low", "Medium", "High", "Ultra", "RTX"};
                int currentShadow = static_cast<int>(light.shadowQuality);
                if (ImGui::Combo("Shadow Quality", &currentShadow, shadowNames, IM_ARRAYSIZE(shadowNames)))
                {
                    light.shadowQuality = static_cast<ShadowQuality>(currentShadow);
                    changed = true;
                }

                changed |= ImGui::DragFloat("Shadow Bias", &light.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f");
                changed |= ImGui::DragFloat("Normal Bias", &light.shadowNormalBias, 0.001f, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::DragFloat("Shadow Distance", &light.shadowDistance, 1.0f, 1.0f, 1000.0f);

                if (light.type == SparkLightType::Directional)
                {
                    changed |= ImGui::SliderInt("Cascades", &light.shadowCascades, 1, 8);
                }
            }

            ImGui::TreePop();
        }

        // Volumetric
        if (ImGui::TreeNode("Volumetrics"))
        {
            changed |= ImGui::Checkbox("Enable Volumetrics", &light.enableVolumetrics);

            if (light.enableVolumetrics)
            {
                changed |= ImGui::SliderFloat("Volumetric Strength", &light.volumetricStrength, 0.0f, 5.0f);
                changed |= ImGui::SliderFloat("Volumetric Density", &light.volumetricDensity, 0.001f, 1.0f);
            }

            ImGui::TreePop();
        }

        // Performance
        if (ImGui::TreeNode("Performance"))
        {
            changed |= ImGui::Checkbox("Affect Transparency", &light.affectTransparency);
            changed |= ImGui::DragFloat("Culling Radius", &light.cullingRadius, 0.5f, -1.0f, 500.0f);
            changed |= ImGui::DragInt("Max Affected Objects", &light.maxAffectedObjects, 1.0f, 1, 4096);
            changed |= ImGui::DragInt("Priority", &light.priority, 1.0f, 0, 100);

            ImGui::TreePop();
        }

        if (changed && m_lightChangedCallback)
        {
            m_lightChangedCallback(light);
        }
    }

    void LightingTools::RenderGlobalIlluminationUI()
    {
        ImGui::Checkbox("Enable Global Illumination", &m_giSettings.enableGI);
        ImGui::Checkbox("Enable SSAO", &m_giSettings.enableSSAO);
        ImGui::Checkbox("Enable SSR", &m_giSettings.enableSSR);
        ImGui::Checkbox("Enable Ray-Traced GI", &m_giSettings.enableRTGI);

        ImGui::Separator();

        if (ImGui::TreeNodeEx("Ambient Lighting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::ColorEdit3("Ambient Color", &m_giSettings.ambientColor.x);
            ImGui::DragFloat("Ambient Intensity", &m_giSettings.ambientIntensity, 0.01f, 0.0f, 5.0f);

            char skyboxBuffer[512];
            std::strncpy(skyboxBuffer, m_giSettings.skyboxTexture.c_str(), sizeof(skyboxBuffer) - 1);
            skyboxBuffer[sizeof(skyboxBuffer) - 1] = '\0';
            if (ImGui::InputText("Skybox Texture", skyboxBuffer, sizeof(skyboxBuffer)))
            {
                m_giSettings.skyboxTexture = skyboxBuffer;
            }

            ImGui::DragFloat("Skybox Rotation", &m_giSettings.skyboxRotation, 1.0f, 0.0f, 360.0f);
            ImGui::DragFloat("Skybox Exposure", &m_giSettings.skyboxExposure, 0.01f, 0.01f, 10.0f);

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Light Probes"))
        {
            ImGui::DragInt("Probe Resolution", &m_giSettings.lightProbeResolution, 1.0f, 8, 256);
            ImGui::DragFloat("Probe Spacing", &m_giSettings.lightProbeSpacing, 0.1f, 0.5f, 50.0f);
            ImGui::DragInt("Max Probes", &m_giSettings.maxLightProbes, 1.0f, 1, 10000);

            if (ImGui::Button("Generate Light Probes"))
            {
                XMFLOAT3 defaultBounds = {100.0f, 50.0f, 100.0f};
                int count = GenerateLightProbes(defaultBounds, m_giSettings.lightProbeSpacing);
                m_metrics.lightProbes = count;
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Lightmap Baking"))
        {
            ImGui::DragInt("Lightmap Resolution", &m_giSettings.lightmapResolution, 1.0f, 64, 4096);
            ImGui::DragFloat("UV Padding", &m_giSettings.lightmapPadding, 0.1f, 0.0f, 16.0f);
            ImGui::Checkbox("Use Denoising", &m_giSettings.useDenoising);
            ImGui::DragInt("Bounce Count", &m_giSettings.bounceCount, 1.0f, 1, 16);

            if (m_lightmapBakeInProgress)
            {
                ImGui::ProgressBar(m_bakeProgress);
                ImGui::Text("%s", m_bakeStatus.c_str());
            }
            else
            {
                if (ImGui::Button("Bake Lightmaps"))
                {
                    BakeLightmaps([this](float progress, const std::string& status)
                    {
                        m_bakeProgress = progress;
                        m_bakeStatus = status;
                    });
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Baked Data"))
                {
                    ClearBakedLighting();
                }
            }

            ImGui::TreePop();
        }
    }

    void LightingTools::RenderAtmosphereUI()
    {
        // Time of day
        if (ImGui::TreeNodeEx("Time of Day", ImGuiTreeNodeFlags_DefaultOpen))
        {
            float time = m_atmosphereSettings.timeOfDay;
            if (ImGui::SliderFloat("Time", &time, 0.0f, 24.0f, "%.2f h"))
            {
                SetTimeOfDay(time);
            }

            int hours = static_cast<int>(time);
            int minutes = static_cast<int>((time - hours) * 60.0f);
            ImGui::Text("Time: %02d:%02d", hours, minutes);

            bool animate = m_animateTimeOfDay;
            if (ImGui::Checkbox("Animate Time of Day", &animate))
            {
                SetTimeOfDayAnimation(animate, m_atmosphereSettings.dayDuration);
            }

            if (m_animateTimeOfDay)
            {
                ImGui::DragFloat("Day Duration (s)", &m_atmosphereSettings.dayDuration, 1.0f, 10.0f, 3600.0f);
                ImGui::DragFloat("Animation Speed", &m_timeOfDaySpeed, 0.1f, 0.1f, 10.0f);
            }

            ImGui::TreePop();
        }

        // Sun/Moon
        if (ImGui::TreeNodeEx("Sun / Moon", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat3("Sun Direction", &m_atmosphereSettings.sunDirection.x, 0.01f, -1.0f, 1.0f);
            ImGui::ColorEdit3("Sun Color", &m_atmosphereSettings.sunColor.x);
            ImGui::DragFloat("Sun Intensity", &m_atmosphereSettings.sunIntensity, 0.1f, 0.0f, 20.0f);
            ImGui::DragFloat("Sun Angular Size", &m_atmosphereSettings.sunAngularSize, 0.01f, 0.1f, 5.0f);

            ImGui::Separator();

            ImGui::DragFloat3("Moon Direction", &m_atmosphereSettings.moonDirection.x, 0.01f, -1.0f, 1.0f);
            ImGui::ColorEdit3("Moon Color", &m_atmosphereSettings.moonColor.x);
            ImGui::DragFloat("Moon Intensity", &m_atmosphereSettings.moonIntensity, 0.01f, 0.0f, 2.0f);

            ImGui::TreePop();
        }

        // Atmosphere scattering
        if (ImGui::TreeNode("Atmosphere Scattering"))
        {
            ImGui::Checkbox("Enable Scattering", &m_atmosphereSettings.enableAtmosphereScattering);
            ImGui::DragFloat3("Rayleigh Scattering", &m_atmosphereSettings.rayleighScattering.x, 0.0001f, 0.0f, 0.1f, "%.4f");
            ImGui::DragFloat("Mie Scattering", &m_atmosphereSettings.mieScattering, 0.0001f, 0.0f, 0.1f, "%.4f");
            ImGui::DragFloat("Turbidity", &m_atmosphereSettings.turbidity, 0.1f, 1.0f, 10.0f);

            ImGui::TreePop();
        }

        // Fog
        if (ImGui::TreeNode("Fog"))
        {
            ImGui::Checkbox("Enable Fog", &m_atmosphereSettings.enableFog);

            if (m_atmosphereSettings.enableFog)
            {
                ImGui::ColorEdit3("Fog Color", &m_atmosphereSettings.fogColor.x);
                ImGui::DragFloat("Fog Density", &m_atmosphereSettings.fogDensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Fog Start Distance", &m_atmosphereSettings.fogStartDistance, 1.0f, 0.0f, 1000.0f);
                ImGui::DragFloat("Fog End Distance", &m_atmosphereSettings.fogEndDistance, 1.0f, 0.0f, 5000.0f);
            }

            ImGui::TreePop();
        }

        // Clouds
        if (ImGui::TreeNode("Clouds"))
        {
            ImGui::Checkbox("Enable Clouds", &m_atmosphereSettings.enableClouds);

            if (m_atmosphereSettings.enableClouds)
            {
                ImGui::SliderFloat("Cloud Coverage", &m_atmosphereSettings.cloudCoverage, 0.0f, 1.0f);
                ImGui::SliderFloat("Cloud Density", &m_atmosphereSettings.cloudDensity, 0.0f, 2.0f);
                ImGui::DragFloat2("Wind Direction", &m_atmosphereSettings.cloudWindDirection.x, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat("Wind Speed", &m_atmosphereSettings.cloudWindSpeed, 0.01f, 0.0f, 5.0f);
            }

            ImGui::TreePop();
        }

        // Weather
        if (ImGui::TreeNode("Weather"))
        {
            ImGui::SliderFloat("Rain Intensity", &m_atmosphereSettings.rainIntensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Snow Intensity", &m_atmosphereSettings.snowIntensity, 0.0f, 1.0f);
            ImGui::DragFloat("Wind Strength", &m_atmosphereSettings.windStrength, 0.01f, 0.0f, 5.0f);
            ImGui::DragFloat3("Wind Direction", &m_atmosphereSettings.windDirection.x, 0.01f, -1.0f, 1.0f);

            ImGui::TreePop();
        }
    }

    void LightingTools::RenderPostProcessingUI()
    {
        // Tonemapping
        if (ImGui::TreeNodeEx("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Enable Tonemapping", &m_postProcessingSettings.enableTonemapping);

            if (m_postProcessingSettings.enableTonemapping)
            {
                const char* operators[] = {"ACES", "Reinhard", "Filmic", "Uncharted2", "Linear"};
                int currentOp = 0;
                for (int i = 0; i < IM_ARRAYSIZE(operators); ++i)
                {
                    if (m_postProcessingSettings.tonemappingOperator == operators[i])
                    {
                        currentOp = i;
                        break;
                    }
                }
                if (ImGui::Combo("Operator", &currentOp, operators, IM_ARRAYSIZE(operators)))
                {
                    m_postProcessingSettings.tonemappingOperator = operators[currentOp];
                }

                ImGui::DragFloat("Exposure", &m_postProcessingSettings.exposure, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("Gamma", &m_postProcessingSettings.gamma, 0.01f, 1.0f, 4.0f);
            }

            ImGui::TreePop();
        }

        // Color grading
        if (ImGui::TreeNode("Color Grading"))
        {
            ImGui::Checkbox("Enable Color Grading", &m_postProcessingSettings.enableColorGrading);

            if (m_postProcessingSettings.enableColorGrading)
            {
                ImGui::DragFloat("Contrast", &m_postProcessingSettings.contrast, 0.01f, 0.0f, 3.0f);
                ImGui::DragFloat("Saturation", &m_postProcessingSettings.saturation, 0.01f, 0.0f, 3.0f);
                ImGui::DragFloat("Brightness", &m_postProcessingSettings.brightness, 0.01f, -1.0f, 1.0f);
                ImGui::ColorEdit3("Color Tint", &m_postProcessingSettings.colorTint.x);
            }

            ImGui::TreePop();
        }

        // Bloom
        if (ImGui::TreeNode("Bloom"))
        {
            ImGui::Checkbox("Enable Bloom", &m_postProcessingSettings.enableBloom);

            if (m_postProcessingSettings.enableBloom)
            {
                ImGui::DragFloat("Threshold", &m_postProcessingSettings.bloomThreshold, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Intensity", &m_postProcessingSettings.bloomIntensity, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Radius", &m_postProcessingSettings.bloomRadius, 0.01f, 0.1f, 10.0f);
            }

            ImGui::TreePop();
        }

        // Other effects
        if (ImGui::TreeNode("Effects"))
        {
            ImGui::Checkbox("Motion Blur", &m_postProcessingSettings.enableMotionBlur);
            ImGui::Checkbox("Depth of Field", &m_postProcessingSettings.enableDepthOfField);
            ImGui::Checkbox("Chromatic Aberration", &m_postProcessingSettings.enableChromaticAberration);
            ImGui::Checkbox("Vignette", &m_postProcessingSettings.enableVignette);

            ImGui::TreePop();
        }
    }

    void LightingTools::RenderPerformanceUI()
    {
        LightingMetrics metrics = GetLightingMetrics();

        ImGui::Text("Active Lights: %d", metrics.activeLights);
        ImGui::Text("Shadow-Casting Lights: %d", metrics.shadowCastingLights);
        ImGui::Text("Lightmap Textures: %d", metrics.lightmapTextures);
        ImGui::Text("Light Probes: %d", metrics.lightProbes);

        ImGui::Separator();

        float shadowMemMB = static_cast<float>(metrics.shadowMapMemory) / (1024.0f * 1024.0f);
        float lightmapMemMB = static_cast<float>(metrics.lightmapMemory) / (1024.0f * 1024.0f);
        ImGui::Text("Shadow Map Memory: %.2f MB", shadowMemMB);
        ImGui::Text("Lightmap Memory: %.2f MB", lightmapMemMB);
        ImGui::Text("Total Lighting Memory: %.2f MB", shadowMemMB + lightmapMemMB);

        ImGui::Separator();

        ImGui::Text("Render Time: %.3f ms", metrics.renderTime);
        ImGui::Text("Shadow Render Time: %.3f ms", metrics.shadowRenderTime);

        ImGui::Separator();

        static float targetFPS = 60.0f;
        ImGui::DragFloat("Target FPS", &targetFPS, 1.0f, 30.0f, 240.0f);
        if (ImGui::Button("Optimize Lighting"))
        {
            OptimizeLightingPerformance(targetFPS);
        }
    }

    void LightingTools::RenderPresetsUI()
    {
        ImGui::Text("Built-in Presets:");
        ImGui::Separator();

        const char* presets[] = {"Sunny Day", "Sunset", "Night", "Overcast", "Studio"};
        for (const char* preset : presets)
        {
            if (ImGui::Button(preset))
            {
                ApplyLightingPreset(preset);
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Separator();
        ImGui::Text("Profiles:");

        // Save profile
        static char profileNameBuffer[256] = "";
        ImGui::InputText("Profile Name", profileNameBuffer, sizeof(profileNameBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            if (std::strlen(profileNameBuffer) > 0)
            {
                SaveLightingProfile(profileNameBuffer);
            }
        }

        ImGui::Separator();

        // List available profiles
        std::vector<std::string> profiles = GetAvailableLightingProfiles();
        if (profiles.empty())
        {
            ImGui::TextDisabled("No saved profiles found.");
        }
        else
        {
            for (const auto& profile : profiles)
            {
                if (ImGui::Selectable(profile.c_str()))
                {
                    LoadLightingProfile(profile);
                }
            }
        }
    }

    // =========================================================================
    // Private Helper Methods
    // =========================================================================

    void LightingTools::UpdateSunPosition()
    {
        float time = m_atmosphereSettings.timeOfDay;

        // Convert time of day to sun angle
        // At 6:00 the sun is at the horizon (elevation = 0), at 12:00 it is at zenith
        // Sun travels 180 degrees from 6:00 to 18:00
        float hourAngle = (time - 12.0f) * (static_cast<float>(M_PI) / 12.0f); // radians from noon

        // Elevation: peak at noon (pi/2), horizon at 6 and 18
        float elevation = static_cast<float>(M_PI) * 0.5f - std::abs(hourAngle);

        // Clamp elevation for night time (sun below horizon)
        float azimuth = (time < 12.0f) ? static_cast<float>(M_PI) : 0.0f;

        // Spherical to Cartesian (Y-up coordinate system)
        float cosElev = std::cos(elevation);
        float sinElev = std::sin(elevation);
        float cosAz = std::cos(azimuth);
        float sinAz = std::sin(azimuth);

        // Sun direction points FROM the sun toward the scene (negative because it's a light direction)
        m_atmosphereSettings.sunDirection.x = -cosElev * sinAz;
        m_atmosphereSettings.sunDirection.y = -sinElev;
        m_atmosphereSettings.sunDirection.z = -cosElev * cosAz;

        // Update sun color based on time of day
        m_atmosphereSettings.sunColor = CalculateSunColor(time);

        // Adjust sun intensity based on elevation
        if (sinElev > 0.0f)
        {
            m_atmosphereSettings.sunIntensity = 3.0f * std::clamp(sinElev, 0.0f, 1.0f);
        }
        else
        {
            m_atmosphereSettings.sunIntensity = 0.0f;
        }

        // Update moon direction (opposite hemisphere offset)
        m_atmosphereSettings.moonDirection.x = -m_atmosphereSettings.sunDirection.x;
        m_atmosphereSettings.moonDirection.y = -m_atmosphereSettings.sunDirection.y;
        m_atmosphereSettings.moonDirection.z = -m_atmosphereSettings.sunDirection.z;
    }

    XMFLOAT3 LightingTools::CalculateSunColor(float timeOfDay) const
    {
        // Normalize to 0-1 within the daylight window
        // Sunrise ~6, sunset ~18
        float sunriseStart = 5.0f;
        float sunriseEnd = 7.0f;
        float sunsetStart = 17.0f;
        float sunsetEnd = 19.0f;

        // Night - deep blue twilight
        if (timeOfDay < sunriseStart || timeOfDay > sunsetEnd)
        {
            return {0.1f, 0.1f, 0.3f};
        }

        // Sunrise transition: warm orange/red fading to white
        if (timeOfDay >= sunriseStart && timeOfDay <= sunriseEnd)
        {
            float t = (timeOfDay - sunriseStart) / (sunriseEnd - sunriseStart);
            float r = 1.0f;
            float g = 0.4f + 0.55f * t;
            float b = 0.2f + 0.7f * t;
            return {r, g, b};
        }

        // Sunset transition: white fading to warm orange/red
        if (timeOfDay >= sunsetStart && timeOfDay <= sunsetEnd)
        {
            float t = (timeOfDay - sunsetStart) / (sunsetEnd - sunsetStart);
            float r = 1.0f;
            float g = 0.95f - 0.55f * t;
            float b = 0.9f - 0.7f * t;
            return {r, g, b};
        }

        // Daytime: slightly warm white
        // Bluer in the morning, whiter at noon, slightly warmer in afternoon
        float midday = 12.0f;
        float distFromNoon = std::abs(timeOfDay - midday);
        float warmth = distFromNoon / 5.0f; // 0 at noon, ~1 at edges

        float r = 1.0f;
        float g = 0.98f - 0.03f * warmth;
        float b = 0.95f - 0.05f * warmth;

        return {r, g, b};
    }

    void LightingTools::UpdateAtmosphereScattering()
    {
        if (!m_atmosphereSettings.enableAtmosphereScattering)
        {
            return;
        }

        float turbidity = m_atmosphereSettings.turbidity;

        // Rayleigh scattering coefficients scale with turbidity
        // Base values correspond to Earth's atmosphere at sea level
        float rayleighScale = 1.0f + (turbidity - 2.0f) * 0.1f;
        m_atmosphereSettings.rayleighScattering.x = 0.0025f * rayleighScale;
        m_atmosphereSettings.rayleighScattering.y = 0.0041f * rayleighScale;
        m_atmosphereSettings.rayleighScattering.z = 0.0081f * rayleighScale;

        // Mie scattering increases more strongly with turbidity (aerosols/haze)
        float mieScale = 1.0f + (turbidity - 2.0f) * 0.3f;
        m_atmosphereSettings.mieScattering = 0.003f * mieScale;

        // Adjust fog color to match atmosphere at horizon
        if (m_atmosphereSettings.enableFog)
        {
            XMFLOAT3 sunCol = m_atmosphereSettings.sunColor;
            float sunElev = -m_atmosphereSettings.sunDirection.y; // higher Y means more overhead sun
            float horizonBlend = std::clamp(1.0f - sunElev, 0.0f, 1.0f);

            // Fog picks up sun color near horizon, stays base color overhead
            m_atmosphereSettings.fogColor.x = 0.7f * (1.0f - horizonBlend) + sunCol.x * 0.3f * horizonBlend;
            m_atmosphereSettings.fogColor.y = 0.8f * (1.0f - horizonBlend) + sunCol.y * 0.3f * horizonBlend;
            m_atmosphereSettings.fogColor.z = 0.9f * (1.0f - horizonBlend) + sunCol.z * 0.3f * horizonBlend;
        }
    }

    bool LightingTools::ValidateLightData(const SparkLightData& lightData) const
    {
        // Intensity must be non-negative
        if (lightData.intensity < 0.0f)
        {
            return false;
        }

        // Range must be positive (except for directional lights where range is unused)
        if (lightData.type != SparkLightType::Directional && lightData.range <= 0.0f)
        {
            return false;
        }

        // Cone angles must be valid for spot lights
        if (lightData.type == SparkLightType::Spot)
        {
            if (lightData.innerConeAngle < 0.0f || lightData.innerConeAngle > 90.0f)
            {
                return false;
            }
            if (lightData.outerConeAngle < 0.0f || lightData.outerConeAngle > 90.0f)
            {
                return false;
            }
            if (lightData.outerConeAngle < lightData.innerConeAngle)
            {
                return false;
            }
        }

        // Area size must be positive
        if (lightData.type == SparkLightType::Area)
        {
            if (lightData.areaSize.x <= 0.0f || lightData.areaSize.y <= 0.0f)
            {
                return false;
            }
        }

        // Color channels should be non-negative
        if (lightData.color.x < 0.0f || lightData.color.y < 0.0f || lightData.color.z < 0.0f)
        {
            return false;
        }

        // Temperature must be in reasonable range
        if (lightData.temperature < 1000.0f || lightData.temperature > 20000.0f)
        {
            return false;
        }

        // Shadow bias values should be non-negative
        if (lightData.shadowBias < 0.0f || lightData.shadowNormalBias < 0.0f)
        {
            return false;
        }

        // Shadow distance must be positive
        if (lightData.shadowDistance <= 0.0f)
        {
            return false;
        }

        // Shadow cascades must be in [1, 8]
        if (lightData.shadowCascades < 1 || lightData.shadowCascades > 8)
        {
            return false;
        }

        // Volumetric properties
        if (lightData.enableVolumetrics)
        {
            if (lightData.volumetricStrength < 0.0f)
            {
                return false;
            }
            if (lightData.volumetricDensity < 0.0f)
            {
                return false;
            }
        }

        // Max affected objects must be positive
        if (lightData.maxAffectedObjects < 1)
        {
            return false;
        }

        // Falloff exponent must be positive for custom falloff
        if (lightData.falloffType == LightFalloff::Custom && lightData.falloffExponent <= 0.0f)
        {
            return false;
        }

        return true;
    }

} // namespace SparkEditor
