/**
 * @file VisualScriptGraphIO.h
 * @brief Engine-owned .vscript graph file format: parse, validate, serialize, load and save
 *
 * A .vscript file is UTF-8 JSON:
 *
 *     {
 *       "format": "spark.vscript",
 *       "version": 1,
 *       "className": "Collectible",
 *       "description": "...",
 *       "variables": [{"name": "speed", "type": "Float", "default": "2.0f"}],
 *       "nodes": [{"id": 1, "type": "OnStart", "position": [0, 0],
 *                  "inputs": [], "outputs": [{"kind": "Execution"}], "properties": {}}],
 *       "connections": [{"from": [1, 0], "to": [2, 0]}],
 *       "functions": [],
 *       "customEvents": []
 *     }
 *
 * Node types and pin kinds are stored by name, never by enum value. A pin's
 * "default" is a number (Float/Int/Entity), a bool, a string (String) or an
 * [x, y, z] array (Vector3), and is omitted when zero or empty.
 *
 * Parsing fails closed: an unknown or repeated key, format, version, node
 * type or pin kind, a duplicate, zero or out-of-range (above kMaxNodeId) node
 * id, a wire to a missing node or pin, an execution wire joined to a data pin,
 * incompatible data kinds, a second wire into one input, or a second wire out
 * of one execution output all reject the whole file with a message naming the
 * offending element. Serialization is canonical (one element per line, keys
 * in a fixed order, floats in shortest round-trip form), so
 * Serialize(Parse(file)) reproduces a canonical file byte for byte and graph
 * diffs stay reviewable.
 *
 * Contract: stateless free-standing functions, safe on any thread; editor and
 * tooling tier (never per frame); allocations are proportional to the file.
 *
 * @see VisualScriptCompiler.h for compiling a loaded graph to AngelScript
 */

#pragma once

#include "VisualScriptGraphTypes.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Spark::Scripting
{
    /**
     * @brief Reads and writes visual script graphs in the .vscript format
     */
    class VisualScriptGraphIO
    {
      public:
        static constexpr std::string_view kFormat = "spark.vscript"; ///< Value of the "format" key
        static constexpr int kVersion = 1;                           ///< Only supported "version"
        static constexpr size_t kMaxFileBytes = 8u * 1024u * 1024u;  ///< Larger files are rejected unread
        static constexpr uint32_t kMaxNodeId = (1u << 24) - 1u;      ///< Largest accepted node id (ids start at 1)

        /**
         * @brief Parse and validate .vscript text
         * @param text Complete file contents
         * @return The graph, or a message naming the first invalid element
         */
        static std::expected<VisualScriptGraph, std::string> Parse(std::string_view text);

        /**
         * @brief Serialize a graph to canonical .vscript text (ends with a newline)
         * @param graph Graph to write; its in-memory form is written as is (Save validates)
         */
        static std::string Serialize(const VisualScriptGraph& graph);

        /**
         * @brief Read and parse a .vscript file
         * @param path File to read
         * @return The graph, or a message prefixed with the file path
         */
        static std::expected<VisualScriptGraph, std::string> LoadFile(const std::filesystem::path& path);

        /**
         * @brief Validate and atomically write a graph (temporary file, then rename)
         *
         * The serialized text is parsed back before anything is written, so a
         * graph that could not be loaded again is never saved.
         * @param path Destination file
         * @param graph Graph to write
         * @return Nothing on success, or a message naming the failure
         */
        static std::expected<void, std::string> SaveFile(const std::filesystem::path& path,
                                                         const VisualScriptGraph& graph);

        /// Stable file name of a node type ("OnStart", "Branch", ...), or nullptr for an unknown value.
        static const char* NodeTypeName(ScriptNodeType type);

        /// Node type for a stable file name.
        static std::optional<ScriptNodeType> NodeTypeFromName(std::string_view name);

        /// Stable file name of a pin kind ("Execution", "Float", ...).
        static const char* PinKindName(PinKind kind);

        /// Pin kind for a stable file name.
        static std::optional<PinKind> PinKindFromName(std::string_view name);

        /**
         * @brief Whether an output pin of kind @p from may feed an input pin of kind @p to
         *
         * Execution only joins Execution. Data joins the same kind, Any, or Int/Float
         * (implicit numeric conversion).
         */
        static bool ArePinKindsCompatible(PinKind from, PinKind to);
    };

} // namespace Spark::Scripting
