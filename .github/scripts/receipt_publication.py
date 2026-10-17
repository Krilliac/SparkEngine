"""Bounded, atomic no-replace publication for portable consumer receipts.

The caller supplies an existing real directory. Linux uses an anonymous inode
anchored to its directory descriptor; Windows retains the package writer's
native file handle guarantees while directory handles prevent ancestor replacement.
"""
from contextlib import contextmanager
import os
from pathlib import Path
import stat
import sys

MAX_RECEIPT_BYTES = 1024 * 1024
_REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class ReceiptPublicationError(ValueError):
    pass


def _identity(info):
    return info.st_dev, info.st_ino


def _link_like(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & _REPARSE_POINT)


def _real_directories(parent):
    paths = list(reversed((parent, *parent.parents)))
    for path in paths:
        info = path.lstat()
        if _link_like(info) or not stat.S_ISDIR(info.st_mode):
            raise ReceiptPublicationError("receipt parent must contain only real directories")
    return paths


def _sync_directory(descriptor):
    # Unsupported durability is a failure too; never silently weaken the receipt.
    os.fsync(descriptor)


def _publish_posix(destination, payload):
    if sys.platform != "linux" or not hasattr(os, "O_TMPFILE"):
        raise ReceiptPublicationError("POSIX receipt publication requires Linux O_TMPFILE support")
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    parent_fd = os.open(destination.anchor, flags)
    descriptor = None
    published = False
    try:
        for component in destination.parent.parts[1:]:
            next_fd = os.open(component, flags, dir_fd=parent_fd)
            os.close(parent_fd)
            parent_fd = next_fd
        parent_identity = _identity(os.fstat(parent_fd))
        try:
            os.stat(destination.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise ReceiptPublicationError("receipt destination already exists")
        _sync_directory(parent_fd)
        # No staging pathname exists to be swapped or accidentally unlinked.
        # O_EXCL must not be used with O_TMPFILE: it would prohibit linking.
        descriptor = os.open(".", os.O_RDWR | os.O_TMPFILE | os.O_CLOEXEC,
                             0o600, dir_fd=parent_fd)
        owned_identity = _identity(os.fstat(descriptor))
        offset = 0
        view = memoryview(payload)
        while offset < len(payload):
            written = os.write(descriptor, view[offset:])
            if not 0 < written <= len(payload) - offset:
                raise OSError("receipt write made invalid progress")
            offset += written
        os.fsync(descriptor)
        # Linux open(2) documents this unprivileged O_TMPFILE publication path.
        # dst_dir_fd selects linkat; follow_symlinks=True supplies AT_SYMLINK_FOLLOW
        # for the kernel-owned proc fd link, never for the destination pathname.
        source = f"/proc/self/fd/{descriptor}"
        staged = os.stat(source)
        if (not stat.S_ISREG(staged.st_mode) or staged.st_nlink != 0
                or _identity(staged) != owned_identity):
            raise ReceiptPublicationError("anonymous receipt staging identity changed")
        _real_directories(destination.parent)
        if _identity(destination.parent.lstat()) != parent_identity:
            raise ReceiptPublicationError("receipt parent identity changed")
        os.link(source, destination.name, dst_dir_fd=parent_fd, follow_symlinks=True)
        published = True
        _sync_directory(parent_fd)
        linked = os.stat(destination.name, dir_fd=parent_fd, follow_symlinks=False)
        if _link_like(linked) or not stat.S_ISREG(linked.st_mode) or _identity(linked) != owned_identity:
            raise ReceiptPublicationError("published receipt identity changed")
        _real_directories(destination.parent)
        if _identity(destination.parent.lstat()) != parent_identity:
            raise ReceiptPublicationError("receipt parent changed during publication")
    except Exception as error:
        if published:
            # There is no atomic identity-conditional unlink. Leave all visible
            # names alone, including a competing replacement, and fail closed.
            raise ReceiptPublicationError(
                f"receipt may already be visible; publication/durability is unconfirmed: {error}"
            ) from error
        raise
    finally:
        if descriptor is not None:
            os.close(descriptor)
        os.close(parent_fd)


@contextmanager
def _locked_windows_parents(parent):
    import ctypes
    import package_evidence_io as windows_io

    handles = []
    try:
        for path in _real_directories(parent):
            # FILE_READ_ATTRIBUTES; share read only (no write/delete sharing);
            # OPEN_EXISTING; BACKUP_SEMANTICS | OPEN_REPARSE_POINT.
            handle = windows_io._CreateFileW(str(path), 0x80, 1, None, 3, 0x02200000, None)
            if ctypes.cast(handle, ctypes.c_void_p).value in (None, windows_io._INVALID_HANDLE_VALUE):
                raise OSError(ctypes.get_last_error(), "cannot lock receipt parent")
            handles.append(handle)
            info = path.lstat()
            if _link_like(info) or not stat.S_ISDIR(info.st_mode):
                raise ReceiptPublicationError("receipt parent is link-like")
        yield
    finally:
        for handle in reversed(handles):
            windows_io._close_windows_staging(handle)


def publish_receipt_no_replace(destination, payload):
    """Publish complete bytes once; reject existing names and unsafe traversal."""
    if type(payload) is not bytes or not 0 < len(payload) <= MAX_RECEIPT_BYTES:
        raise ReceiptPublicationError("receipt bytes must be nonempty and at most 1 MiB")
    destination = Path(destination)
    name = destination.name
    reserved = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
                *(f"lpt{i}" for i in range(1, 10))}
    if (not name or name in {".", ".."} or ".." in destination.parts or len(name.encode("utf-8")) > 200
            or any(character in name for character in ':\\/') or any(ord(character) < 32 for character in name)
            or name.rstrip(" .") != name or name.split(".")[0].rstrip(" .").lower() in reserved):
        raise ReceiptPublicationError("receipt destination must be a safe plain file name")
    destination = destination.absolute()
    try:
        _real_directories(destination.parent)
        if os.name == "nt":
            import package_evidence_io as windows_io
            with _locked_windows_parents(destination.parent):
                windows_io.publish_bytes_no_replace(destination, payload)
        elif os.name == "posix":
            _publish_posix(destination, payload)
        else:
            raise ReceiptPublicationError("receipt publication is unsupported on this operating system")
    except (OSError, ValueError) as error:
        raise ReceiptPublicationError(f"receipt publication failed: {error}") from error
