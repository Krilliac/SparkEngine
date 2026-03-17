/**
 * @file LocalizationPanel.h
 * @brief Localization editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace SparkEditor
{

    /**
     * @brief Panel for editing localization string tables
     *
     * Provides a table view for browsing, searching, editing, and adding
     * localization keys and their translations across multiple languages.
     */
    class LocalizationPanel : public EditorPanel
    {
      public:
        LocalizationPanel();
        ~LocalizationPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct LocalizedString
        {
            std::string key;
            std::unordered_map<std::string, std::string> translations; // lang -> text
        };

        void RenderLanguageSelector();
        void RenderStringTable();
        void RenderAddEntryForm();

        std::vector<LocalizedString> m_strings;
        std::vector<std::string> m_languages = {"en", "es", "fr", "de", "ja", "zh"};
        int m_currentLanguage = 0;
        char m_filterText[128] = {};
        char m_newKey[128] = {};
        char m_newValue[512] = {};
    };

} // namespace SparkEditor
