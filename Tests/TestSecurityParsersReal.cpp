/**
 * @file TestSecurityParsersReal.cpp
 * @brief Hostile-input regressions for the untrusted-input parsers on the
 *        stable-v1 path, run against production source.
 *
 * Every test here states the crafted input and would fail (or, for the nesting
 * cases, crash the process on a stack overflow) without the corresponding
 * hardening change. No mirrors: each test includes the shipped header and drives
 * the shipped class.
 */

#include "TestFramework.h"

#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/PacketValidator.h"
#include "Engine/Streaming/SceneManifest.h"
#include "Utils/JsonUtils.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    /// Build a NetBuffer-shaped string field: uint16 little-endian length, then bytes.
    void AppendStringField(std::vector<uint8_t>& payload, const std::string& text)
    {
        const uint16_t length = static_cast<uint16_t>(text.size());
        payload.push_back(static_cast<uint8_t>(length & 0xFF));
        payload.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        payload.insert(payload.end(), text.begin(), text.end());
    }

    std::filesystem::path ScratchPath(const std::string& name)
    {
        auto directory = std::filesystem::temp_directory_path() / "spark_security_parsers_tests";
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        return directory / name;
    }

    template <typename T> void AppendRaw(std::vector<uint8_t>& out, const T& value)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }
} // namespace

// ============================================================================
// Json: nesting depth, input budget (security-parsers-01, -20)
// ============================================================================

TEST(SecurityParsers_JsonDeeplyNestedArrayRejectedNotRecursed)
{
    // 200,000 '[' characters. Before the depth bound this recursed once per
    // bracket through ParseValue/ParseArray and overflowed the 1 MB thread stack;
    // a stack overflow is not a catchable exception, so the process died.
    const std::string hostile(200000, '[');

    Spark::Json::Value value;
    std::string error;
    EXPECT_FALSE(Spark::Json::ParseStrict(hostile, &value, &error));
    EXPECT_TRUE(value.IsNull());
    EXPECT_STR_CONTAINS(error, "depth");

    // The lenient entry point must be bounded too — it is what the plugin host,
    // module manager and mod loader call.
    EXPECT_TRUE(Spark::Json::Parse(hostile).IsNull());
}

TEST(SecurityParsers_JsonDeeplyNestedObjectRejected)
{
    std::string hostile;
    hostile.reserve(400000);
    for (int i = 0; i < 100000; ++i)
        hostile += "{\"a\":";

    Spark::Json::Value value;
    std::string error;
    EXPECT_FALSE(Spark::Json::ParseStrict(hostile, &value, &error));
    EXPECT_TRUE(Spark::Json::Parse(hostile).IsNull());
}

TEST(SecurityParsers_JsonAcceptsNestingWithinTheBound)
{
    // 64 levels: comfortably inside the default budget, so the depth guard must
    // not change acceptance for legitimate documents.
    std::string document(64, '[');
    document += "1";
    document += std::string(64, ']');

    Spark::Json::Value value;
    std::string error;
    EXPECT_TRUE(Spark::Json::ParseStrict(document, &value, &error));
    EXPECT_TRUE(value.IsArray());
}

TEST(SecurityParsers_JsonParseBoundedEnforcesTheCallerBudget)
{
    const Spark::Json::JsonLimits manifestLimits{.maxBytes = 64, .maxDepth = 4, .maxNodes = 16};

    Spark::Json::Value value;
    std::string error;

    // Within budget.
    EXPECT_TRUE(Spark::Json::ParseBounded("{\"id\":\"demo\"}", manifestLimits, &value, &error));
    EXPECT_EQ(value[std::string("id")].AsString(), std::string("demo"));

    // Over the byte budget.
    const std::string oversized = "{\"id\":\"" + std::string(200, 'x') + "\"}";
    EXPECT_FALSE(Spark::Json::ParseBounded(oversized, manifestLimits, &value, &error));
    EXPECT_STR_CONTAINS(error, "size");

    // Over the depth budget.
    EXPECT_FALSE(Spark::Json::ParseBounded("[[[[[[1]]]]]]", manifestLimits, &value, &error));
    EXPECT_STR_CONTAINS(error, "depth");

    // Over the node budget.
    EXPECT_FALSE(
        Spark::Json::ParseBounded("[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18]", manifestLimits, &value, &error));
    EXPECT_STR_CONTAINS(error, "node");
}

TEST(SecurityParsers_JsonNodeBudgetCountsEachObjectMemberOnce)
{
    // Five nodes: the root object plus four member values. The pre-scan used to
    // charge the ':' as well as the '{' or ',' that introduces the member, so
    // every object member cost two nodes and this document was refused at a
    // budget it fits inside.
    const char* const document = "{\"a\":1,\"b\":2,\"c\":3,\"d\":4}";

    Spark::Json::Value value;
    std::string error;

    const Spark::Json::JsonLimits exact{.maxBytes = 256, .maxDepth = 4, .maxNodes = 5};
    EXPECT_TRUE(Spark::Json::ParseBounded(document, exact, &value, &error));
    EXPECT_NEAR(value[std::string("d")].AsNumber(), 4.0, 1e-9);

    // The budget must still bite one node lower, or "counted once" would be
    // indistinguishable from "stopped counting".
    const Spark::Json::JsonLimits tight{.maxBytes = 256, .maxDepth = 4, .maxNodes = 4};
    EXPECT_FALSE(Spark::Json::ParseBounded(document, tight, &value, &error));
    EXPECT_STR_CONTAINS(error, "node");
}

// ============================================================================
// Json: surrogate pairs (security-parsers-16)
// ============================================================================

TEST(SecurityParsers_JsonCombinesSurrogatePairsIntoValidUtf8)
{
    Spark::Json::Value value;
    std::string error;
    EXPECT_TRUE(Spark::Json::ParseStrict("\"\\uD83D\\uDE00\"", &value, &error));

    // U+1F600 is four UTF-8 bytes: F0 9F 98 80. Encoding the halves separately
    // produced two 3-byte lone surrogates (WTF-8), which is not valid UTF-8.
    const std::string decoded = value.AsString();
    EXPECT_EQ(decoded.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(decoded[0]), 0xF0u);
    EXPECT_EQ(static_cast<unsigned char>(decoded[1]), 0x9Fu);
    EXPECT_EQ(static_cast<unsigned char>(decoded[2]), 0x98u);
    EXPECT_EQ(static_cast<unsigned char>(decoded[3]), 0x80u);
}

TEST(SecurityParsers_JsonRejectsUnpairedSurrogate)
{
    Spark::Json::Value value;
    std::string error;
    EXPECT_FALSE(Spark::Json::ParseStrict("\"\\uD83D\"", &value, &error));
    EXPECT_STR_CONTAINS(error, "surrogate");

    EXPECT_FALSE(Spark::Json::ParseStrict("\"\\uDE00\"", &value, &error));
    EXPECT_STR_CONTAINS(error, "surrogate");
}

TEST(SecurityParsers_JsonStillDecodesBmpEscapes)
{
    Spark::Json::Value value;
    std::string error;
    EXPECT_TRUE(Spark::Json::ParseStrict("\"\\u00e9\"", &value, &error));
    EXPECT_EQ(value.AsString().size(), 2u);
}

// ============================================================================
// PacketValidator: string-field screening (security-parsers-02)
// ============================================================================

TEST(SecurityParsers_ChatPayloadWithControlCharactersIsRejected)
{
    Spark::Net::PacketValidator validator;

    Spark::Net::NetworkMessage message;
    message.type = Spark::Net::MessageType::ChatMessage;
    // An authenticated peer sending ANSI escape sequences into the console/ImGui
    // chat renderer. Before the schema's string fields were actually screened,
    // this passed validation untouched and rejectedBadString stayed at 0 forever.
    AppendStringField(message.payload, std::string("hello\x1b[31mworld"));

    const auto result = validator.ValidatePacket(message, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == Spark::Net::PacketViolation::BadString);
    EXPECT_EQ(validator.GetStatistics().rejectedBadString, 1u);
}

TEST(SecurityParsers_ChatPayloadWithPlainTextIsAccepted)
{
    Spark::Net::PacketValidator validator;

    Spark::Net::NetworkMessage message;
    message.type = Spark::Net::MessageType::ChatMessage;
    AppendStringField(message.payload, "nice shot");

    const auto result = validator.ValidatePacket(message, true, true);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(validator.GetStatistics().rejectedBadString, 0u);
}

TEST(SecurityParsers_ChatPayloadWithLyingLengthPrefixIsRejected)
{
    Spark::Net::PacketValidator validator;

    Spark::Net::NetworkMessage message;
    message.type = Spark::Net::MessageType::ChatMessage;
    // Declares 4096 bytes of text but supplies four.
    message.payload = {0x00, 0x10, 'a', 'b', 'c', 'd'};

    const auto result = validator.ValidatePacket(message, true, true);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.violation == Spark::Net::PacketViolation::BadString);
}

TEST(SecurityParsers_BinarySchemasAreNotTextChecked)
{
    Spark::Net::PacketValidator validator;

    // ClientInput is opaque binary and declares no string-field offset, so bytes
    // that look like control characters must not be rejected as text.
    Spark::Net::NetworkMessage message;
    message.type = Spark::Net::MessageType::ClientInput;
    message.payload = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    EXPECT_TRUE(validator.ValidatePacket(message, true, true).valid);
}

// ============================================================================
// Virtual path policy (security-parsers-08)
// ============================================================================

TEST(SecurityParsers_VirtualPathPolicyRejectsWindowsEscapes)
{
    // Traversal, decided on the normalized path rather than by substring.
    EXPECT_FALSE(Spark::IsVirtualPathSafe("../secrets.txt"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("assets/../../secrets.txt"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("a/../../etc/x"));

    // Absolute and drive-relative.
    EXPECT_FALSE(Spark::IsVirtualPathSafe("/etc/passwd"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("\\Windows\\System32\\config"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("C:evil"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("C:/Windows/notepad.exe"));

    // NTFS alternate data stream.
    EXPECT_FALSE(Spark::IsVirtualPathSafe("assets/logo.png:secret"));

    // Reserved device names resolve as devices regardless of the directory prefix.
    EXPECT_FALSE(Spark::IsVirtualPathSafe("COM1"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("mods/pack/NUL"));
    EXPECT_FALSE(Spark::IsVirtualPathSafe("mods/con.txt"));

    EXPECT_FALSE(Spark::IsVirtualPathSafe(""));
}

TEST(SecurityParsers_VirtualPathPolicyKeepsLegitimateNames)
{
    // The old substring test for ".." rejected these. Normalization must not.
    EXPECT_TRUE(Spark::IsVirtualPathSafe("weapon..old.mesh"));
    EXPECT_TRUE(Spark::IsVirtualPathSafe("models/rifle..lod1.mesh"));
    EXPECT_TRUE(Spark::IsVirtualPathSafe("textures/player.png"));
    EXPECT_TRUE(Spark::IsVirtualPathSafe("audio/ui/click.wav"));
    // Resolves back inside the root, so it is contained.
    EXPECT_TRUE(Spark::IsVirtualPathSafe("models/../textures/player.png"));
    // "common" is not a reserved device name even though it starts with "com".
    EXPECT_TRUE(Spark::IsVirtualPathSafe("common/shared.mesh"));
}

// ============================================================================
// VFS mount priority (security-parsers-09)
// ============================================================================

TEST(SecurityParsers_ZeroByteOverrideWinsThePriorityContest)
{
    const auto engineRoot = ScratchPath("vfs_engine");
    const auto modRoot = ScratchPath("vfs_mod");
    std::error_code ec;
    std::filesystem::create_directories(engineRoot, ec);
    std::filesystem::create_directories(modRoot, ec);

    {
        std::ofstream original(engineRoot / "config.cfg", std::ios::binary | std::ios::trunc);
        original << "difficulty=hard";
    }
    {
        // A mod blanks the config by shipping a zero-byte override.
        std::ofstream blanked(modRoot / "config.cfg", std::ios::binary | std::ios::trunc);
    }

    auto& vfs = Spark::VirtualFileSystem::GetInstance();
    vfs.Initialize();
    vfs.Unmount("securityparsers_engine");
    vfs.Unmount("securityparsers_mod");
    vfs.Mount("securityparsers_engine", std::make_unique<Spark::LocalFileProvider>(engineRoot.string()),
              Spark::ENGINE_PRIORITY);
    vfs.Mount("securityparsers_mod", std::make_unique<Spark::LocalFileProvider>(modRoot.string()), Spark::MOD_PRIORITY);

    // Before the fix an empty read was treated as a failure and the walk fell
    // through to the engine copy, silently inverting the mount priority.
    EXPECT_EQ(vfs.ReadFile("config.cfg").size(), 0u);
    EXPECT_EQ(vfs.ReadTextFile("config.cfg").size(), 0u);
    EXPECT_EQ(vfs.ResolveProvider("config.cfg"), std::string("securityparsers_mod"));

    // A rejected path is never re-offered to a lower-priority mount.
    EXPECT_EQ(vfs.ReadFile("../config.cfg").size(), 0u);
    EXPECT_FALSE(vfs.Exists("../config.cfg"));

    vfs.Unmount("securityparsers_engine");
    vfs.Unmount("securityparsers_mod");
    std::filesystem::remove_all(engineRoot, ec);
    std::filesystem::remove_all(modRoot, ec);
}

// ============================================================================
// Scene manifest containment (security-parsers-17)
// ============================================================================

TEST(SecurityParsers_SceneManifestDropsOutOfRootAssetPaths)
{
    const std::string content = "name = TownSquare\n"
                                "mesh = models/fountain.mesh\n"
                                "mesh = ../../../../Users/victim/.ssh/id_rsa\n"
                                "texture = /etc/shadow\n"
                                "texture = textures/cobblestone.dds\n"
                                "audio = COM1\n"
                                "audio = audio/ambience.wav\n";

    const auto manifest = Spark::Streaming::SceneManifest::ParseFromString(content);

    EXPECT_EQ(manifest.name, std::string("TownSquare"));
    EXPECT_EQ(manifest.meshPaths.size(), 1u);
    EXPECT_EQ(manifest.meshPaths[0], std::string("models/fountain.mesh"));
    EXPECT_EQ(manifest.texturePaths.size(), 1u);
    EXPECT_EQ(manifest.texturePaths[0], std::string("textures/cobblestone.dds"));
    EXPECT_EQ(manifest.audioPaths.size(), 1u);
    EXPECT_EQ(manifest.audioPaths[0], std::string("audio/ambience.wav"));
    EXPECT_EQ(manifest.TotalAssetCount(), 3u);
}

// ============================================================================
// Animation loader desync (security-parsers-12)
// ============================================================================

TEST(SecurityParsers_SkeletonWithOutOfRangeNameLengthIsRejected)
{
    const auto path = ScratchPath("desync.skel");

    std::vector<uint8_t> file;
    file.insert(file.end(), {'S', 'K', 'E', 'L'});
    AppendRaw(file, static_cast<uint32_t>(1)); // version
    AppendRaw(file, static_cast<uint32_t>(2)); // boneCount
    // Bone 0 declares a 65,536 byte name but supplies none. The old loader skipped
    // the branch WITHOUT consuming the bytes, so parentIndex and both matrices were
    // filled from the following bytes and the load still reported success.
    AppendRaw(file, static_cast<uint32_t>(0x00010000));
    AppendRaw(file, static_cast<int32_t>(-1));
    for (int i = 0; i < 32; ++i)
        AppendRaw(file, 1.0f);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    auto skeleton = Spark::Animation::AnimationManager::GetInstance().LoadSkeleton(path.string());
    ASSERT_TRUE(skeleton != nullptr);
    EXPECT_EQ(skeleton->bones.size(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SecurityParsers_SkeletonWithNonFiniteMatrixIsRejected)
{
    const auto path = ScratchPath("nonfinite.skel");

    std::vector<uint8_t> file;
    file.insert(file.end(), {'S', 'K', 'E', 'L'});
    AppendRaw(file, static_cast<uint32_t>(1)); // version
    AppendRaw(file, static_cast<uint32_t>(1)); // boneCount
    AppendRaw(file, static_cast<uint32_t>(4)); // nameLen
    file.insert(file.end(), {'r', 'o', 'o', 't'});
    AppendRaw(file, static_cast<int32_t>(-1));
    // A NaN bit pattern in the first matrix element. NaNs propagate through global
    // transforms into skinning and anything driven from bone transforms.
    AppendRaw(file, static_cast<uint32_t>(0x7FC00000));
    for (int i = 0; i < 31; ++i)
        AppendRaw(file, 1.0f);
    for (int i = 0; i < 16; ++i)
        AppendRaw(file, 1.0f);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    auto skeleton = Spark::Animation::AnimationManager::GetInstance().LoadSkeleton(path.string());
    ASSERT_TRUE(skeleton != nullptr);
    EXPECT_EQ(skeleton->bones.size(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SecurityParsers_AnimationChannelDesyncAbandonsWholeFile)
{
    // Two clips. Clip 0's first channel declares a 65,536 byte bone name but
    // supplies none: the length prefix is consumed and the payload is not, so the
    // reader is one field out of phase from that point on. The channel-level guard
    // used to break only the INNER loop, push the half-built clip, and keep decoding
    // clip 1 from misaligned bytes — returning two plausible-looking clips and
    // logging success. A desync must abandon the whole file.
    const auto path = ScratchPath("desync.sanim");

    std::vector<uint8_t> file;
    file.insert(file.end(), {'A', 'N', 'I', 'M'});
    AppendRaw(file, static_cast<uint32_t>(1)); // version
    AppendRaw(file, static_cast<uint32_t>(2)); // clipCount

    // Clip 0
    const std::string clipName = "walk";
    AppendRaw(file, static_cast<uint32_t>(clipName.size()));
    file.insert(file.end(), clipName.begin(), clipName.end());
    AppendRaw(file, 1.0f);                              // duration
    AppendRaw(file, 30.0f);                             // ticksPerSecond
    file.push_back(uint8_t{1});                         // loop
    AppendRaw(file, static_cast<uint32_t>(1));          // channelCount
    AppendRaw(file, static_cast<uint32_t>(0x00010000)); // hostile boneNameLen

    // Trailing bytes that a desynced reader would happily decode as clip 1.
    for (int i = 0; i < 64; ++i)
        AppendRaw(file, static_cast<uint32_t>(1));

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    const auto clips = Spark::Animation::AnimationManager::GetInstance().LoadAnimations(path.string());
    EXPECT_EQ(clips.size(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SecurityParsers_TruncatedAnimationKeyframesAbandonWholeFile)
{
    // One clip, one channel, three declared position keys and only one supplied.
    // Every keyframe loop stops on !good() and leaves the rest of its vector
    // zero-initialised, so without a truncation check the channel is pushed with two
    // silent zero keys, the clip is pushed, and LoadAnimations logs a successful load
    // of a file that does not contain what it declares.
    const auto path = ScratchPath("truncated_keys.sanim");

    std::vector<uint8_t> file;
    file.insert(file.end(), {'A', 'N', 'I', 'M'});
    AppendRaw(file, static_cast<uint32_t>(1)); // version
    AppendRaw(file, static_cast<uint32_t>(1)); // clipCount

    const std::string clipName = "walk";
    AppendRaw(file, static_cast<uint32_t>(clipName.size()));
    file.insert(file.end(), clipName.begin(), clipName.end());
    AppendRaw(file, 1.0f);                     // duration
    AppendRaw(file, 30.0f);                    // ticksPerSecond
    file.push_back(uint8_t{1});                // loop
    AppendRaw(file, static_cast<uint32_t>(1)); // channelCount

    const std::string boneName = "bone";
    AppendRaw(file, static_cast<uint32_t>(boneName.size()));
    file.insert(file.end(), boneName.begin(), boneName.end());
    AppendRaw(file, static_cast<int32_t>(0));  // boneIndex
    AppendRaw(file, static_cast<uint32_t>(3)); // posKeyCount - only one key follows
    AppendRaw(file, 0.0f);                     // key 0: time
    AppendRaw(file, 1.0f);                     // key 0: value.x
    AppendRaw(file, 2.0f);                     // key 0: value.y
    AppendRaw(file, 3.0f);                     // key 0: value.z

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    const auto clips = Spark::Animation::AnimationManager::GetInstance().LoadAnimations(path.string());
    EXPECT_EQ(clips.size(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SecurityParsers_WellFormedAnimationFileStillLoads)
{
    // The corruption checks must not reject a complete file: a clip whose declared
    // counts all match its contents loads with every channel intact.
    const auto path = ScratchPath("wellformed.sanim");

    std::vector<uint8_t> file;
    file.insert(file.end(), {'A', 'N', 'I', 'M'});
    AppendRaw(file, static_cast<uint32_t>(1)); // version
    AppendRaw(file, static_cast<uint32_t>(1)); // clipCount

    const std::string clipName = "idle";
    AppendRaw(file, static_cast<uint32_t>(clipName.size()));
    file.insert(file.end(), clipName.begin(), clipName.end());
    AppendRaw(file, 2.0f);                     // duration
    AppendRaw(file, 30.0f);                    // ticksPerSecond
    file.push_back(uint8_t{0});                // loop
    AppendRaw(file, static_cast<uint32_t>(1)); // channelCount

    const std::string boneName = "root";
    AppendRaw(file, static_cast<uint32_t>(boneName.size()));
    file.insert(file.end(), boneName.begin(), boneName.end());
    AppendRaw(file, static_cast<int32_t>(0));  // boneIndex
    AppendRaw(file, static_cast<uint32_t>(1)); // posKeyCount
    for (int i = 0; i < 4; ++i)                // time + XMFLOAT3
        AppendRaw(file, 0.5f);
    AppendRaw(file, static_cast<uint32_t>(1)); // rotKeyCount
    for (int i = 0; i < 5; ++i)                // time + XMFLOAT4
        AppendRaw(file, 0.5f);
    AppendRaw(file, static_cast<uint32_t>(1)); // sclKeyCount
    for (int i = 0; i < 4; ++i)                // time + XMFLOAT3
        AppendRaw(file, 1.0f);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    const auto clips = Spark::Animation::AnimationManager::GetInstance().LoadAnimations(path.string());
    ASSERT_TRUE(clips.size() == 1u);
    EXPECT_EQ(clips[0]->channels.size(), 1u);
    EXPECT_EQ(clips[0]->channels[0].positionKeys.size(), 1u);
    EXPECT_EQ(clips[0]->channels[0].rotationKeys.size(), 1u);
    EXPECT_EQ(clips[0]->channels[0].scaleKeys.size(), 1u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SecurityParsers_CorruptSkeletonIsNotCachedSoARepairCanLoad)
{
    // A failed load that is memoised makes a repaired asset unreachable for the
    // lifetime of the process: the second LoadSkeleton below would return the cached
    // empty skeleton and the character would silently stay in bind pose. The same
    // path is loaded twice, corrupt first, valid second.
    const auto path = ScratchPath("repairable.skel");

    std::vector<uint8_t> corrupt;
    corrupt.insert(corrupt.end(), {'S', 'K', 'E', 'L'});
    AppendRaw(corrupt, static_cast<uint32_t>(1));          // version
    AppendRaw(corrupt, static_cast<uint32_t>(1));          // boneCount
    AppendRaw(corrupt, static_cast<uint32_t>(0x00010000)); // hostile nameLen
    AppendRaw(corrupt, static_cast<int32_t>(-1));
    for (int i = 0; i < 32; ++i)
        AppendRaw(corrupt, 1.0f);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(corrupt.data()), static_cast<std::streamsize>(corrupt.size()));
    }

    auto& manager = Spark::Animation::AnimationManager::GetInstance();
    auto broken = manager.LoadSkeleton(path.string());
    ASSERT_TRUE(broken != nullptr);
    EXPECT_EQ(broken->bones.size(), 0u);

    // Repair the asset in place and load it again through the same cache key.
    const std::string boneName = "root";
    std::vector<uint8_t> repaired;
    repaired.insert(repaired.end(), {'S', 'K', 'E', 'L'});
    AppendRaw(repaired, static_cast<uint32_t>(1)); // version
    AppendRaw(repaired, static_cast<uint32_t>(1)); // boneCount
    AppendRaw(repaired, static_cast<uint32_t>(boneName.size()));
    repaired.insert(repaired.end(), boneName.begin(), boneName.end());
    AppendRaw(repaired, static_cast<int32_t>(-1));
    for (int i = 0; i < 32; ++i)
        AppendRaw(repaired, 1.0f);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(repaired.data()), static_cast<std::streamsize>(repaired.size()));
    }

    auto fixed = manager.LoadSkeleton(path.string());
    ASSERT_TRUE(fixed != nullptr);
    EXPECT_EQ(fixed->bones.size(), 1u);
    EXPECT_EQ(fixed->bones[0].name, std::string("root"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SecurityParsers_TruncatedSkeletonIsRejectedNotPartiallyLoaded)
{
    // Declares two bones and supplies one. Without the mid-bone truncation guard the
    // second bone is filled from whatever the failed reads leave behind, and the
    // partial skeleton is cached and logged as a successful load of a file that is
    // missing half its content. A truncated skeleton must load nothing.
    const auto path = ScratchPath("truncated.skel");

    const std::string boneName = "root";
    std::vector<uint8_t> file;
    file.insert(file.end(), {'S', 'K', 'E', 'L'});
    AppendRaw(file, static_cast<uint32_t>(1)); // version
    AppendRaw(file, static_cast<uint32_t>(2)); // boneCount - only one bone follows
    AppendRaw(file, static_cast<uint32_t>(boneName.size()));
    file.insert(file.end(), boneName.begin(), boneName.end());
    AppendRaw(file, static_cast<int32_t>(-1));
    for (int i = 0; i < 32; ++i)
        AppendRaw(file, 1.0f);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }

    auto skeleton = Spark::Animation::AnimationManager::GetInstance().LoadSkeleton(path.string());
    ASSERT_TRUE(skeleton != nullptr);
    EXPECT_EQ(skeleton->bones.size(), 0u);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ============================================================================
// Server-side hit validation (security-parsers-14)
// ============================================================================

TEST(SecurityParsers_HitValidationClampsClientSuppliedRange)
{
    auto& manager = Spark::Net::NetworkManager::GetInstance();
    manager.GetLagCompensator().Clear();

    Spark::Net::HistorySnapshot snapshot;
    snapshot.timestamp = 100.0f;
    Spark::Net::HistorySnapshot::EntityState distant{};
    distant.networkID = 7;
    distant.position = {0.0f, 0.0f, 50000.0f};
    distant.boundsMin = {-1.0f, -1.0f, 49999.0f};
    distant.boundsMax = {1.0f, 1.0f, 50001.0f};
    snapshot.entities.push_back(distant);
    manager.GetLagCompensator().RecordSnapshot(snapshot);

    const DirectX::XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    const DirectX::XMFLOAT3 direction{0.0f, 0.0f, 1.0f};

    // A client asking for a 1e30 metre hitscan reached everything in the rewound
    // snapshot before maxDistance was clamped.
    const auto absurd = manager.ValidateHit(100.0f, 0.0f, origin, direction, 1e30f);
    EXPECT_FALSE(absurd.hit);

    // Non-finite scalars are refused outright rather than poisoning comparisons.
    const auto nanRequest =
        manager.ValidateHit(std::numeric_limits<float>::quiet_NaN(), 0.0f, origin, direction, 100.0f);
    EXPECT_FALSE(nanRequest.hit);

    const DirectX::XMFLOAT3 degenerate{0.0f, 0.0f, 0.0f};
    const auto zeroRay = manager.ValidateHit(100.0f, 0.0f, origin, degenerate, 100.0f);
    EXPECT_FALSE(zeroRay.hit);

    manager.GetLagCompensator().Clear();
}

TEST(SecurityParsers_HitValidationStillAcceptsLegitimateShots)
{
    auto& manager = Spark::Net::NetworkManager::GetInstance();
    manager.GetLagCompensator().Clear();

    Spark::Net::HistorySnapshot snapshot;
    snapshot.timestamp = 100.0f;
    Spark::Net::HistorySnapshot::EntityState target{};
    target.networkID = 42;
    target.position = {0.0f, 0.0f, 50.0f};
    target.boundsMin = {-1.0f, -1.0f, 49.0f};
    target.boundsMax = {1.0f, 1.0f, 51.0f};
    snapshot.entities.push_back(target);
    manager.GetLagCompensator().RecordSnapshot(snapshot);

    const DirectX::XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    const DirectX::XMFLOAT3 direction{0.0f, 0.0f, 1.0f};

    const auto result = manager.ValidateHit(100.0f, 0.0f, origin, direction, 1000.0f);
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.entityID, 42u);

    manager.GetLagCompensator().Clear();
}

TEST(SecurityParsers_HitValidationRejectsOutOfWindowClientTimestamp)
{
    // Clamping halfRTT alone left clientTimestamp unbounded, and RewindToTime falls
    // back to "whichever snapshot we have" when the target is outside the retained
    // history — so a client could name any time at all and still resolve against an
    // edge snapshot. The rewind target is now bounded by the server's own newest
    // snapshot and the compensator's retained window.
    auto& manager = Spark::Net::NetworkManager::GetInstance();
    manager.GetLagCompensator().Clear();
    manager.GetLagCompensator().SetMaxHistoryDuration(1.0f);

    Spark::Net::HistorySnapshot snapshot;
    snapshot.timestamp = 100.0f;
    Spark::Net::HistorySnapshot::EntityState target{};
    target.networkID = 42;
    target.position = {0.0f, 0.0f, 50.0f};
    target.boundsMin = {-1.0f, -1.0f, 49.0f};
    target.boundsMax = {1.0f, 1.0f, 51.0f};
    snapshot.entities.push_back(target);
    manager.GetLagCompensator().RecordSnapshot(snapshot);

    const DirectX::XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    const DirectX::XMFLOAT3 direction{0.0f, 0.0f, 1.0f};

    // Control: a shot at the server's own snapshot time still lands.
    EXPECT_TRUE(manager.ValidateHit(100.0f, 0.0f, origin, direction, 1000.0f).hit);

    // A timestamp far beyond the server's newest snapshot is not a world state the
    // client could have seen.
    EXPECT_FALSE(manager.ValidateHit(5000.0f, 0.0f, origin, direction, 1000.0f).hit);

    // A timestamp older than the retained window resolved against the oldest
    // snapshot before the bound existed.
    EXPECT_FALSE(manager.ValidateHit(0.5f, 0.0f, origin, direction, 1000.0f).hit);

    manager.GetLagCompensator().Clear();
}
