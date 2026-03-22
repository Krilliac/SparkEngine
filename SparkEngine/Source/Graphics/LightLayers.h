/**
 * @file LightLayers.h
 * @brief Per-object light layer masking for selective lighting
 * @author Spark Engine Team
 * @date 2025
 *
 * Light layers allow per-object control over which lights affect which
 * renderers. A light only illuminates an object if their layer masks
 * have at least one bit in common (bitwise AND > 0).
 *
 * Use cases: character-only rim lights, interior-only lights that don't
 * bleed through walls, art-directed lighting on specific props.
 *
 * @see LightingSystem.h, ClusteredLightCulling.h
 */

#pragma once

#include <cstdint>
#include <string>

namespace Spark::Graphics
{

    /**
     * @brief 32-bit light layer mask
     *
     * Each bit represents a layer. Lights and renderers both have a mask.
     * A light affects an object iff (light.mask & object.mask) != 0.
     */
    struct LightLayerMask
    {
        uint32_t bits = 0xFFFFFFFF; ///< Default: all layers (everything lit)

        /** @brief Enable a specific layer (0-31) */
        void EnableLayer(int layer) { bits |= (1u << layer); }

        /** @brief Disable a specific layer */
        void DisableLayer(int layer) { bits &= ~(1u << layer); }

        /** @brief Check if a layer is enabled */
        bool HasLayer(int layer) const { return (bits & (1u << layer)) != 0; }

        /** @brief Set to a single layer only */
        void SetSingleLayer(int layer) { bits = (1u << layer); }

        /** @brief Set all layers (default) */
        void SetAll() { bits = 0xFFFFFFFF; }

        /** @brief Clear all layers (no lighting) */
        void ClearAll() { bits = 0; }

        /** @brief Check if this mask interacts with another */
        bool Interacts(const LightLayerMask& other) const { return (bits & other.bits) != 0; }

        /** @brief Count enabled layers */
        int CountLayers() const
        {
            uint32_t n = bits;
            n = n - ((n >> 1) & 0x55555555u);
            n = (n & 0x33333333u) + ((n >> 2) & 0x33333333u);
            return static_cast<int>(((n + (n >> 4)) & 0x0F0F0F0Fu) * 0x01010101u >> 24);
        }
    };

    /**
     * @brief Named light layer definitions
     *
     * Provides human-readable names for the 32 available light layers.
     * Used by the editor for layer assignment UI.
     */
    class LightLayerRegistry
    {
      public:
        static constexpr int kMaxLayers = 32;

        LightLayerRegistry()
        {
            // Default layer names
            m_names[0] = "Default";
            m_names[1] = "Characters";
            m_names[2] = "Environment";
            m_names[3] = "UI Elements";
            m_names[4] = "Vehicles";
            m_names[5] = "Weapons";
            m_names[6] = "Effects";
            m_names[7] = "Interior";
            for (int i = 8; i < kMaxLayers; ++i)
                m_names[i] = "Layer " + std::to_string(i);
        }

        void SetLayerName(int layer, const std::string& name)
        {
            if (layer >= 0 && layer < kMaxLayers)
                m_names[layer] = name;
        }

        const std::string& GetLayerName(int layer) const
        {
            static const std::string empty;
            if (layer >= 0 && layer < kMaxLayers)
                return m_names[layer];
            return empty;
        }

        /** @brief Find layer index by name, -1 if not found */
        int FindLayer(const std::string& name) const
        {
            for (int i = 0; i < kMaxLayers; ++i)
            {
                if (m_names[i] == name)
                    return i;
            }
            return -1;
        }

        std::string Console_GetStatus() const
        {
            std::string result = "Light Layers:\n";
            for (int i = 0; i < kMaxLayers; ++i)
            {
                if (m_names[i] != "Layer " + std::to_string(i))
                {
                    result += "  [" + std::to_string(i) + "] " + m_names[i] + "\n";
                }
            }
            return result;
        }

      private:
        std::string m_names[kMaxLayers];
    };

} // namespace Spark::Graphics
