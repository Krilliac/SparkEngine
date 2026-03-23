/**
 * @file SceneSerializer.h
 * @brief Scene file serialization and deserialization system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides functionality for saving and loading scene files
 * in both binary and JSON formats, with version compatibility and
 * error handling.
 */

#pragma once

#include "SceneFile.h"
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace SparkEditor
{

    /**
 * @brief Scene serialization format options
 */
    enum class SerializationFormat
    {
        BINARY, ///< Compact binary format for production
        JSON,   ///< Human-readable JSON format for debugging/version control
        AUTO    ///< Automatically detect format from file extension
    };

    /**
 * @brief Scene serialization result
 */
    struct SerializationResult
    {
        bool success = false;              ///< Whether operation succeeded
        std::string errorMessage;          ///< Error description if failed
        std::vector<std::string> warnings; ///< Non-fatal warnings
        size_t bytesProcessed = 0;         ///< Number of bytes read/written
        float processingTime = 0.0f;       ///< Time taken in seconds
    };

    /**
 * @brief Scene file serialization and deserialization system
 * 
 * Handles saving and loading of scene files with support for both binary
 * and JSON formats. Provides version compatibility, validation, and
 * comprehensive error handling.
 * 
 * Features:
 * - Binary format for production (fast loading, compact)
 * - JSON format for debugging and version control (human-readable)
 * - Automatic format detection
 * - Version compatibility handling
 * - Data validation and integrity checks
 * - Asset dependency tracking
 * - Error reporting and recovery
 */
    class SceneSerializer
    {
      public:
        /**
     * @brief Constructor
     */
        SceneSerializer();

        /**
     * @brief Destructor
     */
        ~SceneSerializer();

        /**
     * @brief Save scene to file
     * 
     * Serializes a scene to the specified file path using the chosen format.
     * Automatically creates directories if they don't exist.
     * 
     * @param scene Scene data to save
     * @param filePath Output file path
     * @param format Serialization format to use
     * @return Serialization result with success status and details
     */
        SerializationResult SaveScene(const SceneFile& scene, const std::string& filePath,
                                      SerializationFormat format = SerializationFormat::AUTO);

        /**
     * @brief Load scene from file
     * 
     * Deserializes a scene from the specified file path. Automatically
     * detects file format and handles version compatibility.
     * 
     * @param filePath Input file path
     * @param outScene Output scene data
     * @return Serialization result with success status and details
     */
        SerializationResult LoadScene(const std::string& filePath, SceneFile& outScene);

        /**
     * @brief Validate scene file without loading
     * 
     * Performs a quick validation of the scene file to check for
     * corruption, version compatibility, and basic structural integrity.
     * 
     * @param filePath Scene file to validate
     * @return Validation result
     */
        SerializationResult ValidateSceneFile(const std::string& filePath);

        /**
     * @brief Export scene to different format
     * 
     * Converts a scene file from one format to another, useful for
     * format migration or debugging purposes.
     * 
     * @param inputPath Input scene file path
     * @param outputPath Output scene file path
     * @param outputFormat Target format for conversion
     * @return Conversion result
     */
        SerializationResult ConvertSceneFormat(const std::string& inputPath, const std::string& outputPath,
                                               SerializationFormat outputFormat);

        /**
     * @brief Get supported file extensions
     * @param format Format to get extensions for
     * @return Vector of file extensions (including the dot)
     */
        static std::vector<std::string> GetSupportedExtensions(SerializationFormat format);

        /**
     * @brief Detect format from file extension
     * @param filePath File path to analyze
     * @return Detected format, or AUTO if unknown
     */
        static SerializationFormat DetectFormat(const std::string& filePath);

        /**
     * @brief Check if file is a valid scene file
     * @param filePath File to check
     * @return true if file appears to be a valid scene file
     */
        static bool IsSceneFile(const std::string& filePath);

        /**
     * @brief Set compression level for binary format
     * @param level Compression level (0-9, 0=no compression, 9=maximum)
     */
        void SetCompressionLevel(int level) { m_compressionLevel = level; }

        /**
     * @brief Get current compression level
     * @return Current compression level
     */
        int GetCompressionLevel() const { return m_compressionLevel; }

        /**
     * @brief Enable/disable pretty printing for JSON format
     * @param enabled Whether to format JSON with indentation
     */
        void SetPrettyPrintJSON(bool enabled) { m_prettyPrintJSON = enabled; }

        /**
     * @brief Check if pretty printing is enabled
     * @return true if pretty printing is enabled
     */
        bool IsPrettyPrintEnabled() const { return m_prettyPrintJSON; }

      private:
        /**
     * @brief Save scene in binary format
     * @param scene Scene to save
     * @param filePath Output file path
     * @return Serialization result
     */
        SerializationResult SaveBinary(const SceneFile& scene, const std::string& filePath);

        /**
     * @brief Save scene in JSON format
     * @param scene Scene to save
     * @param filePath Output file path
     * @return Serialization result
     */
        SerializationResult SaveJSON(const SceneFile& scene, const std::string& filePath);

        /**
     * @brief Load scene from binary format
     * @param filePath Input file path
     * @param outScene Output scene data
     * @return Serialization result
     */
        SerializationResult LoadBinary(const std::string& filePath, SceneFile& outScene);

        /**
     * @brief Load scene from JSON format
     * @param filePath Input file path
     * @param outScene Output scene data
     * @return Serialization result
     */
        SerializationResult LoadJSON(const std::string& filePath, SceneFile& outScene);

        /**
     * @brief Serialize transform component to binary
     * @param transform Transform data
     * @param buffer Output buffer
     */
        void SerializeTransform(const Transform& transform, std::vector<uint8_t>& buffer);

        /**
     * @brief Deserialize transform component from binary
     * @param buffer Input buffer
     * @param offset Current read offset (updated)
     * @param transform Output transform data
     * @return true if successful
     */
        bool DeserializeTransform(const std::vector<uint8_t>& buffer, size_t& offset, Transform& transform);

        /**
     * @brief Serialize component to binary
     * @param component Component data
     * @param buffer Output buffer
     */
        void SerializeComponent(const Component& component, std::vector<uint8_t>& buffer);

        /**
     * @brief Deserialize component from binary
     * @param buffer Input buffer
     * @param offset Current read offset (updated)
     * @param component Output component data
     * @return true if successful
     */
        bool DeserializeComponent(const std::vector<uint8_t>& buffer, size_t& offset, Component& component);

        /**
     * @brief Convert transform to JSON object
     * @param transform Transform data
     * @return JSON object (implementation-specific)
     */
        void* TransformToJSON(const Transform& transform);

        /**
     * @brief Convert JSON object to transform
     * @param json JSON object
     * @param transform Output transform data
     * @return true if successful
     */
        bool JSONToTransform(void* json, Transform& transform);

        /**
     * @brief Convert component to JSON object
     * @param component Component data
     * @return JSON object (implementation-specific)
     */
        void* ComponentToJSON(const Component& component);

        /**
     * @brief Convert JSON object to component
     * @param json JSON object
     * @param component Output component data
     * @return true if successful
     */
        bool JSONToComponent(void* json, Component& component);

        /**
     * @brief Validate scene data integrity
     * @param scene Scene to validate
     * @param result Result to append warnings/errors to
     * @return true if scene is valid
     */
        bool ValidateScene(const SceneFile& scene, SerializationResult& result);

        /**
     * @brief Handle version compatibility
     * @param fileVersion Version of loaded file
     * @param scene Scene data to potentially upgrade
     * @param result Result to append warnings to
     * @return true if compatibility was handled successfully
     */
        bool HandleVersionCompatibility(uint32_t fileVersion, SceneFile& scene, SerializationResult& result);

        /**
     * @brief Create backup of scene file before overwriting
     * @param filePath File to backup
     * @return true if backup was created successfully
     */
        bool CreateBackup(const std::string& filePath);

        /**
     * @brief Write data to file with error handling
     * @param filePath Output file path
     * @param data Data to write
     * @return true if write was successful
     */
        bool WriteToFile(const std::string& filePath, const std::vector<uint8_t>& data);

        /**
     * @brief Read data from file with error handling
     * @param filePath Input file path
     * @param data Output data buffer
     * @return true if read was successful
     */
        bool ReadFromFile(const std::string& filePath, std::vector<uint8_t>& data);

        /**
     * @brief Compress data using configured compression level
     * @param input Input data
     * @param output Output compressed data
     * @return true if compression was successful
     */
        bool CompressData(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

        /**
     * @brief Decompress data
     * @param input Compressed input data
     * @param output Output decompressed data
     * @return true if decompression was successful
     */
        bool DecompressData(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

      private:
        int m_compressionLevel = 6;               ///< Compression level for binary format
        bool m_prettyPrintJSON = true;            ///< Pretty print JSON format
        bool m_createBackups = true;              ///< Create backup files before overwriting
        size_t m_maxFileSize = 1024 * 1024 * 100; ///< Maximum allowed file size (100MB)

        // Performance tracking
        mutable size_t m_totalBytesRead = 0;        ///< Total bytes read during session
        mutable size_t m_totalBytesWritten = 0;     ///< Total bytes written during session
        mutable float m_totalProcessingTime = 0.0f; ///< Total processing time during session
    };

} // namespace SparkEditor