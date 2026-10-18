/**
 * @file TestMOD300ShowcaseLocalizationReal.cpp
 * @brief MOD-300: the SparkGame showcase string tables load into the engine LocalizationSystem
 *        and refuse an incomplete table.
 *
 * Links GameModules/SparkGame/Source/Core/ShowcaseLocalization.cpp directly and loads the
 * repository's Assets/Localization/SparkGame tables into the engine LocalizationSystem
 * (its constructor is private, so the process instance is used and its language restored).
 */

#include "TestFramework.h"

#include "../GameModules/SparkGame/Source/Core/ShowcaseLocalization.h"
#include "Engine/Localization/LocalizationSystem.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    /// Restores the LocalizationSystem's current language when the test ends.
    struct ScopedLanguage
    {
        Spark::LocalizationSystem& localization = Spark::LocalizationSystem::Get();
        std::string previous = localization.GetCurrentLanguage();
        ~ScopedLanguage() { localization.SetCurrentLanguage(previous); }
    };

    /// Copies the shipped tables into a private content root, optionally rewriting one of them.
    class ScopedShowcaseRoot
    {
      public:
        explicit ScopedShowcaseRoot(const char* name)
            : m_root(fs::temp_directory_path() / (std::string("spark_mod300_loc_") + name))
        {
            std::error_code error;
            fs::remove_all(m_root, error);
            for (const auto& language : ShowcaseLocalization::kShowcaseLanguageFiles)
            {
                const fs::path target = m_root / language[1];
                fs::create_directories(target.parent_path(), error);
                fs::copy_file(fs::path(SPARK_TEST_SOURCE_DIR) / language[1], target,
                              fs::copy_options::overwrite_existing, error);
            }
        }
        ~ScopedShowcaseRoot()
        {
            std::error_code error;
            fs::remove_all(m_root, error);
        }
        ScopedShowcaseRoot(const ScopedShowcaseRoot&) = delete;
        ScopedShowcaseRoot& operator=(const ScopedShowcaseRoot&) = delete;

        const fs::path& Path() const { return m_root; }

        /// Replaces @p from with @p to in one table; returns false if @p from is absent.
        bool Rewrite(const char* relative, const std::string& from, const std::string& to) const
        {
            const fs::path path = m_root / relative;
            std::ifstream input(path, std::ios::binary);
            std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            input.close();
            const auto at = text.find(from);
            if (at == std::string::npos)
                return false;
            text.replace(at, from.size(), to);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << text;
            return output.good();
        }

      private:
        fs::path m_root;
    };
} // namespace

TEST(SparkGameShowcase_LocalizationLoadsEnAndFrTables)
{
    ScopedLanguage language;
    auto& localization = language.localization;
    std::string error;
    ASSERT_TRUE(ShowcaseLocalization::LoadShowcaseStrings(localization, SPARK_TEST_SOURCE_DIR, &error));
    EXPECT_TRUE(error.empty());

    // English: every key resolves, and the table matches the built-in fallback text.
    ASSERT_TRUE(localization.SetCurrentLanguage("en"));
    for (const auto& entry : ShowcaseLocalization::kShowcaseStrings)
    {
        EXPECT_EQ(localization.GetString(entry.key), std::string(entry.english));
        EXPECT_EQ(ShowcaseLocalization::ShowcaseText(&localization, entry.key), std::string(entry.english));
    }

    // French: every key resolves to its own translation.
    ASSERT_TRUE(localization.SetCurrentLanguage("fr"));
    for (const auto& entry : ShowcaseLocalization::kShowcaseStrings)
    {
        const std::string french = ShowcaseLocalization::ShowcaseText(&localization, entry.key);
        EXPECT_NE(french, std::string(entry.key));
        EXPECT_NE(french, std::string(entry.english));
    }
    EXPECT_EQ(ShowcaseLocalization::ShowcaseText(&localization, "showcase.status.coroutine"),
              std::string("S\xC3\xA9quence de coroutine"));

    // Without a host localization system the labels fall back to English.
    EXPECT_EQ(ShowcaseLocalization::ShowcaseText(nullptr, "showcase.status.coroutine"),
              std::string("Coroutine sequence"));
}

TEST(SparkGameShowcase_LocalizationFailsClosedOnMissingKey)
{
    ScopedLanguage language;
    auto& localization = language.localization;
    ASSERT_TRUE(ShowcaseLocalization::LoadShowcaseStrings(localization, SPARK_TEST_SOURCE_DIR));
    ASSERT_TRUE(localization.SetCurrentLanguage("en"));

    // A changed English value plus a French table missing one key: nothing may be loaded.
    ScopedShowcaseRoot root("missing_key");
    ASSERT_TRUE(
        root.Rewrite("Assets/Localization/SparkGame/showcase_en.json", "\"Kill events\"", "\"Changed kill events\""));
    ASSERT_TRUE(root.Rewrite("Assets/Localization/SparkGame/showcase_fr.json", "\"showcase.status.coroutine\"",
                             "\"showcase.status.removed\""));

    std::string error;
    EXPECT_FALSE(ShowcaseLocalization::LoadShowcaseStrings(localization, root.Path(), &error));
    EXPECT_STR_CONTAINS(error, "showcase_fr.json is missing key showcase.status.coroutine");
    EXPECT_EQ(localization.GetString("showcase.status.kill_events"), std::string("Kill events"));

    // A missing file fails the same way.
    fs::remove(root.Path() / "Assets/Localization/SparkGame/showcase_en.json");
    EXPECT_FALSE(ShowcaseLocalization::LoadShowcaseStrings(localization, root.Path(), &error));
    EXPECT_STR_CONTAINS(error, "cannot load showcase strings from Assets/Localization/SparkGame/showcase_en.json");
    EXPECT_EQ(localization.GetString("showcase.status.kill_events"), std::string("Kill events"));
}
