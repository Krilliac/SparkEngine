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
     * localization keys and translations. Includes language coverage metrics,
     * missing string detection, and CSV import/export.
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
            std::string context;                                       // optional context/comment for translators
            std::unordered_map<std::string, std::string> translations; // lang -> text
        };

        void RenderToolbar();
        void RenderLanguageSelector();
        void RenderCoverageMetrics();
        void RenderStringTable();
        void RenderAddEntryForm();

        std::vector<LocalizedString> m_strings;
        std::vector<std::string> m_languages = {"en", "es", "fr", "de", "ja", "zh"};
        int m_currentLanguage = 0;
        char m_filterText[128] = {};
        char m_newKey[128] = {};
        char m_newValue[512] = {};
        char m_newContext[256] = {};

        bool m_showMissingOnly = false;
        int m_charLengthWarning = 100; // warn if text exceeds this length
    };

} // namespace SparkEditor
