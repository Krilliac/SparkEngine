"""Read-only MSI database and cabinet APIs. Never starts an installation session.

Only system DLLs are loaded; cabinet names never become output paths.
API contracts: https://learn.microsoft.com/windows/win32/api/msiquery/
https://learn.microsoft.com/windows/win32/api/setupapi/nf-setupapi-setupiteratecabinetw
"""
from __future__ import annotations

import ctypes as C
from contextlib import contextmanager
import os
from pathlib import Path
import struct


class NativeMsiError(ValueError):
    """The native payload cannot be completely and safely inventoried."""


def require(condition, message):
    if not condition:
        raise NativeMsiError(message)


def _dll(name):
    require(os.name == "nt", "offline MSI extraction requires Windows system APIs")
    # LOAD_LIBRARY_SEARCH_SYSTEM32: never resolve a DLL beside the package or on PATH.
    return C.WinDLL(name, use_last_error=True, winmode=0x800)


def _function(dll, name, args, result=C.c_uint):
    fn = getattr(dll, name)
    fn.argtypes, fn.restype = args, result
    return fn


class Database:
    """MSIDBOPEN_READONLY plus SELECT/stream reads only; all MSI handles are closed."""

    def __init__(self, path):
        self.dll = _dll("msi.dll")
        u, p, w = C.c_uint, C.c_void_p, C.c_wchar_p
        self.close_handle = _function(self.dll, "MsiCloseHandle", [u])
        self.open_view = _function(self.dll, "MsiDatabaseOpenViewW", [u, w, C.POINTER(u)])
        self.execute = _function(self.dll, "MsiViewExecute", [u, u])
        self.fetch = _function(self.dll, "MsiViewFetch", [u, C.POINTER(u)])
        self.string = _function(self.dll, "MsiRecordGetStringW", [u, u, w, C.POINTER(u)])
        self.read_stream = _function(self.dll, "MsiRecordReadStream", [u, u, p, C.POINTER(u)])
        self.handle = u()
        opening = _function(self.dll, "MsiOpenDatabaseW", [w, p, C.POINTER(u)])
        self.check(opening(str(path), None, C.byref(self.handle)), "open MSI read-only")

    @staticmethod
    def check(code, operation):
        require(code == 0, f"Windows Installer {operation} failed ({code})")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close_handle(self.handle)

    @contextmanager
    def records(self, sql):
        view = C.c_uint()
        self.check(self.open_view(self.handle, sql, C.byref(view)), "open SELECT")
        try:
            self.check(self.execute(view, 0), "execute SELECT")

            def iterator():
                while True:
                    record = C.c_uint()
                    code = self.fetch(view, C.byref(record))
                    if code == 259:  # ERROR_NO_MORE_ITEMS
                        return
                    self.check(code, "fetch record")
                    try:
                        yield record
                    finally:
                        self.close_handle(record)

            rows = iterator()
            try:
                yield rows
            finally:
                rows.close()
        finally:
            self.close_handle(view)

    def text(self, record, field):
        size = C.c_uint(0)
        code = self.string(record, field, None, C.byref(size))
        require(code in (0, 234), f"cannot read MSI text ({code})")
        require(size.value <= 32768, "MSI field exceeds text limit")
        buffer = C.create_unicode_buffer(size.value + 1)
        size.value += 1
        self.check(self.string(record, field, buffer, C.byref(size)), "read text")
        return buffer.value

    def rows(self, table, columns, limit=32767):
        sql = "SELECT " + ", ".join(f"`{column}`" for column in columns) + f" FROM `{table}`"
        result = []
        with self.records(sql) as records:
            for record in records:
                require(len(result) < limit, f"MSI {table} exceeds row limit")
                result.append(tuple(self.text(record, index + 1) for index in range(len(columns))))
        return result

    def word_count(self):
        summary = C.c_uint()
        opening = _function(self.dll, "MsiGetSummaryInformationW",
                            [C.c_uint, C.c_wchar_p, C.c_uint, C.POINTER(C.c_uint)])
        self.check(opening(self.handle, None, 0, C.byref(summary)), "open summary")
        try:
            kind, value, length = C.c_uint(), C.c_int(), C.c_uint()
            read = _function(self.dll, "MsiSummaryInfoGetPropertyW", [C.c_uint, C.c_uint,
                             C.POINTER(C.c_uint), C.POINTER(C.c_int), C.c_void_p,
                             C.c_wchar_p, C.POINTER(C.c_uint)])
            self.check(read(summary, 15, C.byref(kind), C.byref(value), None, None,
                            C.byref(length)), "read source type")
            require(kind.value == 3, "MSI source type must be an integer")
            return value.value
        finally:
            self.close_handle(summary)

    def stream(self, name, target, maximum):
        # Stream names come from the validated Media table. Quote SQL literals.
        sql = "SELECT `Data` FROM `_Streams` WHERE `Name` = '" + name.replace("'", "''") + "'"
        with self.records(sql) as records:
            record = next(records, None)
            require(record is not None, f"missing embedded cabinet stream: {name}")
            buffer = C.create_string_buffer(1024 * 1024)
            total = 0
            with target.open("xb") as output:
                while True:
                    size = C.c_uint(len(buffer))
                    self.check(self.read_stream(record, 1, buffer, C.byref(size)), "read cabinet stream")
                    if not size.value:
                        break
                    total += size.value
                    require(total <= maximum, "embedded cabinet exceeds byte limit")
                    output.write(buffer.raw[:size.value])
            require(next(records, None) is None, f"duplicate cabinet stream: {name}")
        require(total > 0, f"empty embedded cabinet: {name}")


def extract_cabinet(cabinet: Path, targets: dict):
    """Extract exactly the expected File IDs to preselected numeric filenames."""
    with cabinet.open("rb") as source:
        header = source.read(36)
    require(len(header) == 36 and header[:4] == b"MSCF", "invalid embedded cabinet header")
    fields = struct.unpack("<4sIIIIIBBHHHHH", header)
    require(fields[2] == cabinet.stat().st_size and fields[6:8] == (3, 1), "unsupported cabinet size/version")
    require(fields[10] & ~4 == 0, "spanned/external cabinet header is unsupported")
    require(fields[8] > 0 and fields[9] == len(targets), "cabinet header File count mismatch")
    dll = _dll("setupapi.dll")

    class FileInfo(C.Structure):
        _fields_ = [("NameInCabinet", C.c_wchar_p), ("FileSize", C.c_uint32),
                    ("Win32Error", C.c_uint32), ("DosDate", C.c_uint16),
                    ("DosTime", C.c_uint16), ("DosAttribs", C.c_uint16),
                    ("FullTargetName", C.c_wchar * 260)]

    class FilePaths(C.Structure):
        _fields_ = [("Target", C.c_wchar_p), ("Source", C.c_wchar_p),
                    ("Win32Error", C.c_uint32), ("Flags", C.c_uint32)]

    seen, completed, failures = set(), set(), []
    ordered_ids = list(targets)
    callback_type = C.WINFUNCTYPE(C.c_uint, C.c_void_p, C.c_uint, C.c_size_t, C.c_size_t)

    @callback_type
    def callback(_context, notification, param1, _param2):
        try:
            if notification == 0x10:  # SPFILENOTIFY_CABINETINFO
                return 0
            if notification == 0x12:  # SPFILENOTIFY_NEEDNEWCABINET
                raise NativeMsiError("spanned/external cabinets are unsupported")
            if notification == 0x11:  # SPFILENOTIFY_FILEINCABINET
                info = C.cast(param1, C.POINTER(FileInfo)).contents
                name = info.NameInCabinet
                require(name in targets and name not in seen, f"unexpected/duplicate cabinet member: {name}")
                require(name == ordered_ids[len(seen)], f"cabinet File sequence order mismatch: {name}")
                target, size = targets[name]
                require(info.FileSize == size and info.Win32Error == 0, f"cabinet size/error mismatch: {name}")
                require(info.DosAttribs & ~0xA7 == 0, f"non-regular cabinet member: {name}")
                require(not target.exists() and len(str(target)) < 260, "unsafe cabinet extraction target")
                seen.add(name)
                info.FullTargetName = str(target)
                return 1  # FILEOP_DOIT
            if notification == 0x13:  # SPFILENOTIFY_FILEEXTRACTED
                info = C.cast(param1, C.POINTER(FilePaths)).contents
                require(info.Win32Error == 0, f"cabinet extraction failed ({info.Win32Error})")
                require(info.Target not in completed, "duplicate extracted target")
                completed.add(info.Target)
                return 0
            raise NativeMsiError(f"unexpected cabinet notification: {notification}")
        except Exception as error:
            failures.append(str(error))
            # FILEINCABINET accepts FILEOP_ABORT; other notifications accept error codes.
            return 0 if notification == 0x11 else 13

    iterate = _function(dll, "SetupIterateCabinetW",
                        [C.c_wchar_p, C.c_uint32, callback_type, C.c_void_p], C.c_int)
    success = iterate(str(cabinet), 0, callback, None)
    require(not failures, "; ".join(failures))
    require(success, f"SetupIterateCabinetW failed ({C.get_last_error()})")
    require(seen == set(targets), "cabinet is missing expected File table members")
    require(completed == {str(target) for target, _ in targets.values()}, "incomplete cabinet extraction")
