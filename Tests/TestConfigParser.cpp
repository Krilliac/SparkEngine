// TestConfigParser.cpp - Tests for INI-style config file parser
// Uses the actual ConfigParser.h header

#include "TestFramework.h"
#include "Utils/ConfigParser.h"
#include <cstdio>

// =============================================================================
// Tests
// =============================================================================

TEST(ConfigParser_ParseBasic) {
    Spark::ConfigParser cfg;
    EXPECT_TRUE(cfg.LoadFromString(
        "[Graphics]\n"
        "width = 1920\n"
        "height = 1080\n"
        "fullscreen = true\n"
    ));

    EXPECT_TRUE(cfg.HasSection("Graphics"));
    EXPECT_EQ(cfg.GetInt("Graphics", "width", 0), 1920);
    EXPECT_EQ(cfg.GetInt("Graphics", "height", 0), 1080);
    EXPECT_TRUE(cfg.GetBool("Graphics", "fullscreen", false));
}

TEST(ConfigParser_Comments) {
    Spark::ConfigParser cfg;
    cfg.LoadFromString(
        "# This is a comment\n"
        "; This is also a comment\n"
        "[Section]\n"
        "key = value\n"
        "# comment in section\n"
        "key2 = value2\n"
    );

    EXPECT_EQ(cfg.GetString("Section", "key"), std::string("value"));
    EXPECT_EQ(cfg.GetString("Section", "key2"), std::string("value2"));
}

TEST(ConfigParser_DefaultValues) {
    Spark::ConfigParser cfg;

    EXPECT_EQ(cfg.GetString("missing", "key", "default"), std::string("default"));
    EXPECT_EQ(cfg.GetInt("missing", "key", 42), 42);
    EXPECT_NEAR(cfg.GetFloat("missing", "key", 3.14f), 3.14f, 0.01f);
    EXPECT_FALSE(cfg.GetBool("missing", "key", false));
}

TEST(ConfigParser_MultipleSections) {
    Spark::ConfigParser cfg;
    cfg.LoadFromString(
        "[Audio]\n"
        "volume = 0.8\n"
        "\n"
        "[Video]\n"
        "vsync = on\n"
    );

    EXPECT_TRUE(cfg.HasSection("Audio"));
    EXPECT_TRUE(cfg.HasSection("Video"));
    EXPECT_NEAR(cfg.GetFloat("Audio", "volume", 0.0f), 0.8f, 0.01f);
    EXPECT_TRUE(cfg.GetBool("Video", "vsync", false));
}

TEST(ConfigParser_SetValues) {
    Spark::ConfigParser cfg;
    cfg.SetString("Game", "name", "SparkEngine");
    cfg.SetInt("Game", "maxFps", 60);
    cfg.SetFloat("Game", "gravity", 9.81f);
    cfg.SetBool("Game", "debug", true);

    EXPECT_EQ(cfg.GetString("Game", "name"), std::string("SparkEngine"));
    EXPECT_EQ(cfg.GetInt("Game", "maxFps", 0), 60);
    EXPECT_NEAR(cfg.GetFloat("Game", "gravity", 0.0f), 9.81f, 0.01f);
    EXPECT_TRUE(cfg.GetBool("Game", "debug", false));
}

TEST(ConfigParser_HasKeySection) {
    Spark::ConfigParser cfg;
    cfg.SetString("Section", "key", "value");

    EXPECT_TRUE(cfg.HasSection("Section"));
    EXPECT_TRUE(cfg.HasKey("Section", "key"));
    EXPECT_FALSE(cfg.HasKey("Section", "missing"));
    EXPECT_FALSE(cfg.HasSection("Missing"));
}

TEST(ConfigParser_GetSections) {
    Spark::ConfigParser cfg;
    cfg.SetString("A", "k", "v");
    cfg.SetString("B", "k", "v");
    cfg.SetString("C", "k", "v");

    auto sections = cfg.GetSections();
    EXPECT_EQ((int)sections.size(), 3);
}

TEST(ConfigParser_GetKeys) {
    Spark::ConfigParser cfg;
    cfg.SetString("Section", "a", "1");
    cfg.SetString("Section", "b", "2");
    cfg.SetString("Section", "c", "3");

    auto keys = cfg.GetKeys("Section");
    EXPECT_EQ((int)keys.size(), 3);
}

TEST(ConfigParser_RemoveKey) {
    Spark::ConfigParser cfg;
    cfg.SetString("Section", "key", "value");
    EXPECT_TRUE(cfg.HasKey("Section", "key"));

    EXPECT_TRUE(cfg.RemoveKey("Section", "key"));
    EXPECT_FALSE(cfg.HasKey("Section", "key"));
    EXPECT_FALSE(cfg.RemoveKey("Section", "key")); // Already removed
}

TEST(ConfigParser_RemoveSection) {
    Spark::ConfigParser cfg;
    cfg.SetString("Section", "key", "value");
    EXPECT_TRUE(cfg.HasSection("Section"));

    EXPECT_TRUE(cfg.RemoveSection("Section"));
    EXPECT_FALSE(cfg.HasSection("Section"));
}

TEST(ConfigParser_Clear) {
    Spark::ConfigParser cfg;
    cfg.SetString("A", "k", "v");
    cfg.SetString("B", "k", "v");
    cfg.Clear();
    EXPECT_FALSE(cfg.HasSection("A"));
    EXPECT_FALSE(cfg.HasSection("B"));
}

TEST(ConfigParser_SaveAndLoad) {
    std::string path = "/tmp/spark_test_config.ini";

    Spark::ConfigParser cfg;
    cfg.SetString("Graphics", "resolution", "1920x1080");
    cfg.SetInt("Graphics", "fps", 60);
    cfg.SetBool("Audio", "muted", false);
    cfg.SetFloat("Audio", "volume", 0.75f);

    EXPECT_TRUE(cfg.Save(path));

    Spark::ConfigParser loaded;
    EXPECT_TRUE(loaded.Load(path));

    EXPECT_EQ(loaded.GetString("Graphics", "resolution"), std::string("1920x1080"));
    EXPECT_EQ(loaded.GetInt("Graphics", "fps", 0), 60);
    EXPECT_FALSE(loaded.GetBool("Audio", "muted", true));
    EXPECT_NEAR(loaded.GetFloat("Audio", "volume", 0.0f), 0.75f, 0.01f);

    std::remove(path.c_str());
}

TEST(ConfigParser_SaveToString) {
    Spark::ConfigParser cfg;
    cfg.SetString("Section", "key", "value");

    std::string output = cfg.SaveToString();
    EXPECT_TRUE(output.find("[Section]") != std::string::npos);
    EXPECT_TRUE(output.find("key = value") != std::string::npos);
}

TEST(ConfigParser_BoolVariants) {
    Spark::ConfigParser cfg;
    cfg.LoadFromString(
        "[Booleans]\n"
        "a = true\n"
        "b = yes\n"
        "c = on\n"
        "d = 1\n"
        "e = false\n"
        "f = no\n"
        "g = off\n"
        "h = 0\n"
    );

    EXPECT_TRUE(cfg.GetBool("Booleans", "a", false));
    EXPECT_TRUE(cfg.GetBool("Booleans", "b", false));
    EXPECT_TRUE(cfg.GetBool("Booleans", "c", false));
    EXPECT_TRUE(cfg.GetBool("Booleans", "d", false));
    EXPECT_FALSE(cfg.GetBool("Booleans", "e", true));
    EXPECT_FALSE(cfg.GetBool("Booleans", "f", true));
    EXPECT_FALSE(cfg.GetBool("Booleans", "g", true));
    EXPECT_FALSE(cfg.GetBool("Booleans", "h", true));
}

TEST(ConfigParser_WhitespaceHandling) {
    Spark::ConfigParser cfg;
    cfg.LoadFromString(
        "  [  Section  ]  \n"
        "  key  =  value  \n"
    );

    EXPECT_TRUE(cfg.HasSection("Section"));
    EXPECT_EQ(cfg.GetString("Section", "key"), std::string("value"));
}

TEST(ConfigParser_GlobalKeys) {
    Spark::ConfigParser cfg;
    cfg.LoadFromString(
        "global_key = global_value\n"
        "[Section]\n"
        "key = value\n"
    );

    EXPECT_EQ(cfg.GetString("", "global_key"), std::string("global_value"));
    EXPECT_EQ(cfg.GetString("Section", "key"), std::string("value"));
}
