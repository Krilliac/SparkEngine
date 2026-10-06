/**
 * @file FuzzAssetMigrationProduction.cpp
 * @brief libc++-compiled production adapter for the asset migration libFuzzer harness.
 *
 * Spark::AssetMigrationRegistry::MigrateAsset (Core/AssetMigration.h) reads the SPRK header
 * of an asset file loaded from disk, chooses a chain of registered migration steps and
 * rewrites the buffer in place. The engine registers no steps of its own, so the adapter
 * registers three representative ones once:
 *  - Scene 1.0.0 -> 1.1.0 parses a [u32 count][strings] payload and adds a u32 flags word
 *    after every string (it refuses a malformed or trailing payload),
 *  - any type 1.1.0 -> 2.0.0 appends a "V2" marker,
 *  - Material 1.0.0 -> 2.0.0 keeps the payload,
 * and keeps Prefab's current version at 1.0.0 while every other type is at 2.0.0. The first
 * input byte is the asset type the caller expects; the rest is the file. A violated contract
 * aborts so libFuzzer records a crash:
 *  - the result and the rewritten bytes equal an independent model of the documented format:
 *    a valid header (magic, a headerSize no smaller than the fixed fields, a known type equal
 *    to the expected one), a version no newer than current, a migration path, and a payload
 *    that its dataSize and checksum describe; a current file is left as it is,
 *  - a refused file is left byte-for-byte untouched,
 *  - a migrated file's header describes the bytes that follow it (headerSize, dataSize,
 *    checksum) and names the current version,
 *  - migrating a migrated file again is a no-op that succeeds,
 *  - migrating the same bytes twice gives the same result.
 */

#include "FuzzAssetMigrationProduction.h"

#include "Core/AssetMigration.h"
#include "Utils/CRC32.h"
#include "Utils/Serializer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::uint32_t kMagic = 0x5350524Bu;
    constexpr std::size_t kHeaderBytes = sizeof(Spark::AssetFileHeader);

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzAssetMigration: AssetMigrationRegistry::MigrateAsset violated: %s\n", what);
        std::abort();
    }

    using Spark::AssetFileHeader;
    using Spark::AssetType;
    using Spark::AssetVersion;
    using Transform = bool (*)(Spark::BinaryReader&, Spark::BinaryWriter&);

    std::vector<std::uint8_t> Remaining(Spark::BinaryReader& reader)
    {
        std::vector<std::uint8_t> bytes(reader.Remaining());
        if (!reader.ReadBytes(bytes.data(), bytes.size()))
        {
            InvariantFailure("the fixture could not read the remaining payload");
        }
        return bytes;
    }

    /// Scene 1.0 -> 1.1: [u32 count][count strings] becomes [u32 count][count x (string, u32 0)].
    bool AddNameFlags(Spark::BinaryReader& reader, Spark::BinaryWriter& writer)
    {
        const auto count = reader.Read<std::uint32_t>();
        if (reader.HasError() || count > reader.Remaining() / 4u)
        {
            return false;
        }
        writer.Write<std::uint32_t>(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::string name = reader.ReadString();
            if (reader.HasError())
            {
                return false;
            }
            writer.WriteString(name);
            writer.Write<std::uint32_t>(0u);
        }
        return reader.IsEOF();
    }

    /// Any type 1.1 -> 2.0: the payload gains a two-byte "V2" marker.
    bool AppendV2Marker(Spark::BinaryReader& reader, Spark::BinaryWriter& writer)
    {
        const std::vector<std::uint8_t> bytes = Remaining(reader);
        writer.WriteBytes(bytes.data(), bytes.size());
        writer.Write<std::uint8_t>('V');
        writer.Write<std::uint8_t>('2');
        return true;
    }

    /// Material 1.0 -> 2.0: the payload is unchanged.
    bool CopyPayload(Spark::BinaryReader& reader, Spark::BinaryWriter& writer)
    {
        const std::vector<std::uint8_t> bytes = Remaining(reader);
        writer.WriteBytes(bytes.data(), bytes.size());
        return true;
    }

    class FixtureStep final : public Spark::IMigrationStep
    {
      public:
        FixtureStep(AssetVersion from, AssetVersion to, AssetType type, Transform transform)
            : m_from(from), m_to(to), m_type(type), m_transform(transform)
        {
        }

        AssetVersion GetSourceVersion() const override { return m_from; }
        AssetVersion GetTargetVersion() const override { return m_to; }
        bool Migrate(Spark::BinaryReader& reader, Spark::BinaryWriter& writer) override
        {
            return m_transform(reader, writer);
        }
        std::string_view GetDescription() const override { return "SEC-120 fuzz fixture step"; }
        AssetType GetAssetType() const override { return m_type; }

      private:
        AssetVersion m_from;
        AssetVersion m_to;
        AssetType m_type;
        Transform m_transform;
    };

    constexpr AssetVersion kV100{1, 0, 0};
    constexpr AssetVersion kV110{1, 1, 0};
    constexpr AssetVersion kV200{2, 0, 0};

    bool RegisterFixtureSteps()
    {
        auto& registry = Spark::AssetMigrationRegistry::GetInstance();
        registry.Initialize();
        registry.RegisterMigration(std::make_unique<FixtureStep>(kV100, kV110, AssetType::Scene, &AddNameFlags));
        registry.RegisterMigration(std::make_unique<FixtureStep>(kV110, kV200, AssetType::Unknown, &AppendV2Marker));
        registry.RegisterMigration(std::make_unique<FixtureStep>(kV100, kV200, AssetType::Material, &CopyPayload));
        registry.SetCurrentVersion(AssetType::Prefab, kV100);
        return true;
    }

    AssetVersion ModelCurrentVersion(AssetType type)
    {
        if (type == AssetType::Prefab ||
            static_cast<std::uint8_t>(type) > static_cast<std::uint8_t>(AssetType::ShaderCache))
        {
            return kV100;
        }
        return kV200;
    }

    std::vector<Transform> ModelPath(AssetType type, AssetVersion from)
    {
        if (from == kV100 && type == AssetType::Scene)
        {
            return {&AddNameFlags, &AppendV2Marker};
        }
        if (from == kV100 && type == AssetType::Material)
        {
            return {&CopyPayload};
        }
        if (from == kV110)
        {
            return {&AppendV2Marker};
        }
        return {};
    }

    std::uint32_t Crc(const std::uint8_t* bytes, std::size_t size)
    {
        return size == 0 ? 0u : Spark::ComputeCRC32(bytes, size);
    }

    enum class Outcome
    {
        Refused,
        Unchanged,
        Migrated
    };

    struct Expected
    {
        Outcome outcome = Outcome::Refused;
        std::vector<std::uint8_t> bytes;
    };

    /// Independent model of MigrateAsset's documented contract.
    Expected Model(AssetType type, const std::vector<std::uint8_t>& file)
    {
        if (file.size() < kHeaderBytes)
        {
            return {};
        }
        AssetFileHeader header{};
        std::memcpy(&header, file.data(), kHeaderBytes);
        if (header.magic != kMagic || header.headerSize < kHeaderBytes ||
            static_cast<std::uint8_t>(header.assetType) > static_cast<std::uint8_t>(AssetType::ShaderCache) ||
            header.assetType != type)
        {
            return {};
        }
        const AssetVersion current = ModelCurrentVersion(type);
        if (header.version > current)
        {
            return {};
        }
        if (header.version == current)
        {
            return {Outcome::Unchanged, file};
        }
        const std::vector<Transform> path = ModelPath(type, header.version);
        if (path.empty() || header.headerSize > file.size())
        {
            return {};
        }
        const std::size_t payloadSize = file.size() - header.headerSize;
        if (header.dataSize != payloadSize || header.checksum != Crc(file.data() + header.headerSize, payloadSize))
        {
            return {};
        }

        std::vector<std::uint8_t> payload(file.begin() + static_cast<std::ptrdiff_t>(header.headerSize), file.end());
        for (const Transform transform : path)
        {
            Spark::BinaryReader reader(payload);
            Spark::BinaryWriter writer;
            if (!transform(reader, writer))
            {
                return {};
            }
            payload = writer.GetBuffer();
        }
        header.version = current;
        header.headerSize = static_cast<std::uint32_t>(kHeaderBytes);
        header.dataSize = payload.size();
        header.checksum = Crc(payload.data(), payload.size());
        std::vector<std::uint8_t> migrated(kHeaderBytes);
        std::memcpy(migrated.data(), &header, kHeaderBytes);
        migrated.insert(migrated.end(), payload.begin(), payload.end());
        return {Outcome::Migrated, std::move(migrated)};
    }

    void CheckSelfDescribing(const std::vector<std::uint8_t>& file, AssetType type)
    {
        if (file.size() < kHeaderBytes)
        {
            InvariantFailure("a migrated file is shorter than its header");
        }
        AssetFileHeader header{};
        std::memcpy(&header, file.data(), kHeaderBytes);
        if (header.magic != kMagic || header.assetType != type)
        {
            InvariantFailure("a migrated file lost its magic or asset type");
        }
        if (!(header.version == ModelCurrentVersion(type)))
        {
            InvariantFailure("a migrated file does not name the current version");
        }
        if (header.headerSize != kHeaderBytes)
        {
            InvariantFailure("a migrated file's headerSize does not say where its payload starts");
        }
        const std::size_t payloadSize = file.size() - kHeaderBytes;
        if (header.dataSize != payloadSize)
        {
            InvariantFailure("a migrated file's dataSize does not match its payload");
        }
        if (header.checksum != Crc(file.data() + kHeaderBytes, payloadSize))
        {
            InvariantFailure("a migrated file's checksum does not match its payload");
        }
    }
} // namespace

extern "C" int SparkFuzzMigrateAsset(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    static const bool registered = RegisterFixtureSteps();
    (void)registered;
    const auto& registry = Spark::AssetMigrationRegistry::GetInstance();

    const AssetType type = size == 0 ? AssetType::Scene : static_cast<AssetType>(data[0]);
    const std::vector<std::uint8_t> original =
        size <= 1 ? std::vector<std::uint8_t>() : std::vector<std::uint8_t>(data + 1, data + size);
    const Expected expected = Model(type, original);

    std::vector<std::uint8_t> file = original;
    const bool ok = registry.MigrateAsset(file, type);
    if (!ok)
    {
        if (file != original)
        {
            InvariantFailure("a refused file was modified");
        }
        if (expected.outcome != Outcome::Refused)
        {
            InvariantFailure("a file the format accepts was refused");
        }
        return 0;
    }
    if (expected.outcome == Outcome::Refused)
    {
        InvariantFailure("a file the format refuses was accepted");
    }
    if (file != expected.bytes)
    {
        InvariantFailure("the rewritten file differs from the format model");
    }
    if (expected.outcome == Outcome::Migrated)
    {
        CheckSelfDescribing(file, type);
    }

    std::vector<std::uint8_t> again = file;
    if (!registry.MigrateAsset(again, type) || again != file)
    {
        InvariantFailure("migrating a current file again was not a successful no-op");
    }

    std::vector<std::uint8_t> repeat = original;
    if (!registry.MigrateAsset(repeat, type) || repeat != file)
    {
        InvariantFailure("migrating the same bytes twice gave different results");
    }
    return 0;
}
