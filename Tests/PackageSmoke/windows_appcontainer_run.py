#!/usr/bin/env python3
"""Run a copied installed Windows FPS package with the checkout unreachable (RDY-020).

Windows counterpart of asset_repository_isolation.py. Windows has no mount
namespace, so the package runs inside a fresh AppContainer instead. An
AppContainer token passes an access check only through an ACE naming its own
package SID or ALL APPLICATION PACKAGES, and neither appears on the repository
or build tree. The run therefore needs no administrator rights and never
touches the user's repository ACLs. The only ACEs added are on the test-owned
package copies (read/execute). The child writes under its own AppContainer
profile; selected evidence is copied to the test-owned output directory before
the profile is deleted. The package-copy ACEs are removed on exit.

One session proves, in the same container:

* canaries: ``type`` of a source-tree file and of a build-tree file fails, and
  ``type`` of the package copy's level1.scene succeeds;
* NullRHI: the headless host loads the packaged module and arena; the same run
  from a copy without level1.scene fails and emits no arena record;
* D3D11/WARP: the windowed host loads the package copy's level1.scene (its
  exec_audit.log records the load path and SUCCESS) and renders and saves a
  frame that CheckFPSVisibleFrame.ps1 accepts; the same run from a copy without
  bin/Assets loads no scene and produces no such frame. (A copy without only
  level1.scene still draws the module's procedural props, which that frame
  check accepts, so the D3D11 control removes the whole asset tree and the
  scene load is judged from the audit trail.)

Nothing runs uncontained, and no run falls back to the repository. Record
grammar (arena, lifecycle) is judged afterwards by the existing CMake parsers
in RunInstalledFPSPackage.cmake; this script judges containment.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import ntpath
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys

SCENE_RELATIVE = ("bin", "Assets", "Scenes", "level1.scene")
ARENA_TOKEN = "SPARK_FPS_HEADLESS_ARENA"
SCREENSHOT_NAME = "fps-visible.png"
AUDIT_NAME = "exec_audit.log"
SCENE_LOAD_CALL = "SceneManager::LoadScene called. filepath="
SCENE_LOAD_SUCCESS = "SceneManager::LoadScene returned: SUCCESS"
RUN_TIMEOUT_SECONDS = 120
CANARY_TIMEOUT_SECONDS = 30


@dataclass(frozen=True)
class RunResult:
    """One contained process: exit code (None on timeout) and captured output."""

    exit_code: int | None
    stdout: str
    stderr: str


def _normalized(path: str) -> str:
    return ntpath.normcase(ntpath.normpath(ntpath.abspath(path)))


def is_within(path: str, root: str) -> bool:
    """Case-insensitive Windows containment, component-wise (not a string prefix)."""
    path_n, root_n = _normalized(path), _normalized(root)
    return path_n == root_n or path_n.startswith(root_n.rstrip("\\") + "\\")


def layout_errors(owned_dirs: list[str], forbidden_roots: list[str]) -> list[str]:
    """Package copies and the evidence output must be disjoint from source and build.

    A package copy inside a forbidden root would be granted to the container.
    The evidence output must also stay outside those roots.
    """
    errors = []
    if not forbidden_roots:
        errors.append("no source/build roots were named, so nothing proves them unreachable")
    for owned in owned_dirs:
        for root in forbidden_roots:
            if is_within(owned, root):
                errors.append(f"{owned} is inside the forbidden root {root}")
            elif is_within(root, owned):
                errors.append(f"forbidden root {root} is inside the granted directory {owned}")
    return errors


def names_forbidden_root(text: str, forbidden_roots: list[str]) -> str | None:
    """The forbidden root that ``text`` names, in either slash spelling and any case."""
    haystack = text.replace("/", "\\").casefold()
    for root in forbidden_roots:
        needle = ntpath.normpath(root).casefold().rstrip("\\")
        if needle and needle in haystack:
            return root
    return None


def loaded_scene_paths(audit: str) -> list[str]:
    """Every scene path the windowed module asked SceneManager to load, from its exec_audit.log."""
    paths = []
    for line in audit.splitlines():
        _, marker, path = line.partition(SCENE_LOAD_CALL)
        if marker:
            paths.append(path.strip())
    return paths


def engine_argv(package: str, phase: str, visual_script: str | None = None) -> list[str]:
    """The installed-package command lines RunSparkFPSHeadlessArena/RunInstalledFPSD3D11 use."""
    bin_dir = ntpath.join(package, "bin")
    argv = [ntpath.join(bin_dir, "SparkEngine.exe")]
    if phase == "nullrhi":
        argv.append("-headless")
    elif phase != "d3d11":
        raise ValueError(f"unknown phase {phase!r}")
    argv += ["-game", ntpath.join(bin_dir, "SparkGameFPS.dll"), "-require-game",
             "-test-frames", "8", "-threads", "2"]
    if phase == "d3d11":
        if not visual_script:
            raise ValueError("the D3D11 phase needs the visual.exec script")
        argv += ["-window-size", "640x360", "-no-subprocess", "-exec", visual_script]
    else:
        argv.append("-no-subprocess")
    return argv


def phase_environment(phase: str) -> dict[str, str]:
    if phase == "nullrhi":
        return {"SPARK_RHI_BACKEND": "null"}
    return {"SPARK_RHI_BACKEND": "d3d11", "SPARK_D3D11_DRIVER": "warp"}


def containment_verdict(results: dict[str, RunResult], forbidden_roots: list[str],
                        screenshots: dict[str, str | None], output_dir: str,
                        frames_authored: dict[str, bool], audits: dict[str, str], package: str) -> list[str]:
    """Judge one session. Every rule fails closed; an absent run is a failure.

    ``screenshots`` maps each D3D11 run to the saved frame path (None when no
    frame was written), ``frames_authored`` to the CheckFPSVisibleFrame.ps1
    verdict for that frame and ``audits`` to its exec_audit.log text ("" when
    none was written). ``package`` is the positive package copy.
    """
    errors = []
    required = ("canary-source", "canary-build", "canary-package", "nullrhi-positive",
                "nullrhi-negative", "d3d11-positive", "d3d11-negative")
    for name in required:
        if name not in results:
            errors.append(f"{name} did not run")
    if errors:
        return errors

    for name in ("canary-source", "canary-build"):
        if results[name].exit_code is None:
            errors.append(f"{name}: timed out, so the repository access check is inconclusive")
        elif results[name].exit_code == 0:
            errors.append(f"{name}: the container read a repository/build file, so the checkout is reachable")
    package_canary = results["canary-package"]
    if package_canary.exit_code != 0 or "[Scene]" not in package_canary.stdout:
        errors.append("canary-package: the container could not read the package copy's level1.scene")

    for name in ("nullrhi-positive", "nullrhi-negative", "d3d11-positive", "d3d11-negative"):
        result = results[name]
        if result.exit_code is None:
            errors.append(f"{name}: timed out")
        leaked = names_forbidden_root("\n".join((result.stdout, result.stderr, audits.get(name, ""))),
                                      forbidden_roots)
        if leaked is not None:
            errors.append(f"{name}: output names the forbidden root {leaked}")

    if results["nullrhi-positive"].exit_code != 0:
        errors.append(f"nullrhi-positive: exited {results['nullrhi-positive'].exit_code}")
    negative = results["nullrhi-negative"]
    if negative.exit_code == 0:
        errors.append("nullrhi-negative: the scene-less package still passed, so the verdict does not "
                      "depend on package assets")
    if ARENA_TOKEN in negative.stdout or ARENA_TOKEN in negative.stderr:
        errors.append("nullrhi-negative: emitted an arena record without the authored scene")

    if results["d3d11-positive"].exit_code != 0:
        errors.append(f"d3d11-positive: exited {results['d3d11-positive'].exit_code}")
    positive_frame = screenshots.get("d3d11-positive")
    if positive_frame is None:
        errors.append("d3d11-positive: saved no screenshot")
    elif not is_within(positive_frame, output_dir):
        errors.append(f"d3d11-positive: screenshot {positive_frame} is outside the run output directory")
    elif not frames_authored.get("d3d11-positive", False):
        errors.append("d3d11-positive: the saved frame is not a visible authored arena frame")
    positive_audit = audits.get("d3d11-positive", "")
    scenes = loaded_scene_paths(positive_audit)
    package_scene = ntpath.join(package, *SCENE_RELATIVE)
    if not scenes or SCENE_LOAD_SUCCESS not in positive_audit:
        errors.append("d3d11-positive: exec_audit.log records no successful level1.scene load")
    elif any(_normalized(scene) != _normalized(package_scene) for scene in scenes):
        errors.append(f"d3d11-positive: loaded {scenes}, not only the package copy's {package_scene}")
    negative_frame = screenshots.get("d3d11-negative")
    if negative_frame is not None and frames_authored.get("d3d11-negative", True):
        errors.append("d3d11-negative: the asset-less package still rendered a visible arena frame")
    if SCENE_LOAD_SUCCESS in audits.get("d3d11-negative", ""):
        errors.append("d3d11-negative: the asset-less package still loaded a scene")
    return errors


class AppContainer:
    """A uniquely named AppContainer profile, deleted on exit. Windows only."""

    def __init__(self) -> None:
        self.name = "SparkPackageIsolation." + secrets.token_hex(8)
        self.sid_string = ""
        self._sid = None
        self._granted: list[str] = []
        self.profile_path: Path | None = None

    def __enter__(self) -> "AppContainer":
        import ctypes
        from ctypes import wintypes

        userenv = ctypes.WinDLL("userenv", use_last_error=True)
        advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        sid = ctypes.c_void_p()
        hr = userenv.CreateAppContainerProfile(
            ctypes.c_wchar_p(self.name), ctypes.c_wchar_p(self.name),
            ctypes.c_wchar_p("SparkEngine package repository-isolation test"),
            None, wintypes.DWORD(0), ctypes.byref(sid))
        if hr != 0:
            raise OSError(f"CreateAppContainerProfile failed: HRESULT 0x{hr & 0xFFFFFFFF:08X}")
        self._sid = sid
        text = ctypes.c_wchar_p()
        if not advapi32.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            self._delete_profile()
            raise ctypes.WinError(ctypes.get_last_error())
        self.sid_string = text.value or ""
        kernel32.LocalFree(text)
        profile = ctypes.c_wchar_p()
        userenv.GetAppContainerFolderPath.argtypes = [
            ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_wchar_p)]
        userenv.GetAppContainerFolderPath.restype = ctypes.c_long
        hr = userenv.GetAppContainerFolderPath(
            ctypes.c_wchar_p(self.sid_string), ctypes.byref(profile))
        if hr != 0:
            self._delete_profile()
            raise OSError(f"GetAppContainerFolderPath failed: HRESULT 0x{hr & 0xFFFFFFFF:08X}")
        try:
            self.profile_path = Path(profile.value) if profile.value else None
        finally:
            ole32 = ctypes.WinDLL("ole32", use_last_error=True)
            ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]
            ole32.CoTaskMemFree.restype = None
            ole32.CoTaskMemFree(ctypes.cast(profile, ctypes.c_void_p))
        if self.profile_path is None or not self.profile_path.is_dir():
            self._delete_profile()
            raise OSError(f"AppContainer profile folder is missing: {self.profile_path}")
        return self

    def grant(self, path: str, rights: str) -> None:
        """Add an inheritable ACE for this container to a test-owned directory."""
        self._icacls(path, "/grant", f"*{self.sid_string}:(OI)(CI){rights}")
        self._granted.append(path)

    def _icacls(self, *args: str) -> None:
        icacls = ntpath.join(os.environ["SystemRoot"], "System32", "icacls.exe")
        result = subprocess.run([icacls, *args, "/Q"], capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            raise OSError(f"icacls {' '.join(args)} failed: {result.stdout}{result.stderr}")

    def _delete_profile(self) -> None:
        import ctypes

        userenv = ctypes.WinDLL("userenv", use_last_error=True)
        advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
        if self._sid is not None:
            advapi32.FreeSid(self._sid)
            self._sid = None
        hr = userenv.DeleteAppContainerProfile(ctypes.c_wchar_p(self.name))
        if hr != 0:
            raise OSError(f"DeleteAppContainerProfile failed: HRESULT 0x{hr & 0xFFFFFFFF:08X}")

    def __exit__(self, *exc: object) -> None:
        cleanup_errors: list[OSError] = []
        try:
            for path in reversed(self._granted):
                try:
                    self._icacls(path, "/remove:g", f"*{self.sid_string}")
                except OSError as error:
                    cleanup_errors.append(error)
        finally:
            try:
                self._delete_profile()
            except OSError as error:
                cleanup_errors.append(error)
        if cleanup_errors:
            raise OSError("AppContainer cleanup failed: " + "; ".join(map(str, cleanup_errors)))

    def run(self, argv: list[str], cwd: str, env: dict[str, str], log_dir: Path,
            timeout: int) -> RunResult:
        """Launch argv inside this container; log_dir is beneath its writable profile."""
        import ctypes
        from ctypes import wintypes
        import msvcrt

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

        class SidAndAttributes(ctypes.Structure):
            _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wintypes.DWORD)]

        class SecurityCapabilities(ctypes.Structure):
            _fields_ = [("AppContainerSid", ctypes.c_void_p),
                        ("Capabilities", ctypes.POINTER(SidAndAttributes)),
                        ("CapabilityCount", wintypes.DWORD), ("Reserved", wintypes.DWORD)]

        class StartupInfo(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
                        ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR),
                        ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                        ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
                        ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
                        ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                        ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                        ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                        ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]

        class StartupInfoEx(ctypes.Structure):
            _fields_ = [("StartupInfo", StartupInfo), ("lpAttributeList", ctypes.c_void_p)]

        class ProcessInformation(ctypes.Structure):
            _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                        ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]

        class JobBasicLimits(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                        ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                        ("SchedulingClass", wintypes.DWORD)]

        class JobExtendedLimits(ctypes.Structure):
            _fields_ = [("BasicLimitInformation", JobBasicLimits), ("IoInfo", ctypes.c_uint64 * 6),
                        ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

        kernel32.CreateProcessW.argtypes = [
            wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL,
            wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p]
        kernel32.InitializeProcThreadAttributeList.argtypes = [
            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.c_size_t)]
        kernel32.UpdateProcThreadAttribute.argtypes = [
            ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_void_p, ctypes.c_void_p]
        kernel32.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                     wintypes.DWORD]
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

        proc_thread_attribute_handle_list = 0x00020002
        proc_thread_attribute_security_capabilities = 0x00020009
        extended_startupinfo_present = 0x00080000
        create_unicode_environment = 0x00000400
        create_suspended = 0x00000004
        startf_usestdhandles = 0x00000100
        job_object_limit_kill_on_job_close = 0x00002000
        job_object_extended_limit_information = 9
        wait_timeout = 0x00000102

        log_dir.mkdir(parents=True, exist_ok=True)
        stdout_path, stderr_path = log_dir / "stdout.log", log_dir / "stderr.log"
        with open(os.devnull, "rb") as null_in, open(stdout_path, "wb") as out, open(stderr_path, "wb") as err:
            handles = [msvcrt.get_osfhandle(stream.fileno()) for stream in (null_in, out, err)]
            for handle in handles:
                os.set_handle_inheritable(handle, True)
            capabilities = SecurityCapabilities(self._sid, None, 0, 0)
            inherited = (wintypes.HANDLE * 3)(*handles)
            size = ctypes.c_size_t(0)
            kernel32.InitializeProcThreadAttributeList(None, 2, 0, ctypes.byref(size))
            attribute_buffer = ctypes.create_string_buffer(size.value)
            if not kernel32.InitializeProcThreadAttributeList(attribute_buffer, 2, 0, ctypes.byref(size)):
                raise ctypes.WinError(ctypes.get_last_error())
            job = None
            process = ProcessInformation()
            try:
                if not kernel32.UpdateProcThreadAttribute(
                        attribute_buffer, 0, proc_thread_attribute_security_capabilities,
                        ctypes.byref(capabilities), ctypes.sizeof(capabilities), None, None):
                    raise ctypes.WinError(ctypes.get_last_error())
                if not kernel32.UpdateProcThreadAttribute(
                        attribute_buffer, 0, proc_thread_attribute_handle_list,
                        inherited, ctypes.sizeof(inherited), None, None):
                    raise ctypes.WinError(ctypes.get_last_error())
                startup = StartupInfoEx()
                startup.StartupInfo.cb = ctypes.sizeof(StartupInfoEx)
                startup.StartupInfo.dwFlags = startf_usestdhandles
                startup.StartupInfo.hStdInput, startup.StartupInfo.hStdOutput, \
                    startup.StartupInfo.hStdError = handles
                startup.lpAttributeList = ctypes.cast(attribute_buffer, ctypes.c_void_p)
                block = "".join(f"{key}={value}\0" for key, value in
                                sorted(env.items(), key=lambda item: item[0].upper())) + "\0"
                environment = ctypes.create_unicode_buffer(block, len(block))
                command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline(argv))

                job = kernel32.CreateJobObjectW(None, None)
                if not job:
                    raise ctypes.WinError(ctypes.get_last_error())
                limits = JobExtendedLimits()
                limits.BasicLimitInformation.LimitFlags = job_object_limit_kill_on_job_close
                if not kernel32.SetInformationJobObject(job, job_object_extended_limit_information,
                                                        ctypes.byref(limits), ctypes.sizeof(limits)):
                    raise ctypes.WinError(ctypes.get_last_error())
                if not kernel32.CreateProcessW(
                        argv[0], command_line, None, None, True,
                        extended_startupinfo_present | create_unicode_environment | create_suspended,
                        environment, cwd, ctypes.byref(startup), ctypes.byref(process)):
                    raise ctypes.WinError(ctypes.get_last_error())
                if not kernel32.AssignProcessToJobObject(job, process.hProcess):
                    error = ctypes.get_last_error()
                    kernel32.TerminateProcess(process.hProcess, 1)
                    raise ctypes.WinError(error)
                kernel32.ResumeThread(process.hThread)
                exit_code: int | None = None
                if kernel32.WaitForSingleObject(process.hProcess, timeout * 1000) != wait_timeout:
                    code = wintypes.DWORD()
                    if not kernel32.GetExitCodeProcess(process.hProcess, ctypes.byref(code)):
                        raise ctypes.WinError(ctypes.get_last_error())
                    exit_code = ctypes.c_int32(code.value).value
                # Anything the run left behind dies with the job.
                kernel32.TerminateJobObject(job, 1)
            finally:
                for handle in (process.hThread, process.hProcess, job):
                    if handle:
                        kernel32.CloseHandle(handle)
                kernel32.DeleteProcThreadAttributeList(attribute_buffer)
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
        (log_dir / "exit_code.txt").write_text("timeout" if exit_code is None else str(exit_code),
                                               encoding="utf-8")
        return RunResult(exit_code, stdout, stderr)


def contained_environment(home: Path, extra: dict[str, str]) -> dict[str, str]:
    """A cleared environment: system root, System32 PATH and user dirs inside the output."""
    system_root = os.environ["SystemRoot"]
    env = {
        "SystemRoot": system_root,
        "windir": system_root,
        "SystemDrive": os.environ.get("SystemDrive", system_root[:2]),
        "PATH": ntpath.join(system_root, "System32"),
        "USERPROFILE": str(home),
        "LOCALAPPDATA": str(home / "LocalAppData"),
        "APPDATA": str(home / "AppData"),
        "TEMP": str(home / "Temp"),
        "TMP": str(home / "Temp"),
    }
    for key in ("LocalAppData", "AppData", "Temp"):
        (home / key).mkdir(parents=True, exist_ok=True)
    env.update(extra)
    return env


def frame_is_authored(frame_check: Path, image: Path) -> bool:
    """CheckFPSVisibleFrame.ps1, run outside the container on the saved frame."""
    powershell = ntpath.join(os.environ["SystemRoot"], "System32", "WindowsPowerShell", "v1.0",
                             "powershell.exe")
    result = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-File", str(frame_check),
                             "-ImagePath", str(image)], capture_output=True, text=True, timeout=60)
    print(f"frame check {image.name} ({image.parent.name}): exit {result.returncode} "
          f"{result.stdout.strip()} {result.stderr.strip()}")
    return result.returncode == 0


def publish_run_artifacts(private_run: Path, output_run: Path) -> None:
    """Retain only the run's evidence before its temporary profile is deleted."""
    output_run.mkdir()
    for name in ("exit_code.txt", "stdout.log", "stderr.log", AUDIT_NAME,
                 SCREENSHOT_NAME, "visual.exec"):
        source = private_run / name
        if source.is_file():
            shutil.copyfile(source, output_run / name)


def run_session(args: argparse.Namespace) -> list[str]:
    forbidden = [str(args.source_root), str(args.build_root)]
    package, output = str(args.package), str(args.output)
    scene_less, asset_less = str(args.scene_less_package), str(args.asset_less_package)
    errors = layout_errors([package, scene_less, asset_less, output], forbidden)
    if errors:
        return errors
    scene = Path(package, *SCENE_RELATIVE)
    if not scene.is_file():
        return [f"the package copy has no {scene}"]
    if Path(scene_less, *SCENE_RELATIVE).exists() or not Path(scene_less, "bin", "Assets").is_dir():
        return ["the scene-less control copy must keep bin/Assets and lack only level1.scene"]
    if Path(asset_less, "bin", "Assets").exists() or not Path(asset_less, "bin", "SparkEngine.exe").is_file():
        return ["the asset-less control copy must keep bin/SparkEngine.exe and lack bin/Assets"]
    negatives = {"nullrhi": scene_less, "d3d11": asset_less}

    output_dir = Path(output)
    cmd = ntpath.join(os.environ["SystemRoot"], "System32", "cmd.exe")
    results: dict[str, RunResult] = {}
    screenshots: dict[str, str | None] = {}
    authored: dict[str, bool] = {}
    audits: dict[str, str] = {}
    with AppContainer() as container:
        print(f"AppContainer {container.name} ({container.sid_string})")
        if container.profile_path is None:
            raise RuntimeError("AppContainer has no writable profile directory")
        private_root = container.profile_path / "SparkPackageIsolationRuns"
        private_root.mkdir()
        for granted in (package, scene_less, asset_less):
            container.grant(granted, "RX")
        canaries = {
            "canary-source": str(args.source_root / "CMakeLists.txt"),
            "canary-build": str(args.build_root / "CMakeCache.txt"),
            "canary-package": str(scene),
        }
        for name, target in canaries.items():
            if name != "canary-package" and not Path(target).is_file():
                return [f"{name}: {target} does not exist outside the container, so the canary proves nothing"]
            run_dir = private_root / name
            results[name] = container.run(
                [cmd, "/d", "/c", "type", target], str(run_dir),
                contained_environment(run_dir / "home", {}), run_dir, CANARY_TIMEOUT_SECONDS)
            publish_run_artifacts(run_dir, output_dir / name)
        for phase in ("nullrhi", "d3d11"):
            for variant, package_root in (("positive", package), ("negative", negatives[phase])):
                name = f"{phase}-{variant}"
                run_dir = private_root / name
                run_dir.mkdir(parents=True, exist_ok=True)
                visual = None
                if phase == "d3d11":
                    visual = run_dir / "visual.exec"
                    visual.write_text(f"0 gfx_screenshot {SCREENSHOT_NAME}\n", encoding="utf-8")
                results[name] = container.run(
                    engine_argv(package_root, phase, str(visual) if visual else None), str(run_dir),
                    contained_environment(run_dir / "home", phase_environment(phase)), run_dir,
                    RUN_TIMEOUT_SECONDS)
                published_run = output_dir / name
                publish_run_artifacts(run_dir, published_run)
                print(f"{name}: exit {results[name].exit_code}")
                if phase == "d3d11":
                    frame = published_run / SCREENSHOT_NAME
                    screenshots[name] = str(frame) if frame.is_file() else None
                    if screenshots[name] is not None:
                        authored[name] = frame_is_authored(args.frame_check, frame)
                    audit = published_run / AUDIT_NAME
                    audits[name] = audit.read_text(encoding="utf-8", errors="replace") if audit.is_file() else ""
    summary = {name: result.exit_code for name, result in results.items()}
    (output_dir / "appcontainer-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return containment_verdict(results, forbidden, screenshots, output, authored, audits, package)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True, help="installed package copy")
    parser.add_argument("--scene-less-package", type=Path, required=True,
                        help="package copy with bin/Assets/Scenes/level1.scene removed (NullRHI control)")
    parser.add_argument("--asset-less-package", type=Path, required=True,
                        help="package copy with bin/Assets removed (D3D11 control)")
    parser.add_argument("--output", type=Path, required=True, help="fresh, test-owned run directory")
    parser.add_argument("--frame-check", type=Path, required=True, help="CheckFPSVisibleFrame.ps1")
    args = parser.parse_args()
    if sys.platform != "win32":
        raise RuntimeError("AppContainer package isolation requires Windows")
    for name in ("source_root", "build_root", "package", "scene_less_package", "asset_less_package", "output"):
        setattr(args, name, Path(os.path.abspath(getattr(args, name))))
    if not args.output.is_dir() or any(args.output.iterdir()):
        raise RuntimeError(f"--output must be an existing empty directory: {args.output}")
    errors = run_session(args)
    if errors:
        raise RuntimeError("\n".join(errors))
    print("installed FPS package ran with the repository unreachable "
          "(Windows AppContainer; NullRHI and D3D11/WARP; negative controls failed)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"AppContainer package isolation failed: {error}", file=sys.stderr)
        sys.exit(1)
