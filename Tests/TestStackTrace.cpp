// TestStackTrace.cpp - Tests for portable stack trace capture

#include "TestFramework.h"
#include "Utils/StackTrace.h"

// =============================================================================
// Basic Capture
// =============================================================================

TEST(StackTrace_CaptureReturnsFrames)
{
    auto trace = Spark::StackTrace::Capture();
    // Should capture at least 1 frame (this function)
    EXPECT_GT(trace.GetFrameCount(), 0);
}

TEST(StackTrace_CaptureFromNestedCall)
{
    // Capture from a deeper call stack
    auto outerTrace = Spark::StackTrace::Capture(0);

    auto lambda = []() { return Spark::StackTrace::Capture(0); };
    auto innerTrace = lambda();

    // Inner trace should have at least as many frames (plus the lambda)
    EXPECT_GT(innerTrace.GetFrameCount(), 0);
    EXPECT_GT(outerTrace.GetFrameCount(), 0);
}

// =============================================================================
// Frame Data
// =============================================================================

TEST(StackTrace_FramesHaveAddresses)
{
    auto trace = Spark::StackTrace::Capture();
    const auto& frames = trace.GetFrames();

    for (const auto& frame : frames)
    {
        // Each frame should have a non-zero address
        EXPECT_NE(frame.address, static_cast<uintptr_t>(0));
    }
}

TEST(StackTrace_FrameToString)
{
    Spark::StackFrame frame;
    frame.address = 0xDEADBEEF;
    frame.functionName = "TestFunction";
    frame.displacement = 0x10;
    frame.fileName = "test.cpp";
    frame.lineNumber = 42;
    frame.moduleName = "TestModule";

    std::string str = frame.ToString();
    EXPECT_STR_CONTAINS(str, "deadbeef");
    EXPECT_STR_CONTAINS(str, "TestFunction");
    EXPECT_STR_CONTAINS(str, "test.cpp");
    EXPECT_STR_CONTAINS(str, "42");
    EXPECT_STR_CONTAINS(str, "TestModule");
}

TEST(StackTrace_FrameToStringMinimal)
{
    Spark::StackFrame frame;
    frame.address = 0x1234;
    // No function name, no file, no module
    std::string str = frame.ToString();
    EXPECT_STR_CONTAINS(str, "1234");
    EXPECT_STR_CONTAINS(str, "<unknown>");
}

// =============================================================================
// ToString Formatting
// =============================================================================

TEST(StackTrace_ToStringFormatted)
{
    auto trace = Spark::StackTrace::Capture();
    std::string output = trace.ToString();

    // Output should have frame markers
    EXPECT_STR_CONTAINS(output, "#0");
    // Should have indentation
    EXPECT_STR_CONTAINS(output, "  ");
}

TEST(StackTrace_ToStringCustomIndent)
{
    auto trace = Spark::StackTrace::Capture();
    std::string output = trace.ToString("    "); // 4-space indent

    EXPECT_STR_CONTAINS(output, "    #0");
}

// =============================================================================
// ToCompactString
// =============================================================================

TEST(StackTrace_ToCompactString)
{
    auto trace = Spark::StackTrace::Capture();
    std::string compact = trace.ToCompactString(3);

    // Should be a single-line format with arrows
    EXPECT_FALSE(compact.empty());
    // If there are more than 3 frames, should show "more" indicator
    if (trace.GetFrameCount() > 3)
    {
        EXPECT_STR_CONTAINS(compact, "more");
    }
}

TEST(StackTrace_ToCompactStringTopN)
{
    auto trace = Spark::StackTrace::Capture(0, 10);
    std::string compact1 = trace.ToCompactString(1);
    std::string compact5 = trace.ToCompactString(5);

    // More frames requested should produce longer or equal string
    EXPECT_FALSE(compact1.empty());
    if (trace.GetFrameCount() > 1)
    {
        EXPECT_GE(compact5.size(), compact1.size());
    }
}

// =============================================================================
// Skip Frames
// =============================================================================

TEST(StackTrace_SkipFrames)
{
    auto trace0 = Spark::StackTrace::Capture(0);
    auto trace3 = Spark::StackTrace::Capture(3);

    // Skipping more frames should result in fewer or equal frames
    EXPECT_GE(trace0.GetFrameCount(), trace3.GetFrameCount());
}

TEST(StackTrace_SkipAllFrames)
{
    // Skip more frames than likely exist in the call stack
    auto trace = Spark::StackTrace::Capture(60);
    EXPECT_EQ(trace.GetFrameCount(), 0);
}

// =============================================================================
// Max Frames
// =============================================================================

TEST(StackTrace_MaxFramesLimit)
{
    auto trace = Spark::StackTrace::Capture(0, 3);
    EXPECT_LE(trace.GetFrameCount(), 3);
}

TEST(StackTrace_MaxFramesCapsAtConstant)
{
    auto trace = Spark::StackTrace::Capture(0, Spark::StackTrace::MAX_FRAMES);
    EXPECT_LE(trace.GetFrameCount(), Spark::StackTrace::MAX_FRAMES);
}

// =============================================================================
// GetFrames Access
// =============================================================================

TEST(StackTrace_GetFramesReturnsSameAsCount)
{
    auto trace = Spark::StackTrace::Capture();
    EXPECT_EQ(static_cast<int>(trace.GetFrames().size()), trace.GetFrameCount());
}

// =============================================================================
// Empty Trace Handling
// =============================================================================

TEST(StackTrace_EmptyTraceToString)
{
    auto trace = Spark::StackTrace::Capture(60); // skip more than exist
    std::string output = trace.ToString();
    // Should be empty or minimal
    EXPECT_EQ(trace.GetFrameCount(), 0);
}

TEST(StackTrace_EmptyTraceCompactString)
{
    auto trace = Spark::StackTrace::Capture(60);
    std::string compact = trace.ToCompactString();
    // Should not crash on empty trace
    EXPECT_EQ(compact.size(), static_cast<size_t>(0));
}
