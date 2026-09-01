// TestSparkError.cpp - Tests for SparkError utilities

#include "TestFramework.h"
#include "Utils/SparkError.h"

#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
#ifdef _WIN32
    int StderrDescriptor()
    {
        return _fileno(stderr);
    }
    int DuplicateDescriptor(int descriptor)
    {
        return _dup(descriptor);
    }
    int OpenNullDescriptor()
    {
        return _open("NUL", _O_WRONLY | _O_NOINHERIT);
    }
    bool RedirectDescriptor(int source, int destination)
    {
        return _dup2(source, destination) == 0;
    }
    void CloseDescriptor(int descriptor)
    {
        (void)_close(descriptor);
    }
    bool IsDescriptorWritable(int descriptor)
    {
        return _write(descriptor, "", 0) == 0;
    }
#else
    int StderrDescriptor()
    {
        return fileno(stderr);
    }
    int DuplicateDescriptor(int descriptor)
    {
        const int duplicate = dup(descriptor);
        if (duplicate >= 0)
        {
            const int flags = fcntl(duplicate, F_GETFD);
            if (flags >= 0)
                (void)fcntl(duplicate, F_SETFD, flags | FD_CLOEXEC);
        }
        return duplicate;
    }
    int OpenNullDescriptor()
    {
#ifdef O_CLOEXEC
        return open("/dev/null", O_WRONLY | O_CLOEXEC);
#else
        return open("/dev/null", O_WRONLY);
#endif
    }
    bool RedirectDescriptor(int source, int destination)
    {
        return dup2(source, destination) == destination;
    }
    void CloseDescriptor(int descriptor)
    {
        (void)close(descriptor);
    }
    bool IsDescriptorWritable(int descriptor)
    {
        return write(descriptor, "", 0) == 0;
    }
#endif

    class ScopedStderrSilencer
    {
      public:
        ScopedStderrSilencer()
        {
            (void)std::fflush(stderr);
            m_destination = StderrDescriptor();
            if (m_destination < 0)
                return;

            m_saved = DuplicateDescriptor(m_destination);
            if (m_saved < 0)
                return;

            const int nullDescriptor = OpenNullDescriptor();
            if (nullDescriptor < 0)
            {
                CloseDescriptor(m_saved);
                m_saved = -1;
                return;
            }

            m_active = RedirectDescriptor(nullDescriptor, m_destination);
            CloseDescriptor(nullDescriptor);
            if (!m_active)
            {
                CloseDescriptor(m_saved);
                m_saved = -1;
            }
        }

        ~ScopedStderrSilencer() noexcept
        {
            if (m_active)
            {
                (void)std::fflush(stderr);
                (void)RedirectDescriptor(m_saved, m_destination);
                CloseDescriptor(m_saved);
                std::clearerr(stderr);
            }
        }

        ScopedStderrSilencer(const ScopedStderrSilencer&) = delete;
        ScopedStderrSilencer& operator=(const ScopedStderrSilencer&) = delete;

        [[nodiscard]] bool IsActive() const noexcept { return m_active; }

        bool Restore() noexcept
        {
            if (!m_active)
                return m_saved < 0;

            (void)std::fflush(stderr);
            if (!RedirectDescriptor(m_saved, m_destination))
                return false;

            CloseDescriptor(m_saved);
            m_saved = -1;
            m_active = false;
            std::clearerr(stderr);
            return true;
        }

      private:
        int m_destination = -1;
        int m_saved = -1;
        bool m_active = false;
    };

    void ExpectStderrRestored(ScopedStderrSilencer& silencer)
    {
        ASSERT_TRUE(silencer.Restore());
        EXPECT_TRUE(IsDescriptorWritable(StderrDescriptor()));
        EXPECT_FALSE(std::ferror(stderr));
    }
} // namespace

TEST(SparkError_SeverityToString)
{
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Trace)), std::string("TRACE"));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Debug)), std::string("DEBUG"));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Info)), std::string("INFO "));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Warn)), std::string("WARN "));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Error)), std::string("ERROR"));
    EXPECT_EQ(std::string(SparkError::SeverityToString(SparkError::Severity::Fatal)), std::string("FATAL"));
}

TEST(SparkError_CheckFailedReturnsFalse)
{
    ScopedStderrSilencer silencer;
    ASSERT_TRUE(silencer.IsActive());

    bool result = SparkError::CheckFailed("false", __FILE__, __LINE__, __FUNCTION__);

    EXPECT_FALSE(result);
    ExpectStderrRestored(silencer);
}

TEST(SparkError_CheckFailedWithMsg)
{
    ScopedStderrSilencer silencer;
    ASSERT_TRUE(silencer.IsActive());

    bool result = SparkError::CheckFailed("expr", __FILE__, __LINE__, __FUNCTION__, "custom message");

    EXPECT_FALSE(result);
    ExpectStderrRestored(silencer);
}

TEST(SparkError_BoundsCheckFailed)
{
    ScopedStderrSilencer silencer;
    ASSERT_TRUE(silencer.IsActive());

    bool result = SparkError::BoundsCheckFailed("idx", 10, 5, __FILE__, __LINE__, __FUNCTION__);

    EXPECT_FALSE(result);
    ExpectStderrRestored(silencer);
}

TEST(SparkError_LogMessageDoesNotCrash)
{
    EXPECT_NO_THROW(SparkError::LogMessage(SparkError::Severity::Info, "Test", __FILE__, __LINE__, __FUNCTION__,
                                           "Test message %d", 42));
}

TEST(SparkError_ScopedContextDoesNotCrash)
{
    EXPECT_NO_THROW({
        SparkError::ScopedContext ctx("TestContext", __FILE__, __LINE__);
        // Context logs on enter and exit
    });
}
