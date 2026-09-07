/**
 * @file TestMSanCanary.cpp
 * @brief RED-capable proof that the MemorySanitizer lane is instrumented end to end.
 *
 * build-linux-msan links an MSan-instrumented libc++ built in-job. ldd/nm prove
 * which file was linked; this test proves that the runtime's stores update
 * shadow memory and that this translation unit is instrumented and not
 * ignorelisted. With an uninstrumented libc++.so.1 the std::string
 * representation written by the out-of-line basic_string::__init stays
 * poisoned and __msan_test_shadow returns >= 0; with the instrumented runtime
 * it returns -1. No uninitialised value is read here, so halt_on_error is
 * irrelevant. In every other build the tests explicitly skip: registration
 * alone is not evidence that MemorySanitizer instrumentation was verified.
 */

#include "TestFramework.h"

#include <memory>
#include <string>

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#define SPARK_TEST_MSAN_BUILD 1
#include <sanitizer/msan_interface.h>
#endif
#endif
#ifndef SPARK_TEST_MSAN_BUILD
#define SPARK_TEST_MSAN_BUILD 0
#endif

TEST(MSanCanary_TestTranslationUnitIsInstrumented)
{
#if SPARK_TEST_MSAN_BUILD
    // An instrumented store writes clean shadow for its destination. An
    // ignorelisted or uninstrumented TU leaves the explicit poison in place,
    // so __msan_test_shadow returns 0 instead of -1.
    volatile int probe = 0;
    __msan_poison(&probe, sizeof probe);
    probe = 1;
    EXPECT_EQ(__msan_test_shadow(&probe, sizeof probe), -1);
#else
    SKIP_TEST("MemorySanitizer instrumentation is unavailable in this build");
#endif
}

TEST(MSanCanary_LibcxxStoresUpdateShadow)
{
#if SPARK_TEST_MSAN_BUILD
    // Longer than libc++'s 22-byte SSO so basic_string::__init (extern
    // template, out-of-line in libc++.so.1) writes the long representation
    // (pointer, size, capacity) into the object from inside the runtime.
    static const char kText[] = "MSan canary: this literal exceeds the SSO buffer";
    std::string onStack(kText);
    EXPECT_EQ(__msan_test_shadow(&onStack, sizeof onStack), -1);
    EXPECT_EQ(__msan_test_shadow(onStack.data(), onStack.size()), -1);
    auto onHeap = std::make_unique<std::string>(kText);
    EXPECT_EQ(__msan_test_shadow(onHeap.get(), sizeof(std::string)), -1);
    EXPECT_EQ(__msan_test_shadow(onHeap->data(), onHeap->size()), -1);
#else
    SKIP_TEST("MemorySanitizer instrumentation is unavailable in this build");
#endif
}
