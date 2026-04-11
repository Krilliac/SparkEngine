/**
 * @file TestConfigParserReal.cpp
 * @brief Real-class tests for Spark::ConfigParser
 */

#include "TestFramework.h"
#include "Utils/ConfigParser.h"

TEST(ConfigParserReal_DefaultConstruction)
{
    Spark::ConfigParser cfg;
    // Default-constructed parser returns defaults for any query.
    EXPECT_EQ(cfg.GetInt("Missing", "key", 42), 42);
    EXPECT_NEAR(cfg.GetFloat("Missing", "key", 3.14f), 3.14f, 0.001f);
    EXPECT_TRUE(cfg.GetBool("Missing", "key", true));
    EXPECT_EQ(cfg.GetString("Missing", "key", "default"), std::string("default"));
}

TEST(ConfigParserReal_LoadFromStringSimple)
{
    Spark::ConfigParser cfg;
    const std::string content = "[Graphics]\n"
                                "resolution_x = 1920\n"
                                "fullscreen = true\n"
                                "brightness = 0.75\n"
                                "name = SparkEngine\n";
    EXPECT_TRUE(cfg.LoadFromString(content));
    EXPECT_EQ(cfg.GetInt("Graphics", "resolution_x"), 1920);
    EXPECT_TRUE(cfg.GetBool("Graphics", "fullscreen"));
    EXPECT_NEAR(cfg.GetFloat("Graphics", "brightness"), 0.75f, 0.001f);
    EXPECT_EQ(cfg.GetString("Graphics", "name"), std::string("SparkEngine"));
}

TEST(ConfigParserReal_LoadFromStringIgnoresComments)
{
    Spark::ConfigParser cfg;
    const std::string content = "# This is a comment\n"
                                "[Section]\n"
                                "; Also a comment\n"
                                "key = value\n";
    EXPECT_TRUE(cfg.LoadFromString(content));
    EXPECT_EQ(cfg.GetString("Section", "key"), std::string("value"));
}

TEST(ConfigParserReal_SetAndGetRoundTrip)
{
    Spark::ConfigParser cfg;
    cfg.SetInt("Graphics", "resolution_x", 2560);
    cfg.SetString("Audio", "device", "Default");
    cfg.SetBool("UI", "show_fps", true);

    EXPECT_EQ(cfg.GetInt("Graphics", "resolution_x"), 2560);
    EXPECT_EQ(cfg.GetString("Audio", "device"), std::string("Default"));
    EXPECT_TRUE(cfg.GetBool("UI", "show_fps"));
}

TEST(ConfigParserReal_SaveToStringRoundTrip)
{
    Spark::ConfigParser cfg;
    cfg.SetInt("Graphics", "width", 1280);
    cfg.SetInt("Graphics", "height", 720);

    const auto saved = cfg.SaveToString();
    EXPECT_TRUE(!saved.empty());

    Spark::ConfigParser cfg2;
    EXPECT_TRUE(cfg2.LoadFromString(saved));
    EXPECT_EQ(cfg2.GetInt("Graphics", "width"), 1280);
    EXPECT_EQ(cfg2.GetInt("Graphics", "height"), 720);
}

TEST(ConfigParserReal_DefaultValueOnMissing)
{
    Spark::ConfigParser cfg;
    cfg.LoadFromString("[S]\nk = 10\n");
    EXPECT_EQ(cfg.GetInt("S", "missing_key", 99), 99);
    EXPECT_EQ(cfg.GetInt("OtherSection", "k", 42), 42);
}

TEST(ConfigParserReal_BoolVariants)
{
    Spark::ConfigParser cfg;
    cfg.LoadFromString("[S]\n"
                       "a = true\n"
                       "b = false\n"
                       "c = 1\n"
                       "d = 0\n");
    EXPECT_TRUE(cfg.GetBool("S", "a"));
    EXPECT_FALSE(cfg.GetBool("S", "b"));
    EXPECT_TRUE(cfg.GetBool("S", "c"));
    EXPECT_FALSE(cfg.GetBool("S", "d"));
}
