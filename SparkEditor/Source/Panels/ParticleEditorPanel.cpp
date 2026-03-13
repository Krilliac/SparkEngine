/**
 * @file ParticleEditorPanel.cpp
 * @brief Visual particle system editor implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "ParticleEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace SparkEditor
{

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    ParticleEditorPanel::ParticleEditorPanel() : EditorPanel("Particle Editor", "particle_editor_panel")
    {
        SetIcon(ICON_FA_FIRE);
    }

    ParticleEditorPanel::~ParticleEditorPanel() = default;

    // =========================================================================
    // EditorPanel interface
    // =========================================================================

    bool ParticleEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(LogCategory::Editor);
        std::cout << "Initializing Particle Editor panel\n";
        InitializePresets();
        NewEffect();
        m_isInitialized = true;
        return true;
    }

    void ParticleEditorPanel::Update(float deltaTime)
    {
        if (m_previewPlaying)
        {
            m_previewTime += deltaTime * m_previewSpeed;
        }
    }

    void ParticleEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderToolbar();
        ImGui::Separator();

        // Layout: emitter list left, properties center, preview right
        float listWidth = 180.0f;
        float availWidth = ImGui::GetContentRegionAvail().x;
        float availHeight = ImGui::GetContentRegionAvail().y;

        // Emitter list
        ImGui::BeginChild("EmitterList", ImVec2(listWidth, availHeight), true);
        RenderEmitterList();
        ImGui::EndChild();

        ImGui::SameLine();

        // Properties
        float propsWidth = availWidth - listWidth - 8.0f;
        ImGui::BeginChild("EmitterProps", ImVec2(propsWidth, availHeight), true);

        if (m_showPresetLibrary)
        {
            RenderPresetLibrary();
        }
        else if (m_currentEffect && m_selectedEmitterIndex >= 0 &&
                 m_selectedEmitterIndex < static_cast<int>(m_currentEffect->emitters.size()))
        {
            RenderEmitterProperties();
        }
        else
        {
            ImGui::TextDisabled("Select an emitter to edit its properties");
        }

        ImGui::EndChild();

        EndPanel();
    }

    void ParticleEditorPanel::Shutdown()
    {
        m_currentEffect.reset();
        m_presets.clear();
        std::cout << "Particle Editor panel shut down\n";
    }

    // =========================================================================
    // Public API
    // =========================================================================

    void ParticleEditorPanel::NewEffect()
    {
        m_currentEffect = std::make_unique<ParticleEffectAsset>();
        m_currentEffect->name = "New Effect";

        ParticleEmitterConfig defaultEmitter;
        defaultEmitter.name = "Emitter 1";
        m_currentEffect->emitters.push_back(defaultEmitter);

        m_selectedEmitterIndex = 0;
        m_currentEffect->isModified = false;
        m_previewTime = 0.0f;
    }

    bool ParticleEditorPanel::LoadEffect(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            std::cout << "Failed to open particle effect: " << filePath << "\n";
            return false;
        }

        auto effect = std::make_unique<ParticleEffectAsset>();
        effect->filePath = filePath;

        std::string line;
        ParticleEmitterConfig* currentEmitter = nullptr;

        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            if (line.rfind("NAME:", 0) == 0)
            {
                effect->name = line.substr(5);
            }
            else if (line.rfind("EMITTER:", 0) == 0)
            {
                effect->emitters.emplace_back();
                currentEmitter = &effect->emitters.back();
                currentEmitter->name = line.substr(8);
            }
            else if (currentEmitter)
            {
                if (line.rfind("  RATE:", 0) == 0)
                    currentEmitter->emissionRate = std::stof(line.substr(7));
                else if (line.rfind("  DURATION:", 0) == 0)
                    currentEmitter->duration = std::stof(line.substr(11));
                else if (line.rfind("  LOOP:", 0) == 0)
                    currentEmitter->looping = line.substr(7) == "true";
                else if (line.rfind("  MAX:", 0) == 0)
                    currentEmitter->maxParticles = std::stoi(line.substr(6));
                else if (line.rfind("  SHAPE:", 0) == 0)
                    currentEmitter->shape = static_cast<EmitterShape>(std::stoi(line.substr(8)));
                else if (line.rfind("  GRAVITY:", 0) == 0)
                    currentEmitter->gravityModifier = std::stof(line.substr(10));
                else if (line.rfind("  BLEND:", 0) == 0)
                    currentEmitter->blendMode = static_cast<ParticleBlendMode>(std::stoi(line.substr(8)));
                else if (line.rfind("  TEXTURE:", 0) == 0)
                    currentEmitter->texturePath = line.substr(10);
            }
        }

        m_currentEffect = std::move(effect);
        m_currentEffect->isModified = false;
        m_selectedEmitterIndex = m_currentEffect->emitters.empty() ? -1 : 0;
        m_previewTime = 0.0f;
        std::cout << "Loaded particle effect: " << m_currentEffect->name << " (" << m_currentEffect->emitters.size()
                  << " emitters)\n";
        return true;
    }

    bool ParticleEditorPanel::SaveEffect(const std::string& filePath)
    {
        if (!m_currentEffect)
            return false;

        std::string path = filePath.empty() ? m_currentEffect->filePath : filePath;
        if (path.empty())
            path = m_currentEffect->name + ".sparkfx";

        std::ofstream file(path);
        if (!file.is_open())
        {
            std::cout << "Failed to save particle effect to: " << path << "\n";
            return false;
        }

        file << "# Spark Particle Effect\n";
        file << "NAME:" << m_currentEffect->name << "\n";

        for (const auto& emitter : m_currentEffect->emitters)
        {
            file << "EMITTER:" << emitter.name << "\n";
            file << "  RATE:" << emitter.emissionRate << "\n";
            file << "  DURATION:" << emitter.duration << "\n";
            file << "  LOOP:" << (emitter.looping ? "true" : "false") << "\n";
            file << "  MAX:" << emitter.maxParticles << "\n";
            file << "  SHAPE:" << static_cast<int>(emitter.shape) << "\n";
            file << "  GRAVITY:" << emitter.gravityModifier << "\n";
            file << "  BLEND:" << static_cast<int>(emitter.blendMode) << "\n";
            file << "  TEXTURE:" << emitter.texturePath << "\n";
        }

        m_currentEffect->filePath = path;
        m_currentEffect->isModified = false;
        std::cout << "Saved particle effect to: " << path << "\n";
        return true;
    }

    void ParticleEditorPanel::ApplyPreset(const std::string& presetName)
    {
        auto it = m_presets.find(presetName);
        if (it == m_presets.end())
            return;

        if (!m_currentEffect)
            NewEffect();

        if (m_selectedEmitterIndex >= 0 && m_selectedEmitterIndex < static_cast<int>(m_currentEffect->emitters.size()))
        {
            std::string oldName = m_currentEffect->emitters[m_selectedEmitterIndex].name;
            m_currentEffect->emitters[m_selectedEmitterIndex] = it->second;
            m_currentEffect->emitters[m_selectedEmitterIndex].name = oldName + " (" + presetName + ")";
            m_currentEffect->isModified = true;
        }
    }

    // =========================================================================
    // Render sub-sections
    // =========================================================================

    void ParticleEditorPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_FILE " New"))
        {
            NewEffect();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Create new particle effect");

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Load"))
        {
            // File dialog placeholder
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            SaveEffect();
        }

        SparkEditor::VerticalSeparator();

        // Playback controls
        if (m_previewPlaying)
        {
            if (ImGui::Button(ICON_FA_PAUSE " Pause"))
                m_previewPlaying = false;
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY " Play"))
                m_previewPlaying = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP " Restart"))
        {
            m_previewTime = 0.0f;
            m_previewPlaying = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("##Speed", &m_previewSpeed, 0.1f, 5.0f, "%.1fx");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Playback speed");

        SparkEditor::VerticalSeparator();

        // Preset library toggle
        bool presetActive = m_showPresetLibrary;
        if (presetActive)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));
        if (ImGui::Button(ICON_FA_SHAPES " Presets"))
        {
            m_showPresetLibrary = !m_showPresetLibrary;
        }
        if (presetActive)
            ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Bounds", &m_showBounds);

        // Effect name
        if (m_currentEffect)
        {
            SparkEditor::VerticalSeparator();

            char nameBuf[256];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_currentEffect->name.c_str());
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::InputText("##FXName", nameBuf, sizeof(nameBuf)))
            {
                m_currentEffect->name = nameBuf;
                m_currentEffect->isModified = true;
            }

            if (m_currentEffect->isModified)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "(unsaved)");
            }

            ImGui::SameLine();
            ImGui::Text("Time: %.1fs", m_previewTime);
        }
    }

    void ParticleEditorPanel::RenderEmitterList()
    {
        ImGui::Text(ICON_FA_FIRE " Emitters");
        ImGui::Separator();

        if (!m_currentEffect)
        {
            ImGui::TextDisabled("No effect loaded");
            return;
        }

        for (int i = 0; i < static_cast<int>(m_currentEffect->emitters.size()); ++i)
        {
            ImGui::PushID(i);
            auto& emitter = m_currentEffect->emitters[i];

            bool selected = (i == m_selectedEmitterIndex);
            ImGuiTreeNodeFlags flags =
                ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (selected)
                flags |= ImGuiTreeNodeFlags_Selected;

            // Enable/disable indicator
            if (!emitter.enabled)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

            ImGui::TreeNodeEx(emitter.name.c_str(), flags);

            if (!emitter.enabled)
                ImGui::PopStyleColor();

            if (ImGui::IsItemClicked())
            {
                m_selectedEmitterIndex = i;
                m_showPresetLibrary = false;
            }

            // Context menu
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Duplicate"))
                {
                    auto copy = emitter;
                    copy.name += " (Copy)";
                    m_currentEffect->emitters.insert(m_currentEffect->emitters.begin() + i + 1, copy);
                    m_currentEffect->isModified = true;
                }
                if (ImGui::MenuItem("Delete", nullptr, false, m_currentEffect->emitters.size() > 1))
                {
                    m_currentEffect->emitters.erase(m_currentEffect->emitters.begin() + i);
                    if (m_selectedEmitterIndex >= static_cast<int>(m_currentEffect->emitters.size()))
                        m_selectedEmitterIndex = static_cast<int>(m_currentEffect->emitters.size()) - 1;
                    m_currentEffect->isModified = true;
                    ImGui::EndPopup();
                    ImGui::PopID();
                    return;
                }
                if (i > 0 && ImGui::MenuItem("Move Up"))
                {
                    std::swap(m_currentEffect->emitters[i], m_currentEffect->emitters[i - 1]);
                    if (m_selectedEmitterIndex == i)
                        m_selectedEmitterIndex = i - 1;
                    m_currentEffect->isModified = true;
                }
                if (i < static_cast<int>(m_currentEffect->emitters.size()) - 1 && ImGui::MenuItem("Move Down"))
                {
                    std::swap(m_currentEffect->emitters[i], m_currentEffect->emitters[i + 1]);
                    if (m_selectedEmitterIndex == i)
                        m_selectedEmitterIndex = i + 1;
                    m_currentEffect->isModified = true;
                }
                ImGui::Separator();
                ImGui::MenuItem(emitter.enabled ? "Disable" : "Enable", nullptr, &emitter.enabled);
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button(ICON_FA_PLUS " Add Emitter", ImVec2(-1, 0)))
        {
            ParticleEmitterConfig newEmitter;
            newEmitter.name = "Emitter " + std::to_string(m_currentEffect->emitters.size() + 1);
            m_currentEffect->emitters.push_back(newEmitter);
            m_selectedEmitterIndex = static_cast<int>(m_currentEffect->emitters.size()) - 1;
            m_currentEffect->isModified = true;
        }
    }

    void ParticleEditorPanel::RenderEmitterProperties()
    {
        if (!m_currentEffect || m_selectedEmitterIndex < 0 ||
            m_selectedEmitterIndex >= static_cast<int>(m_currentEffect->emitters.size()))
            return;

        auto& e = m_currentEffect->emitters[m_selectedEmitterIndex];

        // Name
        char nameBuf[256];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", e.name.c_str());
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
        {
            e.name = nameBuf;
            m_currentEffect->isModified = true;
        }

        ImGui::Checkbox("Enabled", &e.enabled);

        // === General ===
        if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragFloat("Duration", &e.duration, 0.1f, 0.1f, 120.0f, "%.1fs"))
                m_currentEffect->isModified = true;
            if (ImGui::Checkbox("Looping", &e.looping))
                m_currentEffect->isModified = true;
            if (ImGui::DragFloat("Start Delay", &e.startDelay, 0.1f, 0.0f, 30.0f, "%.1fs"))
                m_currentEffect->isModified = true;
            if (ImGui::DragInt("Max Particles", &e.maxParticles, 10, 1, 100000))
                m_currentEffect->isModified = true;
        }

        // === Emission ===
        if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::DragFloat("Rate", &e.emissionRate, 1.0f, 0.0f, 10000.0f, "%.0f/s"))
                m_currentEffect->isModified = true;

            ImGui::Separator();
            ImGui::Text("Bursts:");
            RenderBurstList(e.bursts);
        }

        // === Shape ===
        if (ImGui::CollapsingHeader("Shape"))
        {
            const char* shapeNames[] = {"Point", "Sphere", "Hemisphere", "Cone", "Box", "Circle", "Edge"};
            int shapeIdx = static_cast<int>(e.shape);
            if (ImGui::Combo("Shape", &shapeIdx, shapeNames, 7))
            {
                e.shape = static_cast<EmitterShape>(shapeIdx);
                m_currentEffect->isModified = true;
            }

            if (e.shape == EmitterShape::Sphere || e.shape == EmitterShape::Hemisphere ||
                e.shape == EmitterShape::Cone || e.shape == EmitterShape::Circle)
            {
                if (ImGui::DragFloat("Radius", &e.shapeRadius, 0.01f, 0.01f, 100.0f))
                    m_currentEffect->isModified = true;
            }
            if (e.shape == EmitterShape::Cone)
            {
                if (ImGui::SliderFloat("Cone Angle", &e.shapeConeAngle, 0.0f, 90.0f, "%.1f deg"))
                    m_currentEffect->isModified = true;
            }
            if (e.shape == EmitterShape::Box)
            {
                if (ImGui::DragFloat("Width", &e.shapeBoxWidth, 0.01f, 0.01f, 100.0f))
                    m_currentEffect->isModified = true;
                if (ImGui::DragFloat("Height", &e.shapeBoxHeight, 0.01f, 0.01f, 100.0f))
                    m_currentEffect->isModified = true;
                if (ImGui::DragFloat("Depth", &e.shapeBoxDepth, 0.01f, 0.01f, 100.0f))
                    m_currentEffect->isModified = true;
            }
            if (ImGui::Checkbox("Emit From Shell", &e.emitFromShell))
                m_currentEffect->isModified = true;
        }

        // === Initial Values ===
        if (ImGui::CollapsingHeader("Initial Values"))
        {
            ImGui::Text("Lifetime:");
            if (ImGui::DragFloatRange2("##Lifetime", &e.startLifetimeMin, &e.startLifetimeMax, 0.1f, 0.01f, 60.0f,
                                       "Min: %.2f", "Max: %.2f"))
                m_currentEffect->isModified = true;

            ImGui::Text("Speed:");
            if (ImGui::DragFloatRange2("##Speed", &e.startSpeedMin, &e.startSpeedMax, 0.1f, 0.0f, 200.0f, "Min: %.1f",
                                       "Max: %.1f"))
                m_currentEffect->isModified = true;

            ImGui::Text("Size:");
            if (ImGui::DragFloatRange2("##Size", &e.startSizeMin, &e.startSizeMax, 0.01f, 0.001f, 50.0f, "Min: %.3f",
                                       "Max: %.3f"))
                m_currentEffect->isModified = true;

            ImGui::Text("Rotation:");
            if (ImGui::DragFloatRange2("##Rotation", &e.startRotationMin, &e.startRotationMax, 1.0f, 0.0f, 360.0f,
                                       "Min: %.0f", "Max: %.0f"))
                m_currentEffect->isModified = true;

            ImGui::Text("Start Color Min:");
            if (ImGui::ColorEdit4("##ColorMin", &e.startColorMin.x))
                m_currentEffect->isModified = true;
            ImGui::Text("Start Color Max:");
            if (ImGui::ColorEdit4("##ColorMax", &e.startColorMax.x))
                m_currentEffect->isModified = true;
        }

        // === Over Lifetime ===
        if (ImGui::CollapsingHeader("Over Lifetime"))
        {
            RenderCurveEditor("Size Over Lifetime", e.sizeOverLifetime);
            ImGui::Spacing();
            RenderCurveEditor("Speed Over Lifetime", e.speedOverLifetime);
            ImGui::Spacing();
            RenderColorGradient("Color Over Lifetime", e.colorOverLifetime);
        }

        // === Physics ===
        if (ImGui::CollapsingHeader("Physics"))
        {
            if (ImGui::DragFloat("Gravity Modifier", &e.gravityModifier, 0.01f, -10.0f, 10.0f))
                m_currentEffect->isModified = true;
            if (ImGui::DragFloat("Drag", &e.dragCoefficient, 0.01f, 0.0f, 10.0f))
                m_currentEffect->isModified = true;
            if (ImGui::Checkbox("World Space", &e.worldSpace))
                m_currentEffect->isModified = true;
        }

        // === Rendering ===
        if (ImGui::CollapsingHeader("Rendering"))
        {
            const char* blendNames[] = {"Additive", "Alpha Blend", "Multiply", "Premultiplied"};
            int blendIdx = static_cast<int>(e.blendMode);
            if (ImGui::Combo("Blend Mode", &blendIdx, blendNames, 4))
            {
                e.blendMode = static_cast<ParticleBlendMode>(blendIdx);
                m_currentEffect->isModified = true;
            }

            char texBuf[512];
            std::snprintf(texBuf, sizeof(texBuf), "%s", e.texturePath.c_str());
            if (ImGui::InputText("Texture", texBuf, sizeof(texBuf)))
            {
                e.texturePath = texBuf;
                m_currentEffect->isModified = true;
            }

            if (ImGui::DragInt("Sheet Rows", &e.textureSheetRows, 1, 1, 16))
                m_currentEffect->isModified = true;
            if (ImGui::DragInt("Sheet Cols", &e.textureSheetColumns, 1, 1, 16))
                m_currentEffect->isModified = true;
            if (ImGui::Checkbox("Animate Sheet", &e.animateTextureSheet))
                m_currentEffect->isModified = true;
            if (ImGui::Checkbox("Billboard", &e.billboardMode))
                m_currentEffect->isModified = true;
            if (ImGui::DragInt("Sort Order", &e.renderSortOrder))
                m_currentEffect->isModified = true;
        }

        // === Collision ===
        if (ImGui::CollapsingHeader("Collision"))
        {
            if (ImGui::Checkbox("Enable Collision", &e.enableCollision))
                m_currentEffect->isModified = true;

            if (e.enableCollision)
            {
                if (ImGui::SliderFloat("Bounce", &e.collisionBounce, 0.0f, 1.0f))
                    m_currentEffect->isModified = true;
                if (ImGui::SliderFloat("Lifetime Loss", &e.collisionLifetimeLoss, 0.0f, 1.0f))
                    m_currentEffect->isModified = true;
            }
        }

        // === Sub-Emitters ===
        if (ImGui::CollapsingHeader("Sub-Emitters"))
        {
            RenderSubEmitterList(e.subEmitters);
        }
    }

    void ParticleEditorPanel::RenderCurveEditor(const char* label, std::vector<ParticleCurveKey>& curve)
    {
        ImGui::Text("%s", label);

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 80.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(25, 25, 30, 255));
        drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                          IM_COL32(80, 80, 90, 255));

        // Grid lines
        for (int i = 1; i < 4; ++i)
        {
            float y = canvasPos.y + canvasSize.y * (static_cast<float>(i) / 4.0f);
            drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), IM_COL32(45, 45, 50, 255));
        }

        // Draw curve
        if (curve.size() >= 2)
        {
            for (size_t i = 0; i < curve.size() - 1; ++i)
            {
                float x1 = canvasPos.x + curve[i].time * canvasSize.x;
                float y1 = canvasPos.y + canvasSize.y - curve[i].value * canvasSize.y;
                float x2 = canvasPos.x + curve[i + 1].time * canvasSize.x;
                float y2 = canvasPos.y + canvasSize.y - curve[i + 1].value * canvasSize.y;

                y1 = std::clamp(y1, canvasPos.y, canvasPos.y + canvasSize.y);
                y2 = std::clamp(y2, canvasPos.y, canvasPos.y + canvasSize.y);

                drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(100, 200, 255, 255), 2.0f);
            }

            // Draw keyframe handles
            for (size_t i = 0; i < curve.size(); ++i)
            {
                float x = canvasPos.x + curve[i].time * canvasSize.x;
                float y = canvasPos.y + canvasSize.y - curve[i].value * canvasSize.y;
                y = std::clamp(y, canvasPos.y, canvasPos.y + canvasSize.y);

                bool isSelected = (static_cast<int>(i) == m_selectedCurveKeyIndex);
                ImU32 handleCol = isSelected ? IM_COL32(255, 200, 50, 255) : IM_COL32(200, 200, 200, 255);
                drawList->AddCircleFilled(ImVec2(x, y), 5.0f, handleCol);
                drawList->AddCircle(ImVec2(x, y), 5.0f, IM_COL32(255, 255, 255, 255));
            }
        }

        // Invisible button for interaction
        ImGui::InvisibleButton(label, canvasSize);

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            float normX = (mousePos.x - canvasPos.x) / canvasSize.x;
            float normY = 1.0f - (mousePos.y - canvasPos.y) / canvasSize.y;
            normX = std::clamp(normX, 0.0f, 1.0f);
            normY = std::clamp(normY, 0.0f, 1.0f);

            // Check if clicking on existing key
            m_selectedCurveKeyIndex = -1;
            for (int i = 0; i < static_cast<int>(curve.size()); ++i)
            {
                float kx = canvasPos.x + curve[i].time * canvasSize.x;
                float ky = canvasPos.y + canvasSize.y - curve[i].value * canvasSize.y;
                float dx = mousePos.x - kx;
                float dy = mousePos.y - ky;
                if (dx * dx + dy * dy < 64.0f)
                {
                    m_selectedCurveKeyIndex = i;
                    m_isDraggingCurveKey = true;
                    break;
                }
            }
        }

        if (m_isDraggingCurveKey && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && m_selectedCurveKeyIndex >= 0 &&
            m_selectedCurveKeyIndex < static_cast<int>(curve.size()))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            float normX = (mousePos.x - canvasPos.x) / canvasSize.x;
            float normY = 1.0f - (mousePos.y - canvasPos.y) / canvasSize.y;
            curve[m_selectedCurveKeyIndex].time = std::clamp(normX, 0.0f, 1.0f);
            curve[m_selectedCurveKeyIndex].value = std::clamp(normY, 0.0f, 2.0f);
            m_currentEffect->isModified = true;
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_isDraggingCurveKey = false;
        }

        // Right-click to add/remove keys
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ImVec2 mousePos = ImGui::GetMousePos();
            float normX = (mousePos.x - canvasPos.x) / canvasSize.x;
            float normY = 1.0f - (mousePos.y - canvasPos.y) / canvasSize.y;

            ParticleCurveKey newKey;
            newKey.time = std::clamp(normX, 0.0f, 1.0f);
            newKey.value = std::clamp(normY, 0.0f, 2.0f);
            curve.push_back(newKey);
            std::sort(curve.begin(), curve.end(),
                      [](const ParticleCurveKey& a, const ParticleCurveKey& b) { return a.time < b.time; });
            m_currentEffect->isModified = true;
        }
    }

    void ParticleEditorPanel::RenderColorGradient(const char* label, std::vector<ParticleColorKey>& gradient)
    {
        ImGui::Text("%s", label);

        ImVec2 barPos = ImGui::GetCursorScreenPos();
        ImVec2 barSize = ImVec2(ImGui::GetContentRegionAvail().x, 25.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Draw gradient bar
        if (gradient.size() >= 2)
        {
            int segments = static_cast<int>(barSize.x);
            for (int i = 0; i < segments; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(segments);

                // Find surrounding keys
                const ParticleColorKey* left = &gradient[0];
                const ParticleColorKey* right = &gradient[gradient.size() - 1];
                for (size_t k = 0; k < gradient.size() - 1; ++k)
                {
                    if (t >= gradient[k].time && t <= gradient[k + 1].time)
                    {
                        left = &gradient[k];
                        right = &gradient[k + 1];
                        break;
                    }
                }

                float blend = 0.0f;
                float range = right->time - left->time;
                if (range > 0.001f)
                    blend = (t - left->time) / range;

                ImVec4 col;
                col.x = left->color.x + (right->color.x - left->color.x) * blend;
                col.y = left->color.y + (right->color.y - left->color.y) * blend;
                col.z = left->color.z + (right->color.z - left->color.z) * blend;
                col.w = left->color.w + (right->color.w - left->color.w) * blend;

                float x = barPos.x + static_cast<float>(i);
                drawList->AddLine(ImVec2(x, barPos.y), ImVec2(x, barPos.y + barSize.y),
                                  ImGui::ColorConvertFloat4ToU32(col));
            }
        }
        else
        {
            drawList->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y),
                                    IM_COL32(128, 128, 128, 255));
        }

        drawList->AddRect(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), IM_COL32(80, 80, 90, 255));

        // Draw color stop markers
        for (size_t i = 0; i < gradient.size(); ++i)
        {
            float x = barPos.x + gradient[i].time * barSize.x;
            ImVec2 triTop = ImVec2(x, barPos.y + barSize.y);
            ImVec2 triLeft = ImVec2(x - 5.0f, barPos.y + barSize.y + 8.0f);
            ImVec2 triRight = ImVec2(x + 5.0f, barPos.y + barSize.y + 8.0f);
            drawList->AddTriangleFilled(triTop, triLeft, triRight, IM_COL32(220, 220, 220, 255));
        }

        ImGui::InvisibleButton(label, ImVec2(barSize.x, barSize.y + 10.0f));

        // Color editors for each stop
        for (size_t i = 0; i < gradient.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(60.0f);
            if (ImGui::DragFloat("##t", &gradient[i].time, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                m_currentEffect->isModified = true;
            }
            ImGui::SameLine();
            if (ImGui::ColorEdit4("##c", &gradient[i].color.x, ImGuiColorEditFlags_NoInputs))
            {
                m_currentEffect->isModified = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH) && gradient.size() > 2)
            {
                gradient.erase(gradient.begin() + static_cast<long>(i));
                m_currentEffect->isModified = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        if (ImGui::Button(ICON_FA_PLUS " Add Stop"))
        {
            ParticleColorKey key;
            key.time = 0.5f;
            key.color = ImVec4(1, 1, 1, 1);
            gradient.push_back(key);
            std::sort(gradient.begin(), gradient.end(),
                      [](const ParticleColorKey& a, const ParticleColorKey& b) { return a.time < b.time; });
            m_currentEffect->isModified = true;
        }
    }

    void ParticleEditorPanel::RenderBurstList(std::vector<ParticleBurst>& bursts)
    {
        for (int i = 0; i < static_cast<int>(bursts.size()); ++i)
        {
            ImGui::PushID(i);
            auto& burst = bursts[i];

            ImGui::SetNextItemWidth(60.0f);
            ImGui::DragFloat("##time", &burst.time, 0.1f, 0.0f, 120.0f, "%.1fs");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            ImGui::DragInt("##min", &burst.minCount, 1, 1, 10000);
            ImGui::SameLine();
            ImGui::Text("-");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            ImGui::DragInt("##max", &burst.maxCount, 1, 1, 10000);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH))
            {
                bursts.erase(bursts.begin() + i);
                m_currentEffect->isModified = true;
                ImGui::PopID();
                break;
            }

            m_currentEffect->isModified = true;
            ImGui::PopID();
        }

        if (ImGui::Button(ICON_FA_PLUS " Add Burst"))
        {
            ParticleBurst burst;
            burst.time = 0.0f;
            burst.minCount = 10;
            burst.maxCount = 20;
            bursts.push_back(burst);
            m_currentEffect->isModified = true;
        }
    }

    void ParticleEditorPanel::RenderSubEmitterList(std::vector<SubEmitterEntry>& subEmitters)
    {
        for (int i = 0; i < static_cast<int>(subEmitters.size()); ++i)
        {
            ImGui::PushID(i);
            auto& sub = subEmitters[i];

            const char* triggerNames[] = {"Birth", "Death", "Collision"};
            int triggerIdx = static_cast<int>(sub.trigger);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("##trigger", &triggerIdx, triggerNames, 3))
            {
                sub.trigger = static_cast<SubEmitterTrigger>(triggerIdx);
                m_currentEffect->isModified = true;
            }

            ImGui::SameLine();
            char nameBuf[256];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", sub.emitterName.c_str());
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
            {
                sub.emitterName = nameBuf;
                m_currentEffect->isModified = true;
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            if (ImGui::DragInt("##max", &sub.maxSpawn, 1, 1, 1000))
                m_currentEffect->isModified = true;

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH))
            {
                subEmitters.erase(subEmitters.begin() + i);
                m_currentEffect->isModified = true;
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }

        if (ImGui::Button(ICON_FA_PLUS " Add Sub-Emitter"))
        {
            SubEmitterEntry entry;
            entry.trigger = SubEmitterTrigger::Death;
            entry.emitterName = "SubEmitter";
            entry.maxSpawn = 5;
            subEmitters.push_back(entry);
            m_currentEffect->isModified = true;
        }
    }

    void ParticleEditorPanel::RenderPreviewControls()
    {
        if (m_previewPlaying)
        {
            if (ImGui::Button(ICON_FA_PAUSE))
                m_previewPlaying = false;
        }
        else
        {
            if (ImGui::Button(ICON_FA_PLAY))
                m_previewPlaying = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP))
        {
            m_previewTime = 0.0f;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("Speed", &m_previewSpeed, 0.1f, 5.0f, "%.1fx");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::SliderFloat("Zoom", &m_previewZoom, 1.0f, 50.0f, "%.1f");
    }

    void ParticleEditorPanel::RenderPresetLibrary()
    {
        ImGui::Text(ICON_FA_SHAPES " Preset Library");
        ImGui::Separator();

        if (m_presets.empty())
        {
            ImGui::TextDisabled("No presets available");
            return;
        }

        float buttonWidth = 120.0f;
        float availWidth = ImGui::GetContentRegionAvail().x;
        int cols = std::max(1, static_cast<int>(availWidth / (buttonWidth + 8.0f)));

        int col = 0;
        for (const auto& [name, config] : m_presets)
        {
            if (col > 0)
                ImGui::SameLine();

            ImGui::PushID(name.c_str());

            ImGui::BeginGroup();
            if (ImGui::Button(name.c_str(), ImVec2(buttonWidth, 40.0f)))
            {
                ApplyPreset(name);
                m_showPresetLibrary = false;
            }

            // Tooltip with preset info
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("Shape: %s", config.shape == EmitterShape::Cone     ? "Cone"
                                         : config.shape == EmitterShape::Sphere ? "Sphere"
                                         : config.shape == EmitterShape::Point  ? "Point"
                                         : config.shape == EmitterShape::Box    ? "Box"
                                         : config.shape == EmitterShape::Circle ? "Circle"
                                                                                : "Other");
                ImGui::Text("Rate: %.0f/s", config.emissionRate);
                ImGui::Text("Lifetime: %.1f-%.1fs", config.startLifetimeMin, config.startLifetimeMax);
                ImGui::Text("Max: %d particles", config.maxParticles);
                ImGui::EndTooltip();
            }

            ImGui::EndGroup();
            ImGui::PopID();

            col++;
            if (col >= cols)
                col = 0;
        }
    }

    // =========================================================================
    // Preset creation
    // =========================================================================

    void ParticleEditorPanel::InitializePresets()
    {
        m_presets["Fire"] = CreateFirePreset();
        m_presets["Smoke"] = CreateSmokePreset();
        m_presets["Sparks"] = CreateSparksPreset();
        m_presets["Blood"] = CreateBloodPreset();
        m_presets["Muzzle Flash"] = CreateMuzzleFlashPreset();
        m_presets["Explosion"] = CreateExplosionPreset();
        m_presets["Rain"] = CreateRainPreset();
        m_presets["Dust"] = CreateDustPreset();
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateFirePreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Fire";
        c.duration = 5.0f;
        c.looping = true;
        c.maxParticles = 500;
        c.emissionRate = 80.0f;
        c.shape = EmitterShape::Cone;
        c.shapeRadius = 0.3f;
        c.shapeConeAngle = 15.0f;
        c.startLifetimeMin = 0.5f;
        c.startLifetimeMax = 1.5f;
        c.startSpeedMin = 1.0f;
        c.startSpeedMax = 3.0f;
        c.startSizeMin = 0.2f;
        c.startSizeMax = 0.5f;
        c.startColorMin = ImVec4(1.0f, 0.6f, 0.1f, 1.0f);
        c.startColorMax = ImVec4(1.0f, 0.3f, 0.05f, 1.0f);
        c.sizeOverLifetime = {{0.0f, 0.5f}, {0.3f, 1.0f}, {1.0f, 0.0f}};
        c.colorOverLifetime = {
            {0.0f, {1.0f, 0.8f, 0.2f, 1.0f}}, {0.5f, {1.0f, 0.3f, 0.05f, 0.8f}}, {1.0f, {0.3f, 0.1f, 0.05f, 0.0f}}};
        c.gravityModifier = -0.5f;
        c.blendMode = ParticleBlendMode::Additive;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateSmokePreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Smoke";
        c.duration = 5.0f;
        c.looping = true;
        c.maxParticles = 300;
        c.emissionRate = 30.0f;
        c.shape = EmitterShape::Cone;
        c.shapeRadius = 0.5f;
        c.shapeConeAngle = 20.0f;
        c.startLifetimeMin = 2.0f;
        c.startLifetimeMax = 4.0f;
        c.startSpeedMin = 0.5f;
        c.startSpeedMax = 1.5f;
        c.startSizeMin = 0.3f;
        c.startSizeMax = 0.8f;
        c.startColorMin = ImVec4(0.4f, 0.4f, 0.4f, 0.6f);
        c.startColorMax = ImVec4(0.6f, 0.6f, 0.6f, 0.8f);
        c.sizeOverLifetime = {{0.0f, 0.3f}, {0.5f, 0.8f}, {1.0f, 1.5f}};
        c.colorOverLifetime = {
            {0.0f, {0.5f, 0.5f, 0.5f, 0.6f}}, {0.5f, {0.4f, 0.4f, 0.4f, 0.3f}}, {1.0f, {0.3f, 0.3f, 0.3f, 0.0f}}};
        c.gravityModifier = -0.3f;
        c.dragCoefficient = 0.5f;
        c.blendMode = ParticleBlendMode::AlphaBlend;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateSparksPreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Sparks";
        c.duration = 2.0f;
        c.looping = true;
        c.maxParticles = 200;
        c.emissionRate = 100.0f;
        c.shape = EmitterShape::Point;
        c.startLifetimeMin = 0.3f;
        c.startLifetimeMax = 0.8f;
        c.startSpeedMin = 5.0f;
        c.startSpeedMax = 15.0f;
        c.startSizeMin = 0.02f;
        c.startSizeMax = 0.06f;
        c.startColorMin = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        c.startColorMax = ImVec4(1.0f, 0.5f, 0.1f, 1.0f);
        c.sizeOverLifetime = {{0.0f, 1.0f}, {1.0f, 0.2f}};
        c.colorOverLifetime = {{0.0f, {1.0f, 0.9f, 0.5f, 1.0f}}, {1.0f, {1.0f, 0.3f, 0.0f, 0.0f}}};
        c.gravityModifier = 2.0f;
        c.blendMode = ParticleBlendMode::Additive;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateBloodPreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Blood";
        c.duration = 1.0f;
        c.looping = false;
        c.maxParticles = 100;
        c.emissionRate = 0.0f;
        c.bursts = {{0.0f, 20, 40, 1, 0.0f}};
        c.shape = EmitterShape::Sphere;
        c.shapeRadius = 0.1f;
        c.startLifetimeMin = 0.5f;
        c.startLifetimeMax = 1.2f;
        c.startSpeedMin = 2.0f;
        c.startSpeedMax = 6.0f;
        c.startSizeMin = 0.03f;
        c.startSizeMax = 0.08f;
        c.startColorMin = ImVec4(0.6f, 0.0f, 0.0f, 1.0f);
        c.startColorMax = ImVec4(0.8f, 0.05f, 0.05f, 1.0f);
        c.sizeOverLifetime = {{0.0f, 1.0f}, {0.5f, 0.8f}, {1.0f, 0.3f}};
        c.colorOverLifetime = {{0.0f, {0.7f, 0.0f, 0.0f, 1.0f}}, {1.0f, {0.3f, 0.0f, 0.0f, 0.0f}}};
        c.gravityModifier = 3.0f;
        c.blendMode = ParticleBlendMode::AlphaBlend;
        c.enableCollision = true;
        c.collisionBounce = 0.2f;
        c.collisionLifetimeLoss = 0.3f;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateMuzzleFlashPreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Muzzle Flash";
        c.duration = 0.1f;
        c.looping = false;
        c.maxParticles = 20;
        c.emissionRate = 0.0f;
        c.bursts = {{0.0f, 5, 10, 1, 0.0f}};
        c.shape = EmitterShape::Cone;
        c.shapeRadius = 0.05f;
        c.shapeConeAngle = 10.0f;
        c.startLifetimeMin = 0.03f;
        c.startLifetimeMax = 0.08f;
        c.startSpeedMin = 0.5f;
        c.startSpeedMax = 2.0f;
        c.startSizeMin = 0.1f;
        c.startSizeMax = 0.3f;
        c.startColorMin = ImVec4(1.0f, 0.9f, 0.5f, 1.0f);
        c.startColorMax = ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
        c.sizeOverLifetime = {{0.0f, 1.0f}, {1.0f, 2.0f}};
        c.colorOverLifetime = {{0.0f, {1.0f, 1.0f, 0.8f, 1.0f}}, {1.0f, {1.0f, 0.5f, 0.0f, 0.0f}}};
        c.blendMode = ParticleBlendMode::Additive;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateExplosionPreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Explosion";
        c.duration = 2.0f;
        c.looping = false;
        c.maxParticles = 500;
        c.emissionRate = 0.0f;
        c.bursts = {{0.0f, 50, 100, 1, 0.0f}, {0.05f, 30, 60, 1, 0.0f}};
        c.shape = EmitterShape::Sphere;
        c.shapeRadius = 0.5f;
        c.emitFromShell = true;
        c.startLifetimeMin = 0.5f;
        c.startLifetimeMax = 2.0f;
        c.startSpeedMin = 5.0f;
        c.startSpeedMax = 20.0f;
        c.startSizeMin = 0.2f;
        c.startSizeMax = 0.8f;
        c.startColorMin = ImVec4(1.0f, 0.6f, 0.1f, 1.0f);
        c.startColorMax = ImVec4(1.0f, 0.3f, 0.0f, 1.0f);
        c.sizeOverLifetime = {{0.0f, 0.5f}, {0.2f, 1.0f}, {1.0f, 0.0f}};
        c.colorOverLifetime = {{0.0f, {1.0f, 1.0f, 0.5f, 1.0f}},
                               {0.2f, {1.0f, 0.5f, 0.1f, 1.0f}},
                               {0.6f, {0.5f, 0.2f, 0.1f, 0.5f}},
                               {1.0f, {0.2f, 0.1f, 0.1f, 0.0f}}};
        c.gravityModifier = 1.0f;
        c.dragCoefficient = 1.0f;
        c.blendMode = ParticleBlendMode::Additive;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateRainPreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Rain";
        c.duration = 10.0f;
        c.looping = true;
        c.maxParticles = 5000;
        c.emissionRate = 500.0f;
        c.shape = EmitterShape::Box;
        c.shapeBoxWidth = 20.0f;
        c.shapeBoxHeight = 0.1f;
        c.shapeBoxDepth = 20.0f;
        c.startLifetimeMin = 0.8f;
        c.startLifetimeMax = 1.2f;
        c.startSpeedMin = 15.0f;
        c.startSpeedMax = 20.0f;
        c.startSizeMin = 0.01f;
        c.startSizeMax = 0.02f;
        c.startColorMin = ImVec4(0.6f, 0.7f, 0.8f, 0.5f);
        c.startColorMax = ImVec4(0.7f, 0.8f, 0.9f, 0.7f);
        c.sizeOverLifetime = {{0.0f, 1.0f}, {1.0f, 1.0f}};
        c.gravityModifier = 3.0f;
        c.blendMode = ParticleBlendMode::AlphaBlend;
        c.billboardMode = false;
        return c;
    }

    ParticleEmitterConfig ParticleEditorPanel::CreateDustPreset() const
    {
        ParticleEmitterConfig c;
        c.name = "Dust";
        c.duration = 5.0f;
        c.looping = true;
        c.maxParticles = 200;
        c.emissionRate = 20.0f;
        c.shape = EmitterShape::Box;
        c.shapeBoxWidth = 5.0f;
        c.shapeBoxHeight = 2.0f;
        c.shapeBoxDepth = 5.0f;
        c.startLifetimeMin = 3.0f;
        c.startLifetimeMax = 6.0f;
        c.startSpeedMin = 0.1f;
        c.startSpeedMax = 0.5f;
        c.startSizeMin = 0.02f;
        c.startSizeMax = 0.08f;
        c.startColorMin = ImVec4(0.7f, 0.65f, 0.55f, 0.2f);
        c.startColorMax = ImVec4(0.8f, 0.75f, 0.65f, 0.4f);
        c.sizeOverLifetime = {{0.0f, 0.5f}, {0.5f, 1.0f}, {1.0f, 0.5f}};
        c.colorOverLifetime = {
            {0.0f, {0.7f, 0.65f, 0.55f, 0.0f}}, {0.3f, {0.7f, 0.65f, 0.55f, 0.3f}}, {1.0f, {0.7f, 0.65f, 0.55f, 0.0f}}};
        c.gravityModifier = 0.05f;
        c.dragCoefficient = 0.3f;
        c.blendMode = ParticleBlendMode::AlphaBlend;
        return c;
    }

} // namespace SparkEditor
