/**
 * @file DockPosition.h
 * @brief DockPosition enum definition for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

namespace SparkEditor
{

    /**
 * @brief Dock position enumeration for panel docking
 */
    enum class DockPosition
    {
        Left,
        Right,
        Top,
        Bottom,
        Center,
        Tab,
        Floating
    };

} // namespace SparkEditor