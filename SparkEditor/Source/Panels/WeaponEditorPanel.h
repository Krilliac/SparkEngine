/**
 * @file WeaponEditorPanel.h
 * @brief Weapon configuration and balancing editor panel
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /** @brief Editor panel for configuring weapon stats, viewing DPS charts, and comparing weapons. */
    class WeaponEditorPanel : public EditorPanel
    {
      public:
        WeaponEditorPanel();
        ~WeaponEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderWeaponList();       ///< Draw the weapon selection list.
        void RenderWeaponProperties(); ///< Draw stat sliders for the selected weapon.
        void RenderWeaponPreview();    ///< Draw the weapon model/animation preview.
        void RenderDPSChart();         ///< Draw the DPS-over-range chart.
        void RenderComparisonTable();  ///< Draw side-by-side weapon stat comparison.
        void SaveAllWeapons();         ///< Serialize modified weapon data to disk.

        /// Editable weapon entry displayed in the weapon list.
        struct WeaponEntry
        {
            std::string name;     ///< Weapon display name (e.g. "AK-47").
            int typeId;           ///< Weapon type enum value.
            float damage;         ///< Base damage per hit.
            float fireRate;       ///< Rounds per second.
            int magazineSize;     ///< Rounds per magazine.
            float reloadTime;     ///< Time to reload (seconds).
            float muzzleVelocity; ///< Projectile speed (m/s).
            float accuracy;       ///< Base accuracy [0, 1] (1 = perfect).
            float range;          ///< Effective range (meters).
            bool isModified;      ///< True if stats differ from the saved version.
        };

        std::vector<WeaponEntry> m_weapons; ///< All loaded weapon entries.
        int m_selectedWeapon = 0;           ///< Index of the currently selected weapon.
        bool m_showComparison = false;      ///< Whether the comparison table is visible.
        bool m_showDPSChart = true;         ///< Whether the DPS chart is visible.
        float m_previewTime = 0.0f;         ///< Accumulated time for weapon preview animation.
    };

} // namespace SparkEditor
