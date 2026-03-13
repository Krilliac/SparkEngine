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
        void RenderWeaponList();
        void RenderWeaponProperties();
        void RenderWeaponPreview();
        void RenderDPSChart();
        void RenderComparisonTable();
        void SaveAllWeapons();

        struct WeaponEntry
        {
            std::string name;
            int typeId;
            float damage;
            float fireRate;
            int magazineSize;
            float reloadTime;
            float muzzleVelocity;
            float accuracy;
            float range;
            bool isModified;
        };

        std::vector<WeaponEntry> m_weapons;
        int m_selectedWeapon = 0;
        bool m_showComparison = false;
        bool m_showDPSChart = true;
        float m_previewTime = 0.0f;
    };

} // namespace SparkEditor
