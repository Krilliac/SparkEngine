/**
 * @file RenderGraphExporter.cpp
 * @brief GraphViz .dot export for render graph visualization
 */

#include "RenderGraphExporter.h"

#include <fstream>
#include <set>
#include <sstream>

namespace Spark::Graphics
{

    std::string RenderGraphExporter::GenerateDotString(const std::vector<RenderPassInfo>& passes)
    {
        std::ostringstream dot;
        dot << "digraph RenderGraph {\n";
        dot << "    rankdir=LR;\n";
        dot << "    node [shape=box, style=filled];\n\n";

        // Collect all unique resources
        std::set<std::string> allResources;
        for (const auto& pass : passes)
        {
            for (const auto& r : pass.reads)
                allResources.insert(r);
            for (const auto& w : pass.writes)
                allResources.insert(w);
        }

        // Render pass nodes (blue boxes)
        dot << "    // Render passes\n";
        for (const auto& pass : passes)
        {
            dot << "    \"" << pass.name << "\" [fillcolor=\"#4A90D9\", fontcolor=white];\n";
        }
        dot << "\n";

        // Resource nodes (green ellipses)
        dot << "    // Resources\n";
        for (const auto& res : allResources)
        {
            dot << "    \"" << res << "\" [shape=ellipse, fillcolor=\"#7EC850\", fontcolor=white];\n";
        }
        dot << "\n";

        // Edges: resource -> pass (reads), pass -> resource (writes)
        dot << "    // Dependencies\n";
        for (const auto& pass : passes)
        {
            for (const auto& r : pass.reads)
            {
                dot << "    \"" << r << "\" -> \"" << pass.name << "\" [color=\"#888888\"];\n";
            }
            for (const auto& w : pass.writes)
            {
                dot << "    \"" << pass.name << "\" -> \"" << w << "\" [color=\"#E74C3C\"];\n";
            }
        }

        dot << "}\n";
        return dot.str();
    }

    bool RenderGraphExporter::ExportGraphViz(const std::vector<RenderPassInfo>& passes, const std::string& outputPath)
    {
        std::string dotContent = GenerateDotString(passes);

        std::ofstream file(outputPath);
        if (!file.is_open())
            return false;

        file << dotContent;
        return file.good();
    }

} // namespace Spark::Graphics
