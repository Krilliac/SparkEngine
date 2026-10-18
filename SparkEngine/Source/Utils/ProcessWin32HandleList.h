/**
 * @file ProcessWin32HandleList.h
 * @brief Restricts Win32 CreateProcess handle inheritance to an explicit list.
 *
 * CreateProcess(..., bInheritHandles = TRUE, ...) hands the child EVERY handle
 * that is inheritable in the launching process at that instant — not only the
 * child's standard handles. With several launchers running on different
 * threads (Process::Builder, the editor build pipeline, the external console
 * integration), one child could receive another launch's pipe ends: an
 * unrelated long-lived child then holds a pipe write end open, and the owning
 * reader never sees EOF. PROC_THREAD_ATTRIBUTE_HANDLE_LIST limits inheritance
 * to the handles named here, whatever else is inheritable at the time.
 *
 * Contract:
 * - Thread affinity: none; each instance belongs to one launch on one thread.
 * - Ownership: never owns the listed handles; owns only the attribute list,
 *   which it deletes in the destructor.
 * - Allocation: one small heap block for the attribute list per launch
 *   (process creation is not a hot path).
 */

#pragma once

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

#include <windows.h>

namespace Spark::ProcessDetail
{

    class InheritedHandleList
    {
      public:
        InheritedHandleList() = default;
        ~InheritedHandleList()
        {
            if (m_attributesInitialized)
                DeleteProcThreadAttributeList(Attributes());
        }

        InheritedHandleList(const InheritedHandleList&) = delete;
        InheritedHandleList& operator=(const InheritedHandleList&) = delete;

        /**
         * @brief Mark @p handle inheritable and list it for the child.
         * @return false if the handle could not be made inheritable; the launch
         *         must then fail rather than start a child without its stdio.
         */
        [[nodiscard]] bool AddInheritable(HANDLE handle)
        {
            if (!IsRealHandle(handle))
                return false;
            if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                return false;
            Append(handle);
            return true;
        }

        /**
         * @brief List @p handle only if it is already inheritable.
         *
         * Used for the launcher's own standard handles that an uncaptured child
         * stream reuses: they reach the child exactly when they did before the
         * list existed, and a non-inheritable handle in the list would make
         * CreateProcess fail with ERROR_INVALID_PARAMETER.
         */
        void AddIfInheritable(HANDLE handle)
        {
            if (!IsRealHandle(handle))
                return;
            DWORD flags = 0;
            if (GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT) != 0)
                Append(handle);
        }

        /// Whether CreateProcess should be asked to inherit anything at all.
        [[nodiscard]] bool Empty() const noexcept { return m_handles.empty(); }

        /// The listed handles, for tests and diagnostics.
        [[nodiscard]] const std::vector<HANDLE>& Handles() const noexcept { return m_handles; }

        /**
         * @brief Build the attribute list naming exactly the listed handles.
         *
         * Call once, after every Add*(), and only when !Empty().
         * @return ERROR_SUCCESS, or the Win32 error that prevented building it.
         */
        [[nodiscard]] DWORD Build() noexcept
        {
            if (m_handles.empty() || m_attributesInitialized)
                return ERROR_INVALID_PARAMETER;

            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            if (size == 0)
                return GetLastError();

            m_storage.reset(new (std::nothrow) std::byte[size]);
            if (!m_storage)
                return ERROR_NOT_ENOUGH_MEMORY;
            if (!InitializeProcThreadAttributeList(Attributes(), 1, 0, &size))
                return GetLastError();
            m_attributesInitialized = true;

            if (!UpdateProcThreadAttribute(Attributes(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, m_handles.data(),
                                           m_handles.size() * sizeof(HANDLE), nullptr, nullptr))
                return GetLastError();
            return ERROR_SUCCESS;
        }

        /// The built attribute list for STARTUPINFOEX::lpAttributeList.
        [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Attributes() const noexcept
        {
            return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_storage.get());
        }

      private:
        static bool IsRealHandle(HANDLE handle) noexcept { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }

        void Append(HANDLE handle)
        {
            // A handle listed twice (merged stdout/stderr) fails the attribute.
            if (std::find(m_handles.begin(), m_handles.end(), handle) == m_handles.end())
                m_handles.push_back(handle);
        }

        std::vector<HANDLE> m_handles;
        std::unique_ptr<std::byte[]> m_storage;
        bool m_attributesInitialized = false;
    };

} // namespace Spark::ProcessDetail

#endif // SPARK_PLATFORM_WINDOWS
