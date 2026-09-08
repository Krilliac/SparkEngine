/**
 * @file TerrainAssetFormat.h
 * @brief Canonical .sparkterrain format constants and bounded stream reader
 */

#pragma once

#include <cstdint>
#include <istream>
#include <string>

namespace Spark::Graphics
{

    /**
     * @brief Canonical on-disk layout of a .sparkterrain asset.
     *
     * This namespace is the single definition of the format shared by SparkEditor's writer
     * (TerrainEditor::SaveTerrain) and both readers (TerrainEditor::LoadTerrain and
     * TerrainRenderer::LoadSparkTerrain). Every field is a fixed-width type, so the editor and the runtime
     * cannot disagree about a field's width. Little-endian, no padding:
     *
     * ```
     * uint32  magic                       kMagic
     * uint32  version                     kVersion
     * string  name                        uint32 length + bytes
     * float   size
     * float   positionX, positionY, positionZ
     * int32   lodLevels
     * float   lodBias
     * uint8   generateCollider
     * int32   heightmapWidth, heightmapHeight
     * float   heightScale, minHeight, maxHeight
     * float[] heights                     heightmapWidth * heightmapHeight entries
     * uint32  layerCount
     *   per layer:
     *     string name, diffuseTexture, normalTexture, maskTexture
     *     float  tilingX, tilingY, offsetX, offsetY
     *     float  opacity, metallic, roughness, normalStrength
     * int32   splatmapResolution
     * uint8[] splatmap                    splatmapResolution^2 * 4 entries (RGBA weights)
     * uint32  detailMeshCount
     *   per detail mesh:
     *     string name, meshPath, materialPath
     *     float  density, viewDistance
     *     uint32 instanceCount
     *     float  instanceX, instanceY, instanceZ (repeated instanceCount times)
     * ```
     *
     * Version 1 files (which had no version word and stored only layer names) are not readable; the
     * editor-to-runtime path never worked for them, so no shipped asset depends on that layout.
     */
    namespace SparkTerrain
    {
        inline constexpr uint32_t kMagic = 0x53504B54u; ///< 'SPKT'
        inline constexpr uint32_t kVersion = 2u;

        /// Hard limits. A file declaring more than these is rejected rather than trusted for an allocation.
        inline constexpr uint32_t kMaxStringLength = 4096u;
        inline constexpr int32_t kMinHeightmapResolution = 2;
        inline constexpr int32_t kMaxHeightmapResolution = 8193;
        inline constexpr uint32_t kMaxTextureLayers = 64u;
        inline constexpr int32_t kMaxSplatmapResolution = 8192;
        inline constexpr uint32_t kMaxDetailMeshes = 256u;
        inline constexpr uint32_t kMaxDetailInstancesPerMesh = 4000000u;

        /**
         * @brief Bounds-checked cursor over a .sparkterrain stream.
         *
         * Every read is checked against the real remaining byte count before it is attempted, so a truncated
         * or hostile header can never size an allocation. A failed read leaves the reader permanently failed;
         * callers check Failed() once at the end rather than at every field.
         */
        class Reader
        {
          public:
            explicit Reader(std::istream& stream) : m_stream(stream)
            {
                m_stream.seekg(0, std::ios::end);
                const std::streamoff end = m_stream.tellg();
                m_size = end < 0 ? 0 : static_cast<uint64_t>(end);
                m_stream.seekg(0, std::ios::beg);
                m_failed = !m_stream.good();
            }

            /// @brief Bytes not yet consumed, or 0 once the reader has failed.
            uint64_t Remaining() const
            {
                if (m_failed)
                    return 0;
                return m_size >= m_offset ? m_size - m_offset : 0;
            }

            bool Failed() const { return m_failed; }

            bool ReadBytes(void* destination, uint64_t count)
            {
                if (m_failed || count > Remaining())
                {
                    m_failed = true;
                    return false;
                }
                if (count == 0)
                    return true;
                m_stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
                if (!m_stream.good())
                {
                    m_failed = true;
                    return false;
                }
                m_offset += count;
                return true;
            }

            bool ReadU8(uint8_t& out) { return ReadBytes(&out, sizeof(out)); }
            bool ReadU32(uint32_t& out) { return ReadBytes(&out, sizeof(out)); }
            bool ReadI32(int32_t& out) { return ReadBytes(&out, sizeof(out)); }
            bool ReadF32(float& out) { return ReadBytes(&out, sizeof(out)); }

            /// @brief Read a uint32 length prefix followed by that many bytes, capped at kMaxStringLength.
            bool ReadString(std::string& out)
            {
                uint32_t length = 0;
                if (!ReadU32(length))
                    return false;
                if (length > kMaxStringLength)
                {
                    m_failed = true;
                    return false;
                }
                out.assign(length, '\0');
                return length == 0 ? true : ReadBytes(out.data(), length);
            }

          private:
            std::istream& m_stream;
            uint64_t m_size = 0;
            uint64_t m_offset = 0;
            bool m_failed = false;
        };
    } // namespace SparkTerrain

} // namespace Spark::Graphics
