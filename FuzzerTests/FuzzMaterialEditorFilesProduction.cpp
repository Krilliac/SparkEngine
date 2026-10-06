/** @file FuzzMaterialEditorFilesProduction.cpp
 * @brief Exercise the shipped .spkmat decoder and shared authoring defaults.
 */
#include "FuzzMaterialEditorFilesProduction.h"
#include "Panels/MaterialEditorMaterialParser.h"
#include "Panels/MaterialEditorPanel.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace
{
    void Require(bool condition)
    {
        if (!condition)
        {
            std::abort();
        }
    }

    bool SameDefinition(const SparkEditor::MaterialDefinition& a, const SparkEditor::MaterialDefinition& b)
    {
        if (a.name != b.name || a.shaderPath != b.shaderPath || a.filePath != b.filePath ||
            a.parameters.size() != b.parameters.size() || a.textureSlots.size() != b.textureSlots.size())
        {
            return false;
        }
        for (size_t index = 0; index < a.parameters.size(); ++index)
        {
            const auto& x = a.parameters[index];
            const auto& y = b.parameters[index];
            if (x.name != y.name || x.type != y.type || x.intValue != y.intValue || x.boolValue != y.boolValue ||
                x.texturePath != y.texturePath || std::memcmp(x.floatValues, y.floatValues, sizeof(x.floatValues)) != 0)
            {
                return false;
            }
        }
        for (size_t index = 0; index < a.textureSlots.size(); ++index)
        {
            const auto& x = a.textureSlots[index];
            const auto& y = b.textureSlots[index];
            if (x.name != y.name || x.bindSlot != y.bindSlot || x.texturePath != y.texturePath ||
                x.isAssigned != y.isAssigned || x.tilingU != y.tilingU || x.tilingV != y.tilingV ||
                x.offsetU != y.offsetU || x.offsetV != y.offsetV)
            {
                return false;
            }
        }
        const auto& x = a.renderState;
        const auto& y = b.renderState;
        return x.blendMode == y.blendMode && x.cullMode == y.cullMode && x.depthWrite == y.depthWrite &&
               x.depthTest == y.depthTest && x.castShadows == y.castShadows && x.receiveShadows == y.receiveShadows &&
               x.alphaClipThreshold == y.alphaClipThreshold && x.renderQueue == y.renderQueue;
    }
} // namespace

extern "C" int SparkFuzzParseMaterialEditorFiles(const uint8_t* data, std::size_t size)
{
    if (size > 65536 || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view input(data ? reinterpret_cast<const char*>(data) : "", size);
    SparkEditor::MaterialDefinition first, second;
    first.name = second.name = "retained";
    first.shaderPath = second.shaderPath = "Shaders/Retained.hlsl";
    first.filePath = second.filePath = "retained.spkmat";
    const auto original = first;
    const bool accepted = SparkEditor::ParseMaterialText(input, first);
    Require(SparkEditor::ParseMaterialText(input, second) == accepted);
    Require(SameDefinition(first, second));
    if (!accepted)
    {
        Require(SameDefinition(first, original));
        return 0;
    }
    Require(!first.name.empty() && !first.shaderPath.empty() && first.filePath == original.filePath);
    Require(!first.isModified && !first.isBuiltIn);
    Require(static_cast<int>(first.renderState.blendMode) >= 0 && static_cast<int>(first.renderState.blendMode) <= 4);
    Require(static_cast<int>(first.renderState.cullMode) >= 0 && static_cast<int>(first.renderState.cullMode) <= 2);
    Require(std::isfinite(first.renderState.alphaClipThreshold));
    for (const auto& parameter : first.parameters)
    {
        for (float value : parameter.floatValues)
        {
            Require(std::isfinite(value));
        }
    }
    for (const auto& slot : first.textureSlots)
    {
        Require(slot.bindSlot >= 0 && slot.isAssigned == !slot.texturePath.empty());
        Require(std::isfinite(slot.tilingU) && std::isfinite(slot.tilingV) && std::isfinite(slot.offsetU) &&
                std::isfinite(slot.offsetV));
    }
    Require(first.FindTextureSlot("Ambient Occlusion") != nullptr);
    return 0;
}
