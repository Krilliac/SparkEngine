// TestThirdPartyIntegration.cpp - Tests for third-party library integrations
// Validates stb_image, cgltf, miniaudio, nlohmann/json, tinyexr availability

#include "TestFramework.h"
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// stb_image integration tests
// ============================================================================

#if SPARK_HAS_STB_IMAGE
#include <stb_image.h>

TEST(StbImage_ApiAvailable)
{
    // Verify stb_image functions are linkable
    stbi_set_flip_vertically_on_load(0);
    const char* reason = stbi_failure_reason();
    // Should return empty string or valid pointer, not crash
    EXPECT_TRUE(reason != nullptr);
}

TEST(StbImage_LoadFromMemory_InvalidData)
{
    // Passing invalid data should return nullptr, not crash
    unsigned char garbage[] = {0x01, 0x02, 0x03, 0x04};
    int w = 0, h = 0, channels = 0;
    stbi_uc* result = stbi_load_from_memory(garbage, sizeof(garbage), &w, &h, &channels, 4);
    EXPECT_TRUE(result == nullptr);
}

TEST(StbImage_LoadFromFile_NonExistent)
{
    int w = 0, h = 0, channels = 0;
    stbi_uc* result = stbi_load("/nonexistent/path/texture.png", &w, &h, &channels, 4);
    EXPECT_TRUE(result == nullptr);
}

TEST(StbImage_IsHdr_NonExistent)
{
    int result = stbi_is_hdr("/nonexistent/path/sky.hdr");
    EXPECT_EQ(result, 0);
}

#endif // SPARK_HAS_STB_IMAGE

// ============================================================================
// cgltf integration tests
// ============================================================================

#if SPARK_HAS_CGLTF
#include <cgltf.h>

TEST(Cgltf_ApiAvailable)
{
    // Verify cgltf functions are linkable
    EXPECT_EQ(cgltf_num_components(cgltf_type_vec3), static_cast<cgltf_size>(3));
    EXPECT_EQ(cgltf_num_components(cgltf_type_vec4), static_cast<cgltf_size>(4));
    EXPECT_EQ(cgltf_num_components(cgltf_type_mat4), static_cast<cgltf_size>(16));
    EXPECT_EQ(cgltf_num_components(cgltf_type_scalar), static_cast<cgltf_size>(1));
}

TEST(Cgltf_ParseFile_NonExistent)
{
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, "/nonexistent/model.gltf", &data);
    EXPECT_TRUE(result != cgltf_result_success);
}

TEST(Cgltf_Free_Null)
{
    EXPECT_NO_THROW(cgltf_free(nullptr));
}

TEST(Cgltf_Parse_InvalidJson)
{
    constexpr char invalidJson[] = "{]";
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse(&options, invalidJson, sizeof(invalidJson) - 1, &data);
    EXPECT_TRUE(result != cgltf_result_success);
    EXPECT_TRUE(data == nullptr);
}

#endif // SPARK_HAS_CGLTF

// ============================================================================
// miniaudio integration tests
// ============================================================================

#if SPARK_HAS_MINIAUDIO
#include <miniaudio.h>

TEST(Miniaudio_ConfigInit)
{
    ma_engine_config config = ma_engine_config_init();
    EXPECT_EQ(config.channels, 2u);
    EXPECT_EQ(config.sampleRate, 44100u);
    EXPECT_EQ(config.listenerCount, 1u);
}

TEST(Miniaudio_EngineInitUninit)
{
    ma_engine engine;
    ma_engine_config config = ma_engine_config_init();
    ma_result result = ma_engine_init(&config, &engine);
    EXPECT_EQ(result, MA_SUCCESS);
    ma_engine_uninit(&engine);
}

TEST(Miniaudio_SoundInitFromFile_NonExistent)
{
    ma_engine engine;
    ma_engine_config config = ma_engine_config_init();
    ma_engine_init(&config, &engine);

    ma_sound sound;
    ma_result result = ma_sound_init_from_file(&engine, "/nonexistent/sound.wav", 0, nullptr, nullptr, &sound);
    // Should succeed even with stub (stub doesn't validate file existence)
    EXPECT_EQ(result, MA_SUCCESS);
    ma_sound_uninit(&sound);

    ma_engine_uninit(&engine);
}

TEST(Miniaudio_SoundControls)
{
    ma_engine engine;
    ma_engine_config config = ma_engine_config_init();
    ma_engine_init(&config, &engine);

    ma_sound sound;
    ma_sound_init_from_file(&engine, "test.wav", 0, nullptr, nullptr, &sound);

    ma_sound_set_volume(&sound, 0.5f);
    ma_sound_set_pitch(&sound, 1.5f);
    ma_sound_set_looping(&sound, MA_TRUE);
    ma_sound_set_position(&sound, 1.0f, 2.0f, 3.0f);
    ma_sound_set_spatialization_enabled(&sound, MA_TRUE);

    EXPECT_FALSE(ma_sound_is_playing(&sound));
    ma_sound_start(&sound);
    EXPECT_TRUE(ma_sound_is_playing(&sound));
    ma_sound_stop(&sound);
    EXPECT_FALSE(ma_sound_is_playing(&sound));

    ma_sound_uninit(&sound);
    ma_engine_uninit(&engine);
}

#endif // SPARK_HAS_MINIAUDIO

// ============================================================================
// nlohmann/json integration tests
// ============================================================================

#if SPARK_HAS_NLOHMANN_JSON
#include <nlohmann_json.h>

TEST(NlohmannJson_ParseObject)
{
    auto j = nlohmann::json::parse(R"({"name": "Spark", "version": 1})");
    EXPECT_TRUE(j.is_object());
    EXPECT_TRUE(j.contains("name"));
    std::string name = j["name"].get<std::string>();
    int version = j["version"].get<int>();
    EXPECT_EQ(name, std::string("Spark"));
    EXPECT_EQ(version, 1);
}

TEST(NlohmannJson_ParseArray)
{
    auto j = nlohmann::json::parse("[1, 2, 3]");
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), static_cast<size_t>(3));
    int first = j[static_cast<size_t>(0)].get<int>();
    int last = j[static_cast<size_t>(2)].get<int>();
    EXPECT_EQ(first, 1);
    EXPECT_EQ(last, 3);
}

TEST(NlohmannJson_BuildAndDump)
{
    nlohmann::json j;
    j["engine"] = nlohmann::json("SparkEngine");
    j["fps"] = nlohmann::json(60);
    j["vsync"] = nlohmann::json(true);

    std::string s = j.dump();
    EXPECT_TRUE(s.find("SparkEngine") != std::string::npos);
    EXPECT_TRUE(s.find("60") != std::string::npos);
}

TEST(NlohmannJson_Nested)
{
    auto j = nlohmann::json::parse(R"({"a": {"b": {"c": 42}}})");
    int val = j["a"]["b"]["c"].get<int>();
    EXPECT_EQ(val, 42);
}

TEST(NlohmannJson_RoundTrip)
{
    nlohmann::json original;
    original["items"] = nlohmann::json::array();
    const_cast<nlohmann::json&>(original["items"]).push_back(nlohmann::json("sword"));
    const_cast<nlohmann::json&>(original["items"]).push_back(nlohmann::json("shield"));
    original["count"] = nlohmann::json(2);

    std::string serialized = original.dump();
    auto parsed = nlohmann::json::parse(serialized);
    EXPECT_EQ(parsed, original);
}

// The vendored stub used to return the partial value read so far, so a
// truncated file parsed as a valid-looking document (see ReflectedScene_*).
TEST(NlohmannJson_MalformedInputThrows)
{
    EXPECT_THROW(nlohmann::json::parse("{\"version\": 1, \"entities\": [\n"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(R"({"a": 1)"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(R"({"a": "unterminated)"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(R"({"a": 1}garbage)"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("{} {}"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("[1, 2,]"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("[1 2]"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(R"({"a" 1})"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("{ this is garbage"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("nul"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("[12.]"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse("[1e]"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(R"("\q")"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(R"("\uZZZZ")"), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(""), std::runtime_error);
    EXPECT_THROW(nlohmann::json::parse(" \n "), std::runtime_error);
}

TEST(NlohmannJson_UnicodeEscapesAndControlCharactersRoundTrip)
{
    // Build each JSON string escape as backslash + "uXXXX" so the test text
    // really contains the escape rather than an already-decoded character.
    const auto quotedEscapes = [](std::initializer_list<const char*> hexUnits)
    {
        std::string text = "\"";
        for (const char* hex : hexUnits)
            text += std::string(1, '\\') + 'u' + hex;
        return text + "\"";
    };
    EXPECT_EQ(nlohmann::json::parse(quotedEscapes({"0041"})).get<std::string>(), std::string("A"));
    EXPECT_EQ(nlohmann::json::parse(quotedEscapes({"00e9"})).get<std::string>(), std::string("\xC3\xA9"));
    EXPECT_EQ(nlohmann::json::parse(quotedEscapes({"d83d", "de00"})).get<std::string>(),
              std::string("\xF0\x9F\x98\x80"));
    EXPECT_THROW(nlohmann::json::parse(quotedEscapes({"de00"})), std::runtime_error); // unpaired low surrogate

    nlohmann::json original;
    original["s"] = nlohmann::json(std::string("a\x01"
                                               "b"));
    const auto parsed = nlohmann::json::parse(original.dump());
    EXPECT_EQ(parsed, original);
    EXPECT_NO_THROW(nlohmann::json::parse("  {\"a\": [1, -2.5e3, true, null]} \r\n"));
}

#endif // SPARK_HAS_NLOHMANN_JSON

// ============================================================================
// tinyexr integration tests
// ============================================================================

#if SPARK_HAS_TINYEXR
#include <tinyexr.h>

TEST(TinyExr_ApiAvailable)
{
    EXRHeader header;
    InitEXRHeader(&header);
    EXPECT_NEAR(header.pixel_aspect_ratio, 0.0f, 0.01f);
    EXPECT_NEAR(header.screen_window_width, 0.0f, 0.01f);
    EXPECT_TRUE(header.channels == nullptr);
    EXPECT_EQ(header.num_channels, 0);
    FreeEXRHeader(&header);
}

TEST(TinyExr_ParseVersionNonExistent)
{
    EXRVersion version;
    int result = ParseEXRVersionFromFile(&version, "/nonexistent/image.exr");
    EXPECT_TRUE(result != TINYEXR_SUCCESS);
}

TEST(TinyExr_LoadExrNonExistent)
{
    float* rgba = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    int result = LoadEXR(&rgba, &w, &h, "/nonexistent/image.exr", &err);
    EXPECT_TRUE(result != TINYEXR_SUCCESS);
    EXPECT_TRUE(rgba == nullptr);
    if (err)
        FreeEXRErrorMessage(err);
}

TEST(TinyExr_FreeNull)
{
    EXPECT_NO_THROW(FreeEXRErrorMessage(nullptr));
}

#endif // SPARK_HAS_TINYEXR

// ============================================================================
// Cross-library integration tests
// ============================================================================

#if SPARK_HAS_NLOHMANN_JSON
TEST(NlohmannJson_SparkJsonInterop)
{
    // Test that nlohmann JSON can parse what the engine produces
    auto j = nlohmann::json::parse(R"({
        "textures": [
            {"path": "grass.png", "sRGB": true},
            {"path": "sky.hdr", "sRGB": false}
        ],
        "meshes": ["cube.obj", "tree.gltf"]
    })");

    EXPECT_EQ(j["textures"].size(), static_cast<size_t>(2));
    std::string texPath = j["textures"][static_cast<size_t>(0)]["path"].get<std::string>();
    std::string meshPath = j["meshes"][static_cast<size_t>(1)].get<std::string>();
    EXPECT_EQ(texPath, std::string("grass.png"));
    EXPECT_EQ(meshPath, std::string("tree.gltf"));
}
#endif
