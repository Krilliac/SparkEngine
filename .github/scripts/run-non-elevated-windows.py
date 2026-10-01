"""Run one Windows command with a verified same-user medium-integrity token.

This module is intentionally a callable helper for package qualification.  It
never obtains another user's token and never falls back to an elevated launch.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
import subprocess
import sys
from pathlib import Path


CREATE_SUSPENDED = 0x00000004
CREATE_UNICODE_ENVIRONMENT = 0x00000400
DISABLE_MAX_PRIVILEGE = 0x1
LUA_TOKEN = 0x4
TOKEN_QUERY = 0x0008
TOKEN_DUPLICATE = 0x0002
TOKEN_ASSIGN_PRIMARY = 0x0001
TOKEN_ADJUST_DEFAULT = 0x0080
TOKEN_USER = 1
TOKEN_ELEVATION = 20
TOKEN_INTEGRITY_LEVEL = 25
TOKEN_INFORMATION_CLASS = wintypes.DWORD
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 0x102
HANDLE_FLAG_INHERIT = 0x00000001
MEDIUM_INTEGRITY_RID = 0x2000


class _SID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Sid", wintypes.LPVOID), ("Attributes", wintypes.DWORD)]


class _TOKEN_MANDATORY_LABEL(ctypes.Structure):
    _fields_ = [("Label", _SID_AND_ATTRIBUTES)]


class _SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("nLength", wintypes.DWORD), ("lpSecurityDescriptor", wintypes.LPVOID),
                ("bInheritHandle", wintypes.BOOL)]


class _STARTUPINFO(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
                ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
                ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD), ("dwXSize", wintypes.DWORD),
                ("dwYSize", wintypes.DWORD), ("dwXCountChars", wintypes.DWORD),
                ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("wShowWindow", wintypes.WORD),
                ("cbReserved2", wintypes.WORD), ("lpReserved2", wintypes.LPBYTE),
                ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE),
                ("hStdError", wintypes.HANDLE)]


class _PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]


def _api(name, library, restype, argtypes):
    function = getattr(ctypes.WinDLL(library, use_last_error=True), name)
    function.restype = restype
    function.argtypes = argtypes
    return function


def _require(condition, message):
    if not condition:
        error = ctypes.get_last_error()
        raise OSError(error, f"{message} (Win32 error {error})")


def _sid(text):
    convert = _api("ConvertStringSidToSidW", "advapi32", wintypes.BOOL, [wintypes.LPWSTR, ctypes.POINTER(wintypes.LPVOID)])
    value = wintypes.LPVOID()
    _require(convert(text, ctypes.byref(value)), f"cannot convert SID {text}")
    return value


def _close(handle):
    if handle:
        _api("CloseHandle", "kernel32", wintypes.BOOL, [wintypes.HANDLE])(handle)


def _token(handle, access=None):
    open_token = _api("OpenProcessToken", "advapi32", wintypes.BOOL,
                      [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)])
    value = wintypes.HANDLE()
    if access is None:
        access = TOKEN_QUERY
    _require(open_token(handle, access,
                        ctypes.byref(value)), "OpenProcessToken failed")
    return value


def _token_info(token, info_class, size=4096):
    get_info = _api("GetTokenInformation", "advapi32", wintypes.BOOL,
                    [wintypes.HANDLE, TOKEN_INFORMATION_CLASS, wintypes.LPVOID, wintypes.DWORD,
                     ctypes.POINTER(wintypes.DWORD)])
    buffer = ctypes.create_string_buffer(size)
    actual = wintypes.DWORD()
    _require(get_info(token, info_class, buffer, size, ctypes.byref(actual)),
             f"GetTokenInformation({info_class}) failed")
    return buffer, actual.value


def _token_is_medium_non_elevated(token):
    if _token_is_elevated(token):
        return False
    return _token_integrity_rid(token) == MEDIUM_INTEGRITY_RID


def _token_is_elevated(token):
    elevation, _ = _token_info(token, TOKEN_ELEVATION, ctypes.sizeof(wintypes.DWORD))
    return ctypes.cast(elevation, ctypes.POINTER(wintypes.DWORD)).contents.value != 0


def _token_integrity_rid(token):
    label, _ = _token_info(token, TOKEN_INTEGRITY_LEVEL, 256)
    sid_ptr = ctypes.cast(label, ctypes.POINTER(_TOKEN_MANDATORY_LABEL)).contents.Label.Sid
    count = _api("GetSidSubAuthorityCount", "advapi32", ctypes.POINTER(ctypes.c_ubyte), [wintypes.LPVOID])(sid_ptr)
    subauthority = _api("GetSidSubAuthority", "advapi32", ctypes.POINTER(wintypes.DWORD),
                       [wintypes.LPVOID, wintypes.DWORD])(sid_ptr, count.contents.value - 1)
    return subauthority.contents.value


def _same_user(first, second):
    first_info, _ = _token_info(first, TOKEN_USER)
    second_info, _ = _token_info(second, TOKEN_USER)
    first_sid = ctypes.cast(first_info, ctypes.POINTER(_SID_AND_ATTRIBUTES)).contents.Sid
    second_sid = ctypes.cast(second_info, ctypes.POINTER(_SID_AND_ATTRIBUTES)).contents.Sid
    equal = _api("EqualSid", "advapi32", wintypes.BOOL, [wintypes.LPVOID, wintypes.LPVOID])
    return bool(equal(first_sid, second_sid))


def _restricted_token(source):
    create = _api("CreateRestrictedToken", "advapi32", wintypes.BOOL,
                  [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(_SID_AND_ATTRIBUTES),
                   wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(_SID_AND_ATTRIBUTES),
                   ctypes.POINTER(wintypes.HANDLE)])
    admin_sid = _sid("S-1-5-32-544")
    disabled = _SID_AND_ATTRIBUTES(admin_sid, 0)
    result = wintypes.HANDLE()
    try:
        # LUA_TOKEN requests a limited-user token as well as stripping
        # privileges. Never use SANDBOX_INERT: policy checks remain active.
        _require(create(source, DISABLE_MAX_PRIVILEGE | LUA_TOKEN, 1, ctypes.byref(disabled), 0, None, 0, None,
                        ctypes.byref(result)), "CreateRestrictedToken failed")
    finally:
        _api("LocalFree", "kernel32", wintypes.LPVOID, [wintypes.LPVOID])(admin_sid)
    medium_sid = None
    try:
        medium_sid = _sid("S-1-16-8192")
        label = _TOKEN_MANDATORY_LABEL(_SID_AND_ATTRIBUTES(medium_sid, 0x20))
        set_info = _api("SetTokenInformation", "advapi32", wintypes.BOOL,
                        [wintypes.HANDLE, TOKEN_INFORMATION_CLASS, wintypes.LPVOID, wintypes.DWORD])
        get_length = _api("GetLengthSid", "advapi32", wintypes.DWORD, [wintypes.LPVOID])
        label_size = _mandatory_label_size(get_length(medium_sid))
        _require(set_info(result, TOKEN_INTEGRITY_LEVEL, ctypes.byref(label), label_size),
                 "SetTokenInformation(TokenIntegrityLevel) failed")
    except BaseException:
        _close(result)
        raise
    finally:
        if medium_sid:
            _api("LocalFree", "kernel32", wintypes.LPVOID, [wintypes.LPVOID])(medium_sid)
    return result


def _mandatory_label_size(sid_length):
    """Size used by the documented TOKEN_MANDATORY_LABEL example."""
    return ctypes.sizeof(_TOKEN_MANDATORY_LABEL) + sid_length


def _reap_child(handle, terminate=None, wait=None):
    terminate = terminate or _api("TerminateProcess", "kernel32", wintypes.BOOL,
                                  [wintypes.HANDLE, wintypes.UINT])
    wait = wait or _api("WaitForSingleObject", "kernel32", wintypes.DWORD,
                        [wintypes.HANDLE, wintypes.DWORD])
    initial = wait(handle, 0)
    if initial == WAIT_OBJECT_0:
        return
    _require(initial == WAIT_TIMEOUT, "cannot inspect child before cleanup")
    _require(terminate(handle, 1), "TerminateProcess failed while cleaning up child")
    _require(wait(handle, 5000) == WAIT_OBJECT_0, "child did not terminate during cleanup")


def run(argv, log, timeout, env=None, cwd=None):
    """Run argv with a verified same-user medium token and return its exit code."""
    if os.name != "nt":
        raise OSError("run-non-elevated-windows is only supported on Windows")
    if not argv:
        raise ValueError("argv must not be empty")
    process = _api("GetCurrentProcess", "kernel32", wintypes.HANDLE, [])()
    source = _token(process, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT)
    restricted = None
    child = _PROCESS_INFORMATION()
    output = None
    input_file = None
    child_created = False
    child_reaped = False
    cleanup_error = None
    try:
        output = Path(log).open("w", encoding="utf-8")
        # An already-safe caller still gets token validation and normal process
        # semantics; an elevated caller must use the restricted token path.
        if _token_is_medium_non_elevated(source):
            completed = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, env=env, cwd=cwd)
            return completed.returncode
        if not _token_is_elevated(source) and _token_integrity_rid(source) < MEDIUM_INTEGRITY_RID:
            raise PermissionError("current token is below medium integrity; refusing to raise it")
        restricted = _restricted_token(source)
        startup = _STARTUPINFO()
        startup.cb = ctypes.sizeof(startup)
        startup.dwFlags = 0x00000100
        import msvcrt
        output_handle = wintypes.HANDLE(msvcrt.get_osfhandle(output.fileno()))
        _require(_api("SetHandleInformation", "kernel32", wintypes.BOOL,
                      [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD])(
                          output_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT),
                 "SetHandleInformation failed")
        startup.hStdOutput = output_handle
        startup.hStdError = output_handle
        input_file = open(os.devnull, "rb")
        input_handle = wintypes.HANDLE(msvcrt.get_osfhandle(input_file.fileno()))
        _require(_api("SetHandleInformation", "kernel32", wintypes.BOOL,
                      [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD])(
                          input_handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT),
                 "SetHandleInformation(stdin) failed")
        startup.hStdInput = input_handle
        command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(value) for value in argv]))
        environment = os.environ if env is None else env
        environment_block = ctypes.create_unicode_buffer(
            "\0".join(f"{key}={value}" for key, value in sorted(environment.items(), key=lambda item: item[0].upper()))
            + "\0\0"
        )
        create = _api("CreateProcessAsUserW", "advapi32", wintypes.BOOL,
                      [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.POINTER(_SECURITY_ATTRIBUTES),
                       ctypes.POINTER(_SECURITY_ATTRIBUTES), wintypes.BOOL, wintypes.DWORD, wintypes.LPVOID,
                       wintypes.LPCWSTR, ctypes.POINTER(_STARTUPINFO), ctypes.POINTER(_PROCESS_INFORMATION)])
        _require(create(restricted, None, command, None, None, True, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        ctypes.cast(environment_block, wintypes.LPVOID), str(cwd) if cwd else None,
                        ctypes.byref(startup), ctypes.byref(child)),
                 "CreateProcessAsUserW failed")
        child_created = True
        child_token = _token(child.hProcess)
        try:
            if not _same_user(source, child_token) or not _token_is_medium_non_elevated(child_token):
                raise PermissionError("child token was not same-user medium-integrity and non-elevated")
        finally:
            _close(child_token)
        resume = _api("ResumeThread", "kernel32", wintypes.DWORD, [wintypes.HANDLE])
        _require(resume(child.hThread) != 0xFFFFFFFF, "ResumeThread failed")
        wait = _api("WaitForSingleObject", "kernel32", wintypes.DWORD, [wintypes.HANDLE, wintypes.DWORD])
        wait_result = wait(child.hProcess, max(0, int(timeout * 1000)))
        if wait_result == WAIT_TIMEOUT:
            try:
                _reap_child(child.hProcess)
            except OSError as error:
                raise OSError(error.errno, f"timeout cleanup failed: {error}") from error
            child_reaped = True
            raise subprocess.TimeoutExpired(argv, timeout)
        if wait_result != WAIT_OBJECT_0:
            raise OSError(ctypes.get_last_error(), "WaitForSingleObject failed")
        child_reaped = True
        exit_code = wintypes.DWORD()
        _require(_api("GetExitCodeProcess", "kernel32", wintypes.BOOL,
                      [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)])(child.hProcess, ctypes.byref(exit_code)),
                 "GetExitCodeProcess failed")
        return exit_code.value
    finally:
        if child_created and not child_reaped and child.hProcess:
            cleanup_error = None
            try:
                _reap_child(child.hProcess)
                child_reaped = True
            except OSError as error:
                cleanup_error = error
        _close(child.hThread)
        _close(child.hProcess)
        _close(restricted)
        _close(source)
        if output is not None:
            output.close()
        if input_file is not None:
            input_file.close()
        if cleanup_error is not None:
            original = sys.exc_info()[1]
            context = f"; original error: {original}" if original is not None else ""
            raise OSError(f"child cleanup failed: {cleanup_error}{context}") from cleanup_error
