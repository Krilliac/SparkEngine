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

    /**
     * @brief Weapon balance calculator: DPS charts and side-by-side stat comparison.
     *
     * The stat rows are the panel's own balance-study table. No game module reads
     * or writes them, so the panel offers no save: edits live only in this session
     * and Reset restores the starting values.
     */
    class WeaponEditorPanel : public EditorPanel
    {
      public:
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
            bool isModified;      ///< True if stats differ from the panel's starting values.
        };

        WeaponEditorPanel();
        ~WeaponEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        /// @brief Restore the selected weapon's starting values; false if nothing is selected.
        bool ResetSelectedWeapon();

        /// @brief The current (possibly edited) weapon table.
        const std::vector<WeaponEntry>& GetWeapons() const { return m_weapons; }
        std::vector<WeaponEntry>& GetWeapons() { return m_weapons; }

      private:
        void RenderWeaponList();       ///< Draw the weapon selection list.
        void RenderWeaponProperties(); ///< Draw stat sliders for the selected weapon.
        void RenderWeaponPreview();    ///< Draw the weapon model/animation preview.
        void RenderDPSChart();         ///< Draw the DPS-over-range chart.
        void RenderComparisonTable();  ///< Draw side-by-side weapon stat comparison.

        std::vector<WeaponEntry> m_weapons;  ///< All weapon entries as currently edited.
        std::vector<WeaponEntry> m_baseline; ///< Starting values, used by Reset.
        int m_selectedWeapon = 0;            ///< Index of the currently selected weapon.
        bool m_showComparison = false;       ///< Whether the comparison table is visible.
        bool m_showDPSChart = true;          ///< Whether the DPS chart is visible.
        float m_previewTime = 0.0f;          ///< Accumulated time for weapon preview animation.
    };

} // namespace SparkEditor
