/**
 * @file BuiltinWorkflows.h
 * @brief Pre-built editor workflow templates
 *
 * Registers commonly used workflow sequences into the WorkflowRegistry
 * so they appear in the WorkflowPanel immediately.
 */

#pragma once

namespace SparkEditor
{

    /// @brief Register all built-in workflows. Call once during editor init.
    void RegisterBuiltinWorkflows();

} // namespace SparkEditor
