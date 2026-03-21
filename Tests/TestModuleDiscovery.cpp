// TestModuleDiscovery.cpp - Tests for DiscoveredModule and multi-game module detection
// Standalone implementations for CI testing (no DLL dependency)

#include "TestFramework.h"
#include <string>
#include <vector>

namespace TestModuleDiscovery
{

    /// Mirror of DiscoveredModule from ModuleManager.h for standalone testing
    struct DiscoveredModule
    {
        std::string name;
        std::string path;
        std::string version;
        bool isLoaded = false;
    };

    /// Simulates manifest generation from selected modules
    std::string GenerateManifest(const std::vector<DiscoveredModule>& modules)
    {
        std::string json = "{\n    \"modules\": [\n";
        bool first = true;
        int loadOrder = 1000;

        for (const auto& mod : modules)
        {
            if (!first)
                json += ",\n";
            first = false;

            // Extract filename from path
            std::string filename = mod.path;
            auto pos = filename.find_last_of("/\\");
            if (pos != std::string::npos)
                filename = filename.substr(pos + 1);

            json += "        {\n";
            json += "            \"name\": \"" + mod.name + "\",\n";
            json += "            \"path\": \"" + filename + "\",\n";
            json += "            \"loadOrder\": " + std::to_string(loadOrder) + "\n";
            json += "        }";
            loadOrder++;
        }

        json += "\n    ]\n}\n";
        return json;
    }

    /// Simulates filtering discovered modules by loaded state
    std::vector<DiscoveredModule> FilterLoaded(const std::vector<DiscoveredModule>& modules)
    {
        std::vector<DiscoveredModule> loaded;
        for (const auto& mod : modules)
        {
            if (mod.isLoaded)
                loaded.push_back(mod);
        }
        return loaded;
    }

} // namespace TestModuleDiscovery

// =============================================================================
// Tests
// =============================================================================

TEST(ModuleDiscovery_DiscoveredModuleDefaultState)
{
    TestModuleDiscovery::DiscoveredModule mod;
    EXPECT_TRUE(mod.name.empty());
    EXPECT_TRUE(mod.path.empty());
    EXPECT_EQ(mod.isLoaded, false);
}

TEST(ModuleDiscovery_MultiGameDetection)
{
    // Simulate discovering two game modules
    std::vector<TestModuleDiscovery::DiscoveredModule> discovered = {
        {"Spark Arena - Engine Showcase", "/build/bin/SparkGame.so", "2.0.0", true},
        {"Spark MMO - Networking Showcase", "/build/bin/SparkGameMMO.so", "1.0.0", false},
    };

    EXPECT_EQ(discovered.size(), 2u);
    EXPECT_TRUE(discovered[0].isLoaded);
    EXPECT_FALSE(discovered[1].isLoaded);
}

TEST(ModuleDiscovery_FilterLoadedModules)
{
    std::vector<TestModuleDiscovery::DiscoveredModule> all = {
        {"GameA", "GameA.dll", "1.0", true},
        {"GameB", "GameB.dll", "1.0", false},
        {"GameC", "GameC.dll", "2.0", true},
    };

    auto loaded = TestModuleDiscovery::FilterLoaded(all);
    EXPECT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].name, "GameA");
    EXPECT_EQ(loaded[1].name, "GameC");
}

TEST(ModuleDiscovery_ManifestGenerationSingleModule)
{
    std::vector<TestModuleDiscovery::DiscoveredModule> modules = {
        {"SparkGame", "/build/bin/SparkGame.dll", "2.0.0", true},
    };

    std::string manifest = TestModuleDiscovery::GenerateManifest(modules);
    EXPECT_TRUE(manifest.find("\"name\": \"SparkGame\"") != std::string::npos);
    EXPECT_TRUE(manifest.find("\"path\": \"SparkGame.dll\"") != std::string::npos);
    EXPECT_TRUE(manifest.find("\"loadOrder\": 1000") != std::string::npos);
}

TEST(ModuleDiscovery_ManifestGenerationMultipleModules)
{
    std::vector<TestModuleDiscovery::DiscoveredModule> modules = {
        {"SparkGame", "/build/bin/SparkGame.dll", "2.0.0", true},
        {"SparkGameMMO", "/build/bin/SparkGameMMO.dll", "1.0.0", true},
    };

    std::string manifest = TestModuleDiscovery::GenerateManifest(modules);
    EXPECT_TRUE(manifest.find("\"name\": \"SparkGame\"") != std::string::npos);
    EXPECT_TRUE(manifest.find("\"name\": \"SparkGameMMO\"") != std::string::npos);
    EXPECT_TRUE(manifest.find("\"loadOrder\": 1000") != std::string::npos);
    EXPECT_TRUE(manifest.find("\"loadOrder\": 1001") != std::string::npos);
}

TEST(ModuleDiscovery_ManifestGenerationEmpty)
{
    std::vector<TestModuleDiscovery::DiscoveredModule> modules;
    std::string manifest = TestModuleDiscovery::GenerateManifest(modules);
    EXPECT_TRUE(manifest.find("\"modules\": [") != std::string::npos);
    // Should not contain any module entries
    EXPECT_TRUE(manifest.find("\"name\"") == std::string::npos);
}
