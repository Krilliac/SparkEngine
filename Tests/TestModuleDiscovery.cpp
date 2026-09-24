// TestModuleDiscovery.cpp - Module discovery against the production ModuleManager
//
// RDY-010: these tests previously asserted on a test-local DiscoveredModule copy
// and hand-written manifest/filter helpers, so they could not detect a
// regression in shipped discovery. Every assertion below now drives the real
// ModuleManager discovery, load, and manifest entry points, and the
// loaded-module cases map the real SparkCompatibleModuleFixture shared library.

#include "TestFramework.h"

#include "Core/ModuleManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

namespace
{
#ifdef _WIN32
    constexpr std::string_view kModuleExtension = ".dll";
#elif defined(__APPLE__)
    constexpr std::string_view kModuleExtension = ".dylib";
#else
    constexpr std::string_view kModuleExtension = ".so";
#endif

    constexpr std::string_view kFixtureModuleName = "Spark Compatible ABI Fixture";
    constexpr std::string_view kFixtureModuleVersion = "1.0.0";

    std::filesystem::path PathFromUtf8(std::string_view path)
    {
        return std::filesystem::u8path(path.begin(), path.end());
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::filesystem::path SidecarPath(const std::filesystem::path& modulePath)
    {
        std::filesystem::path sidecar = modulePath;
        sidecar += ".sparkabi";
        return sidecar;
    }

    std::string Filename(const std::string& path)
    {
        return PathToUtf8(PathFromUtf8(path).filename());
    }

    /// Fresh, empty scratch directory removed on scope exit (including ASSERT aborts).
    class ScratchDirectory
    {
      public:
        explicit ScratchDirectory(std::string_view leaf)
            : m_path(std::filesystem::temp_directory_path() / PathFromUtf8(leaf))
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
            std::filesystem::create_directories(m_path);
        }

        ~ScratchDirectory()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }

        ScratchDirectory(const ScratchDirectory&) = delete;
        ScratchDirectory& operator=(const ScratchDirectory&) = delete;

        const std::filesystem::path& Path() const { return m_path; }
        std::string Utf8() const { return PathToUtf8(m_path); }

        /// Write an unmappable placeholder image; discovery must never execute it.
        std::filesystem::path WritePlaceholder(std::string_view filename, bool withSidecar) const
        {
            const std::filesystem::path file = m_path / PathFromUtf8(filename);
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << "not a loadable image";
            }
            if (withSidecar)
            {
                std::ofstream sidecar(SidecarPath(file), std::ios::binary | std::ios::trunc);
                sidecar << "{}";
            }
            return file;
        }

        /// Copy the real compatible ABI fixture (image + generated sidecar) under a new stem.
        std::filesystem::path CopyCompatibleFixture(std::string_view stem) const
        {
            const std::filesystem::path source = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
            std::filesystem::path destination = m_path / PathFromUtf8(stem);
            destination += source.extension();
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(SidecarPath(source), SidecarPath(destination),
                                       std::filesystem::copy_options::overwrite_existing);
            return destination;
        }

      private:
        std::filesystem::path m_path;
    };

    std::string ModuleFile(std::string_view stem)
    {
        std::string name(stem);
        name += kModuleExtension;
        return name;
    }

    std::vector<std::string> Filenames(const std::vector<std::string>& paths)
    {
        std::vector<std::string> names;
        names.reserve(paths.size());
        for (const auto& path : paths)
            names.push_back(Filename(path));
        return names;
    }
} // namespace

// =============================================================================
// Tests
// =============================================================================

TEST(ModuleDiscovery_DiscoveredModuleDefaultState)
{
    // The production struct consumed by the editor's GameModuleSelectorPanel.
    DiscoveredModule mod;
    EXPECT_TRUE(mod.name.empty());
    EXPECT_TRUE(mod.path.empty());
    EXPECT_TRUE(mod.version.empty());
    EXPECT_FALSE(mod.isLoaded);
    EXPECT_FALSE(mod.kindKnown);
    EXPECT_TRUE(mod.kind == Spark::ModuleKind::Game);
}

TEST(ModuleDiscovery_MissingDirectoryYieldsNoCandidates)
{
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "spark-module-discovery-missing";
    std::error_code ec;
    std::filesystem::remove_all(missing, ec);

    ModuleManager manager;
    EXPECT_TRUE(manager.DiscoverModules(PathToUtf8(missing)).empty());
    EXPECT_TRUE(ModuleManager::DiscoverModuleCandidates(PathToUtf8(missing)).empty());
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
}

TEST(ModuleDiscovery_MultiGameDetection)
{
    // Two Game-named images with sidecars are both reported, sorted by filename,
    // while images without a sidecar, without a module name hint, with a
    // system/engine name, or with a foreign extension are filtered out.
    const ScratchDirectory dir("spark-module-discovery-multigame");
    dir.WritePlaceholder(ModuleFile("SparkGameMMO"), true);
    dir.WritePlaceholder(ModuleFile("SparkGame"), true);
    dir.WritePlaceholder(ModuleFile("SparkGameNoSidecar"), false);
    dir.WritePlaceholder(ModuleFile("Unrelated"), true);
    dir.WritePlaceholder(ModuleFile("SparkEngineGameCore"), true);
    dir.WritePlaceholder(ModuleFile("vcruntimeGame"), true);
    dir.WritePlaceholder("SparkGameNotes.txt", true);

    const auto candidates = Filenames(ModuleManager::DiscoverModuleCandidates(dir.Utf8()));
    ASSERT_EQ(candidates.size(), size_t{2});
    EXPECT_EQ(candidates[0], ModuleFile("SparkGame"));
    EXPECT_EQ(candidates[1], ModuleFile("SparkGameMMO"));

    ModuleManager manager;
    const auto discovered = manager.DiscoverModules(dir.Utf8());
    ASSERT_EQ(discovered.size(), size_t{2});
    EXPECT_EQ(discovered[0].name, std::string("SparkGame"));
    EXPECT_EQ(discovered[1].name, std::string("SparkGameMMO"));
    for (const auto& module : discovered)
    {
        // Unloaded candidates are never mapped: only filename metadata exists.
        EXPECT_EQ(module.version, std::string("unknown"));
        EXPECT_FALSE(module.isLoaded);
        EXPECT_FALSE(module.kindKnown);
    }
}

TEST(ModuleDiscovery_FilterLoadedModules)
{
    // A really-loaded module is reported as loaded with its ModuleInfo; an
    // unloaded sibling candidate in the same directory is not.
    const ScratchDirectory dir("spark-module-discovery-filter");
    const std::filesystem::path loadedImage = dir.CopyCompatibleFixture("SparkDiscoveryLoadedModule");
    dir.WritePlaceholder(ModuleFile("SparkDiscoveryZetaGame"), true);

    {
        ModuleManager manager;
        ASSERT_TRUE(manager.LoadModule(PathToUtf8(loadedImage)));

        const auto discovered = manager.DiscoverModules(dir.Utf8());
        ASSERT_EQ(discovered.size(), size_t{2});
        EXPECT_EQ(Filename(discovered[0].path), PathToUtf8(loadedImage.filename()));
        EXPECT_TRUE(discovered[0].isLoaded);
        EXPECT_EQ(discovered[0].name, std::string(kFixtureModuleName));
        EXPECT_EQ(discovered[0].version, std::string(kFixtureModuleVersion));
        EXPECT_TRUE(discovered[0].kindKnown);
        EXPECT_TRUE(discovered[0].kind == Spark::ModuleKind::Addon);

        EXPECT_EQ(Filename(discovered[1].path), ModuleFile("SparkDiscoveryZetaGame"));
        EXPECT_FALSE(discovered[1].isLoaded);
        EXPECT_FALSE(discovered[1].kindKnown);

        std::vector<DiscoveredModule> loadedOnly;
        std::copy_if(discovered.begin(), discovered.end(), std::back_inserter(loadedOnly),
                     [](const DiscoveredModule& module) { return module.isLoaded; });
        ASSERT_EQ(loadedOnly.size(), size_t{1});

        const auto loadedInfo = manager.GetLoadedModuleInfo();
        ASSERT_EQ(loadedInfo.size(), size_t{1});
        EXPECT_EQ(loadedInfo[0].name, loadedOnly[0].name);
        EXPECT_EQ(loadedInfo[0].version, loadedOnly[0].version);
        EXPECT_TRUE(loadedInfo[0].isLoaded);
        EXPECT_TRUE(loadedInfo[0].kind == loadedOnly[0].kind);

        manager.UnloadAll();
        EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    }
}

TEST(ModuleDiscovery_ManifestGenerationSingleModule)
{
    // The selector writes {name, path relative to the manifest, loadOrder}. The
    // production manifest loader must resolve that entry to the discovered image.
    const ScratchDirectory dir("spark-module-discovery-manifest-single");
    const std::filesystem::path image = dir.CopyCompatibleFixture("SparkDiscoveryManifestModule");

    ModuleManager probe;
    const auto discovered = probe.DiscoverModules(dir.Utf8());
    ASSERT_EQ(discovered.size(), size_t{1});
    ASSERT_FALSE(discovered[0].isLoaded);

    const std::filesystem::path manifestPath = dir.Path() / "spark.modules.json";
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        manifest << "{\n    \"modules\": [\n        {\n            \"name\": \"" << discovered[0].name
                 << "\",\n            \"path\": \"" << Filename(discovered[0].path)
                 << "\",\n            \"loadOrder\": 1000\n        }\n    ]\n}\n";
    }

    {
        ModuleManager manager;
        ASSERT_TRUE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
        EXPECT_TRUE(manager.GetLastLoadError().empty());

        const auto loaded = manager.GetLoadedModuleInfo();
        ASSERT_EQ(loaded.size(), size_t{1});
        EXPECT_EQ(loaded[0].name, std::string(kFixtureModuleName));
        EXPECT_EQ(Filename(loaded[0].path), PathToUtf8(image.filename()));

        const auto rediscovered = manager.DiscoverModules(dir.Utf8());
        ASSERT_EQ(rediscovered.size(), size_t{1});
        EXPECT_TRUE(rediscovered[0].isLoaded);
        EXPECT_EQ(rediscovered[0].version, std::string(kFixtureModuleVersion));

        manager.UnloadAll();
    }
}

TEST(ModuleDiscovery_ManifestGenerationMultipleModules)
{
    // A multi-entry selection whose second image is absent still loads the real
    // first image, and only that image is reported as loaded.
    const ScratchDirectory dir("spark-module-discovery-manifest-multi");
    const std::filesystem::path image = dir.CopyCompatibleFixture("SparkDiscoveryManifestFirst");

    const std::filesystem::path manifestPath = dir.Path() / "spark.modules.json";
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        manifest << "{\n    \"modules\": [\n"
                 << "        { \"name\": \"First\", \"path\": \"" << PathToUtf8(image.filename())
                 << "\", \"loadOrder\": 1000 },\n"
                 << "        { \"name\": \"Missing\", \"path\": \"" << ModuleFile("SparkDiscoveryManifestMissingGame")
                 << "\", \"loadOrder\": 1001 }\n"
                 << "    ]\n}\n";
    }

    {
        ModuleManager manager;
        EXPECT_TRUE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
        const auto loaded = manager.GetLoadedModuleInfo();
        ASSERT_EQ(loaded.size(), size_t{1});
        EXPECT_EQ(Filename(loaded[0].path), PathToUtf8(image.filename()));
        manager.UnloadAll();
    }
}

TEST(ModuleDiscovery_ManifestGenerationEmpty)
{
    // Saving a selection with nothing checked produces an empty modules array;
    // the production loader must refuse it rather than report a loaded game.
    const ScratchDirectory dir("spark-module-discovery-manifest-empty");
    const std::filesystem::path manifestPath = dir.Path() / "spark.modules.json";
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        manifest << "{\n    \"modules\": [\n\n    ]\n}\n";
    }

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "non-empty modules array");
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    EXPECT_FALSE(manager.HasModules());
}
