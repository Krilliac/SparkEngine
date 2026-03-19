/**
 * @file RenderGraphExporter.h
 * @brief Exports render graph as GraphViz .dot file for debugging
 *
 * Inspired by Cocos Engine's FrameGraph::exportGraphViz(). Dumps the render
 * pass dependency graph as a .dot file that can be visualized with GraphViz.
 */

#pragma once

#include <string>
#include <vector>

namespace Spark::Graphics
{

    /// @brief Description of a render pass for export
    struct RenderPassInfo
    {
        std::string name;                ///< Pass name (e.g., "ShadowPass", "GBuffer")
        std::vector<std::string> reads;  ///< Resources this pass reads
        std::vector<std::string> writes; ///< Resources this pass writes/creates
    };

    /// @brief Static utility for exporting render graphs to GraphViz format
    class RenderGraphExporter
    {
      public:
        /// @brief Export render graph as a .dot file
        /// @param passes List of render passes with their resource dependencies
        /// @param outputPath File path for the .dot output
        /// @return true if file was written successfully
        static bool ExportGraphViz(const std::vector<RenderPassInfo>& passes, const std::string& outputPath);

        /// @brief Generate GraphViz DOT string without writing to file
        static std::string GenerateDotString(const std::vector<RenderPassInfo>& passes);
    };

} // namespace Spark::Graphics
