/**
 * @file FBXImporter.h
 * @brief Binary FBX file format importer for mesh, skeleton, and animation data
 * @author Spark Engine Team
 * @date 2026
 *
 * Parses the Autodesk FBX binary format directly (no external SDK dependency)
 * to extract mesh geometry, bone hierarchies, and keyframe animations. Supports
 * coordinate system conversion, triangulation, and configurable import options.
 *
 * @see AssetPipeline.h
 */

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace Spark::Graphics
{

    // ============================================================================
    // Coordinate system
    // ============================================================================

    /** @brief Target coordinate system for imported geometry. */
    enum class CoordinateSystem : uint8_t
    {
        LeftHanded, ///< Direct3D convention (default)
        RightHanded ///< OpenGL / FBX native convention
    };

    // ============================================================================
    // Import options
    // ============================================================================

    /** @brief Configuration for FBX import behaviour. */
    struct FBXImportOptions
    {
        float scaleFactor = 1.0f;
        bool importNormals = true;
        bool importUVs = true;
        bool importAnimations = true;
        bool triangulate = true;
        bool flipUVs = false;
        CoordinateSystem targetCoordSystem = CoordinateSystem::LeftHanded;
    };

    // ============================================================================
    // Imported data structures
    // ============================================================================

    /** @brief Mesh geometry extracted from an FBX Geometry node. */
    struct FBXMeshData
    {
        std::vector<float> vertices;   ///< Flat xyz triples
        std::vector<float> normals;    ///< Flat xyz triples (per-vertex)
        std::vector<float> uvs;        ///< Flat uv pairs (per-vertex)
        std::vector<uint32_t> indices; ///< Triangle indices
        uint32_t vertexCount = 0;      ///< Number of vertices
        int materialIndex = -1;        ///< Assigned material index (-1 = none)
    };

    /** @brief Single bone from the skeleton hierarchy. */
    struct FBXBoneData
    {
        std::string name;
        int parentIndex = -1;                ///< -1 for root bones
        std::array<float, 16> bindPose = {}; ///< Column-major 4x4 matrix
    };

    /** @brief A single animation keyframe for one bone. */
    struct FBXKeyframe
    {
        float time = 0.0f;
        std::array<float, 3> position = {};
        std::array<float, 4> rotation = {0.0f, 0.0f, 0.0f, 1.0f}; ///< Quaternion xyzw
        std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    };

    /** @brief Keyframe track for a single bone within an animation. */
    struct FBXBoneTrack
    {
        std::string boneName;
        std::vector<FBXKeyframe> keyframes;
    };

    /** @brief A complete animation clip extracted from an FBX AnimationStack. */
    struct FBXAnimationData
    {
        std::string name;
        float duration = 0.0f;
        float fps = 24.0f;
        std::vector<FBXBoneTrack> boneTracks;
    };

    /** @brief Complete result of an FBX import operation. */
    struct FBXImportResult
    {
        std::vector<FBXMeshData> meshes;
        std::vector<FBXBoneData> bones;
        std::vector<FBXAnimationData> animations;
        std::vector<std::string> warnings;
        std::string errorMessage; ///< Set on failure
        uint32_t fbxVersion = 0;  ///< Detected FBX format version
        bool success = false;
    };

    // ============================================================================
    // FBX binary format constants
    // ============================================================================

    namespace FBXConstants
    {
        /** @brief FBX binary file magic string (23 bytes including null). */
        static constexpr const char MAGIC[] = "Kaydara FBX Binary  \x00\x1a";
        static constexpr size_t MAGIC_LENGTH = sizeof(MAGIC); // includes the format's final zero byte
        static constexpr size_t HEADER_SIZE = 27; ///< Magic (23) + unknown (2) + version (4) but first 27 bytes total

        // Property type codes
        static constexpr char PROP_INT16 = 'Y';
        static constexpr char PROP_INT8 = 'Z';
        static constexpr char PROP_BOOL = 'B';
        static constexpr char PROP_CHAR = 'C';
        static constexpr char PROP_INT32 = 'I';
        static constexpr char PROP_FLOAT = 'F';
        static constexpr char PROP_DOUBLE = 'D';
        static constexpr char PROP_INT64 = 'L';
        static constexpr char PROP_STRING = 'S';
        static constexpr char PROP_RAW = 'R';
        static constexpr char PROP_FLOAT_ARRAY = 'f';
        static constexpr char PROP_DOUBLE_ARRAY = 'd';
        static constexpr char PROP_INT32_ARRAY = 'i';
        static constexpr char PROP_INT64_ARRAY = 'l';
        static constexpr char PROP_BOOL_ARRAY = 'b';
        static constexpr char PROP_BYTE_ARRAY = 'c';
    } // namespace FBXConstants

    // Convenience aliases used by FBXImporter.cpp
    static constexpr const char* FBX_MAGIC = FBXConstants::MAGIC;
    static constexpr size_t FBX_MAGIC_LENGTH = FBXConstants::MAGIC_LENGTH;

    // ============================================================================
    // Binary reader helper
    // ============================================================================

    /** @brief Lightweight reader over a contiguous byte buffer. */
    class FBXBinaryReader
    {
      public:
        FBXBinaryReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

        /** @brief Read a trivially-copyable value and advance the cursor. */
        template <typename T> T Read()
        {
            static_assert(std::is_trivially_copyable_v<T>, "FBXBinaryReader only supports trivially-copyable values");
            static_assert(std::endian::native == std::endian::little,
                          "FBX binary scalar decoding currently requires a little-endian target");
            T value{};
            if (!HasRemaining(sizeof(T)))
            {
                Invalidate();
                return value;
            }
            std::memcpy(&value, m_data + m_pos, sizeof(T));
            m_pos += sizeof(T);
            return value;
        }

        /** @brief Read a string of known length. */
        std::string ReadString(size_t length)
        {
            if (!HasRemaining(length) || !ReserveDecodedBytes(length))
            {
                Invalidate();
                return {};
            }
            std::string result(reinterpret_cast<const char*>(m_data + m_pos), length);
            m_pos += length;
            return result;
        }

        /** @brief Read raw bytes into an output buffer. */
        bool ReadBytes(void* dst, size_t count)
        {
            if (count == 0)
                return m_valid;
            if (!dst || !HasRemaining(count))
            {
                Invalidate();
                return false;
            }
            std::memcpy(dst, m_data + m_pos, count);
            m_pos += count;
            return true;
        }

        /** @brief Skip ahead by count bytes. */
        bool Skip(size_t count)
        {
            if (!HasRemaining(count))
            {
                Invalidate();
                return false;
            }
            m_pos += count;
            return true;
        }

        /** @brief Seek to an absolute position. */
        bool Seek(size_t pos)
        {
            if (pos > m_limit)
            {
                Invalidate();
                return false;
            }
            m_pos = pos;
            return true;
        }

        bool SetLimit(size_t limit)
        {
            if (!m_valid || limit < m_pos || limit > m_size)
            {
                Invalidate();
                return false;
            }
            m_limit = limit;
            return true;
        }

        bool ReserveDecodedBytes(size_t count)
        {
            constexpr size_t kMaxDecodedBytes = 128ull * 1024ull * 1024ull;
            if (count > kMaxDecodedBytes - m_decodedBytes)
            {
                Invalidate();
                return false;
            }
            m_decodedBytes += count;
            return true;
        }

        bool ReserveProperties(size_t count)
        {
            constexpr size_t kMaxProperties = 250'000;
            if (count > kMaxProperties - m_propertyCount)
            {
                Invalidate();
                return false;
            }
            m_propertyCount += count;
            return true;
        }

        void Invalidate()
        {
            m_valid = false;
            m_pos = m_size;
        }

        [[nodiscard]] size_t Position() const { return m_pos; }
        [[nodiscard]] size_t Size() const { return m_size; }
        [[nodiscard]] bool HasRemaining(size_t bytes = 1) const
        {
            return m_valid && m_pos <= m_limit && bytes <= m_limit - m_pos;
        }
        [[nodiscard]] bool IsValid() const { return m_valid; }
        [[nodiscard]] const uint8_t* Data() const { return m_data; }
        [[nodiscard]] size_t Limit() const { return m_limit; }

      private:
        const uint8_t* m_data = nullptr;
        size_t m_size = 0;
        size_t m_pos = 0;
        size_t m_limit = m_size;
        size_t m_decodedBytes = 0;
        size_t m_propertyCount = 0;
        bool m_valid = true;
    };

    // ============================================================================
    // FBX node representation (in-memory parse tree)
    // ============================================================================

    /** @brief A single property value from an FBX node. */
    struct FBXProperty
    {
        char type = 0;
        // Scalar values
        int32_t intValue = 0;
        int64_t longValue = 0;
        float floatValue = 0.0f;
        double doubleValue = 0.0;
        std::string stringValue;
        // Array values
        std::vector<float> floatArray;
        std::vector<double> doubleArray;
        std::vector<int32_t> intArray;
        std::vector<int64_t> longArray;
        std::vector<uint8_t> rawData;
    };

    /** @brief A node in the FBX document tree. */
    struct FBXNode
    {
        std::string name;
        std::vector<FBXProperty> properties;
        std::vector<FBXNode> children;

        /** @brief Find the first child with the given name, or nullptr. */
        const FBXNode* FindChild(const std::string& childName) const
        {
            for (const auto& c : children)
            {
                if (c.name == childName)
                    return &c;
            }
            return nullptr;
        }
    };

    // ============================================================================
    // FBXImporter — singleton
    // ============================================================================

    /**
     * @class FBXImporter
     * @brief Parses FBX binary files into mesh, skeleton, and animation data.
     *
     * No external FBX SDK dependency. Reads the binary node tree directly and
     * extracts Geometry, Deformer, and AnimationStack nodes.
     */
    class FBXImporter
    {
      public:
        /** @brief Get the singleton instance. */
        static FBXImporter& GetInstance()
        {
            static FBXImporter s;
            return s;
        }

        /**
         * @brief Import an FBX file from disk.
         * @param filePath Path to the .fbx file.
         * @param options Import configuration.
         * @return Import result with meshes, bones, animations, and warnings.
         */
        FBXImportResult Import(const std::string& filePath, const FBXImportOptions& options = {});

        /**
         * @brief Import FBX data already loaded into memory.
         * @param data Pointer to the raw FBX bytes.
         * @param size Size of the data buffer in bytes.
         * @param options Import configuration.
         * @return Import result.
         */
        FBXImportResult ImportFromMemory(const uint8_t* data, size_t size, const FBXImportOptions& options = {});

        /**
         * @brief Validate that imported FBX data satisfies pipeline expectations.
         * @param result Import result to validate.
         * @param requireSkeleton True to require at least one bone.
         * @param requireAnimation True to require at least one valid animation clip.
         * @param outError Optional output error text on validation failure.
         * @return True when validation passes.
         */
        bool ValidateForPipeline(const FBXImportResult& result, bool requireSkeleton = false,
                                 bool requireAnimation = false, std::string* outError = nullptr) const;

        /**
         * @brief Check if a file can be imported by inspecting magic bytes.
         * @param filePath Path to the file.
         * @return True if the file begins with the FBX binary magic string.
         */
        bool CanImport(const std::string& filePath) const;

        /**
         * @brief Check if raw data starts with the FBX binary magic.
         * @param data Pointer to the data.
         * @param size Size of the data buffer.
         * @return True if magic bytes match.
         */
        bool CanImportFromMemory(const uint8_t* data, size_t size) const;

        /**
         * @brief Return supported file extensions.
         * @return Vector containing ".fbx".
         */
        std::vector<std::string> GetSupportedExtensions() const { return {".fbx"}; }

        /** @brief Get a status string for console display. */
        std::string Console_GetStatus() const;

      private:
        FBXImporter() = default;
        ~FBXImporter() = default;
        FBXImporter(const FBXImporter&) = delete;
        FBXImporter& operator=(const FBXImporter&) = delete;

        // -- Binary parsing --
        bool ParseHeader(FBXBinaryReader& reader, uint32_t& outVersion) const;
        FBXNode ParseNode(FBXBinaryReader& reader, uint32_t version, size_t parentEnd, uint32_t depth,
                          uint32_t& nodeCount) const;
        FBXProperty ParseProperty(FBXBinaryReader& reader) const;
        std::vector<FBXNode> ParseNodeTree(FBXBinaryReader& reader, uint32_t version) const;

        // -- Data extraction --
        void ExtractGeometry(const std::vector<FBXNode>& nodes, FBXImportResult& result,
                             const FBXImportOptions& options, size_t& totalVertexFloats, size_t& totalIndices) const;
        void ExtractSkeleton(const std::vector<FBXNode>& nodes, FBXImportResult& result) const;
        void ExtractAnimations(const std::vector<FBXNode>& nodes, FBXImportResult& result) const;

        // -- Geometry helpers --
        bool TriangulatePolygon(const std::vector<uint32_t>& polygon, std::vector<uint32_t>& outIndices) const;
        void ConvertCoordinateSystem(FBXMeshData& mesh) const;
        void ApplyScale(FBXMeshData& mesh, float scale) const;
        void FlipUVs(FBXMeshData& mesh) const;

        uint32_t m_filesImported = 0;
        uint32_t m_totalMeshes = 0;
        uint32_t m_totalVertices = 0;
    };

} // namespace Spark::Graphics
