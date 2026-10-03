"""Isolated proposal: actual owned-window dispatch. Import/tests never launch a process."""
import argparse
from collections import deque
import ctypes
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import queue
import re
import stat
import subprocess
import sys
import threading
import time

TITLE = 'Spark Engine - Spark Arena - Engine Showcase'
CLASS = 'SparkEngineWindowClass'
KEYS = {1: 0x71, 2: 0x72, 4: 0x74, 8: 0x78}
SCOUT, VANGUARD = '0', '4' # SparkSDK/Include/Spark/GameTypes.h:224-231.
TRACE_FIELDS = 'v phase input update mask pressed released paused action result reason operation faults profile transfer saves'.split()
WRAPPER = """import subprocess,sys
if sys.stdin.buffer.readline()!=b'GO\\n': sys.exit(125)
p=subprocess.Popen(sys.argv[1:])
print('SPARK_INPUT_CHILD '+str(p.pid),flush=True)
sys.exit(p.wait())
"""


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def plain(path):
    path = Path(path)
    require(path.is_absolute() and '..' not in path.parts and str(path).isascii(), 'absolute plain ASCII path')
    for item in (path, *path.parents):
        info = item.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                'link/reparse path')
    return path.resolve(strict=True)


def copy_primary(slot, destination):
    slot = plain(slot)
    before = slot.stat()
    require(stat.S_ISREG(before.st_mode) and 0 < before.st_size <= 16*1024*1024, 'save copy size/type')
    before_hash = digest(slot)
    with slot.open('rb') as source, destination.open('xb') as target:
        remaining = before.st_size
        while remaining:
            block = source.read(min(65536, remaining))
            require(block, 'save shortened during copy')
            target.write(block)
            remaining -= len(block)
        require(not source.read(1), 'save grew during copy')
    after = slot.stat()
    require((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
            (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'save changed during copy')
    require(digest(slot) == digest(destination) == before_hash, 'save copy hash')
    return before_hash


def restored_frame(frame, saved, load_update):
    live = frame['profile']
    require(live.keys() == saved.keys(), 'restored profile schema')
    for key in saved:
        if key != 'fps.profile.playTime':
            require(live[key] == saved[key], 'restored profile changed after dispatch: '+key)
    elapsed = float(live['fps.profile.playTime']) - float(saved['fps.profile.playTime'])
    updates = frame['update'] - load_update + 1
    # Pinned default timeScale=1, nonnegative dt clamped to0.25 by Game::Update.
    require(updates > 0 and math.isfinite(elapsed) and 0 <= elapsed <= updates * .25 + .00001,
            'restored playTime advanced outside completed-frame bound')


def profile(hex_text):
    require(re.fullmatch('[0-9a-f]+', hex_text) and len(hex_text) <= 8192, 'profile encoding')
    data = bytes.fromhex(hex_text).decode('ascii')
    rows = data.splitlines()
    pairs = [row.split('=', 1) for row in rows]
    require(all(len(p) == 2 and p[0].startswith('fps.profile.') for p in pairs), 'profile fields')
    result = dict(pairs)
    require(len(result) == len(pairs) and rows == sorted(rows), 'duplicate/unsorted profile')
    require(all(math.isfinite(float(v)) for v in result.values()), 'nonfinite profile')
    require('fps.profile.class' in result and 'fps.profile.playTime' in result, 'missing profile')
    return result


def parse_trace(line):
    require(line.startswith('SPARK_FPS_INPUT '), 'trace prefix')
    parts = [p.split('=', 1) for p in line.rstrip('\n').split(' ')[1:]]
    require(all(len(p) == 2 for p in parts), 'trace syntax')
    record = dict(parts)
    require([p[0] for p in parts] == TRACE_FIELDS, 'trace schema/order')
    for key in TRACE_FIELDS:
        if key not in ('phase', 'profile', 'transfer', 'saves'):
            require(re.fullmatch('-?[0-9]+', record[key]), 'trace integer')
            record[key] = int(record[key])
    require(record['v'] == 1 and record['paused'] == 0, 'version or paused')
    require(record['faults'] == 0, 'guarded subsystem fault')
    require(record['phase'] in ('before', 'operation', 'dispatch', 'complete'), 'phase')
    for key in ('mask', 'pressed', 'released'):
        require(0 <= record[key] <= 15, 'key mask')
    require(record['action'] in range(4) and record['result'] in (-1, 0, 1), 'operation status')
    record['profile'] = profile(record['profile'])
    record['transfer'] = None if record['transfer'] == '-' else profile(record['transfer'])
    require(record['saves'] == '-' or re.fullmatch('[0-9a-f]+', record['saves']), 'save directory encoding')
    record['saves'] = None if record['saves'] == '-' else bytes.fromhex(record['saves']).decode('utf-8')
    return record


class Replay:
    def __init__(self, saves, missing_only=False):
        self.saves = Path(saves).resolve()
        self.pending = None
        self.last_input = self.last_update = self.operation = 0
        self.records = self.byte_count = 0
        self.operations = []
        self.frames = []
        self.ended = False
        self.missing_only = missing_only
        self.saves_bound = False

    def feed(self, line):
        if line.startswith('SPARK_FPS_INPUT_END '):
            match = re.fullmatch(r'SPARK_FPS_INPUT_END v=1 records=(\d+) bytes=(\d+) failed=0\n?', line)
            require(match and not self.ended and self.pending is None, 'incomplete terminal')
            require((int(match[1]), int(match[2])) == (self.records, self.byte_count), 'terminal counts')
            self.ended = True
            return None
        if 'SPARK_FPS_INPUT' not in line:
            return None
        require(not self.ended, 'trace after terminal')
        record = parse_trace(line)
        self.records += 1
        self.byte_count += len(line.encode('utf-8'))
        require(self.records <= 256 and self.byte_count <= 98304, 'trace overflow')
        if record['saves'] is not None:
            require(Path(record['saves']).resolve() == self.saves, 'wrong save directory')
            self.saves_bound = True
        require(self.saves_bound, 'missing initial save directory binding')
        require(record['phase'] != 'operation' or record['saves'] is not None, 'operation missing save binding')
        phase = record['phase']
        if phase == 'before':
            require(self.pending is None and record['input'] > self.last_input and
                    record['update'] > self.last_update, 'stale/incomplete frame')
            require(record['action'] == 0 and record['result'] == -1 and
                    record['operation'] == self.operation and record['transfer'] is None and
                    record['reason'] == 0, 'dirty frame start')
            self.pending = {'before': record}
        else:
            require(self.pending is not None, 'phase without before')
            first = self.pending['before']
            for key in ('input', 'update', 'mask', 'pressed', 'released'):
                require(record[key] == first[key], 'cross-frame record')
            require(phase not in self.pending, 'duplicate phase')
            if phase == 'operation':
                require('dispatch' not in self.pending, 'late operation')
                expected = 1 if first['pressed'] & 3 == 1 else 2 if first['pressed'] & 3 == 2 else 3
                require(record['action'] == expected and expected != 3, 'unexpected/conflicting operation')
                require(record['operation'] == self.operation + 1, 'operation serial')
                if self.missing_only:
                    require(expected == 2 and record['result'] == 0 and record['reason'] == 1 and
                            record['transfer'] is None and record['profile'] == first['profile'],
                            'missing-slot did not reject unchanged at actual dispatch')
                else:
                    require(record['result'] == 1 and record['reason'] == 0 and
                            record['transfer'] is not None, 'failed/unbound operation')
                self.operation += 1
                self.operations.append(dict(record, beforeProfile=first['profile']))
            elif phase == 'dispatch':
                op = self.pending.get('operation')
                require(record['operation'] == self.operation, 'dispatch serial')
                require(record['action'] == (op['action'] if op else 0), 'missing operation evidence')
                require(record['result'] == (op['result'] if op else -1) and
                        record['reason'] == (op['reason'] if op else 0) and
                        record['transfer'] == (op['transfer'] if op else None), 'dispatch result/transfer')
                require((first['pressed'] & 3 == 0) == (op is None), 'unobserved persistence edge')
            elif phase == 'complete':
                require('dispatch' in self.pending, 'missing successful dispatch boundary')
                prior = self.pending['dispatch']
                for key in ('action', 'result', 'reason', 'operation', 'transfer'):
                    require(record[key] == prior[key], 'completion changed operation')
                self.frames.append(record)
                self.last_input, self.last_update = record['input'], record['update']
                self.pending = None
                return record
            self.pending[phase] = record
        return None


class OwnedWindow:
    """No global input APIs. Every send revalidates the held process and exact HWND."""
    def __init__(self, pid, executable):
        from ctypes import wintypes as w
        self.w = w
        self.u = ctypes.WinDLL('user32', use_last_error=True)
        self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.k.OpenProcess.restype = w.HANDLE
        self.k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
        self.k.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
        self.k.GetExitCodeProcess.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        self.u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        self.u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        self.u.IsWindow.argtypes = [w.HWND]
        self.u.MapVirtualKeyW.argtypes = [w.UINT, w.UINT]
        self.u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        self.handle = self.k.OpenProcess(0x1000, False, pid)
        require(self.handle, 'cannot bind child process')
        self.pid, self.path, self.hwnd = pid, Path(executable).resolve(), None
        self.start = self.identity()
        self.callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        self.u.EnumWindows.argtypes = [self.callback_type, w.LPARAM]

    def identity(self):
        w = self.w
        code = w.DWORD()
        require(self.k.GetExitCodeProcess(self.handle, ctypes.byref(code)) and code.value == 259, 'child exited')
        path, size = ctypes.create_unicode_buffer(32768), w.DWORD(32768)
        require(self.k.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(size)), 'child path query')
        require(Path(path.value).resolve() == self.path, 'child image mismatch')
        times = [w.FILETIME() for _ in range(4)]
        require(self.k.GetProcessTimes(self.handle, *(ctypes.byref(t) for t in times)), 'child start query')
        return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime

    def matches(self, hwnd):
        pid = self.w.DWORD()
        self.u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value != self.pid:
            return False
        title, cls = ctypes.create_unicode_buffer(512), ctypes.create_unicode_buffer(256)
        self.u.GetWindowTextW(hwnd, title, len(title))
        self.u.GetClassNameW(hwnd, cls, len(cls))
        return title.value == TITLE and cls.value == CLASS

    def validate(self):
        require(self.identity() == self.start, 'process identity changed')
        matches = []
        callback = self.callback_type(lambda hwnd, _: (matches.append(hwnd) if self.matches(hwnd) else None) or True)
        require(self.u.EnumWindows(callback, 0), 'window enumeration failed')
        require(len(matches) == 1, 'ambiguous/missing owned window')
        if self.hwnd is None:
            self.hwnd = matches[0]
        require(matches[0] == self.hwnd and self.u.IsWindow(self.hwnd), 'owned HWND changed')

    def send(self, bit, down):
        self.validate()
        vk = KEYS[bit]
        scan = self.u.MapVirtualKeyW(vk, 0)
        require(scan != 0, 'missing key scan code')
        flags = 1 | (scan << 16) | (0 if down else (3 << 30))
        require(self.u.PostMessageW(self.hwnd, 0x100 if down else 0x101, vk, flags), 'key message rejected')

    def close_window(self):
        self.validate()
        require(self.u.PostMessageW(self.hwnd, 0x10, 0, 0), 'WM_CLOSE rejected')

    def close_handle(self):
        if self.handle:
            self.k.CloseHandle(self.handle)
            self.handle = None


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class Child:
    def __init__(self, command, cwd, environment, source, deadline):
        require(time.monotonic() < deadline, 'owner deadline before child launch')
        self.deadline, self.lines, self.queue = deadline, [], queue.Queue()
        self.job = load_module('_fps_owned_job', source / '.github/scripts/qualify-installed-native.py').job_for(source)
        self.process = None
        assigned = False
        try:
            self.process = subprocess.Popen([sys.executable, '-c', WRAPPER, *map(str, command)], cwd=cwd,
                                            env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            assigned = self.job.assign(self.process)
            require(assigned, 'job assignment failed; no work released')
            self.process.stdin.write(b'GO\n')
            self.process.stdin.flush()
            self.process.stdin.close()
        except BaseException:
            self.job.close()
            if self.process is not None:
                if not assigned:
                    self.process.kill() # Unassigned wrapper has not received GO.
                self.process.wait(timeout=max(.01, min(5, deadline+5-time.monotonic())))
            raise
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        size = 0
        try:
            while raw := self.process.stdout.readline(16385):
                size += len(raw)
                if len(raw) > 16384 or size > 131072 or b'\0' in raw:
                    raise ValueError('stdout overflow/binary')
                line = raw.decode('utf-8').replace('\r\n', '\n')
                self.lines.append(line)
                self.queue.put(line)
        except BaseException as error:
            self.queue.put(error)
        finally:
            self.queue.put(None)

    def next(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('owner deadline')
        item = self.queue.get(timeout=remaining)
        if isinstance(item, BaseException):
            raise item
        return item

    def close(self):
        cleanup_deadline = min(self.deadline + 5, time.monotonic() + 5)
        self.job.close()
        try:
            self.process.wait(timeout=max(0.01, cleanup_deadline - time.monotonic()))
        finally:
            self.reader.join(timeout=max(0, cleanup_deadline - time.monotonic()))
            require(not self.reader.is_alive(), 'capture reader did not finish within owned cleanup')


def run_missing_case(command, host, source, parent_env, root, deadline):
    root.mkdir(exist_ok=False)
    env = parent_env.copy()
    for variable, directory in [('LOCALAPPDATA','local'), ('APPDATA','roaming'), ('TEMP','temp'), ('TMP','temp')]:
        path = root/directory
        path.mkdir(exist_ok=True)
        env[variable] = str(path)
    saves = root/'local/SparkEngine/Saves'
    require(not saves.exists(), 'negative profile root not fresh')
    replay = Replay(saves, missing_only=True)
    child = Child(command, host.parent, env, source, deadline)
    window, held = None, False
    deferred, pending = [], deque()
    report = {'case':'missing-slot', 'passed':False}
    try:
        def frame():
            nonlocal window
            while True:
                line = pending.popleft() if pending else child.next()
                require(line is not None, 'negative child ended early')
                if line.startswith('SPARK_INPUT_CHILD '):
                    require(window is None and re.fullmatch(r'SPARK_INPUT_CHILD [0-9]+\n',line), 'negative PID receipt')
                    window = OwnedWindow(int(line.split()[1]), host)
                    pending.extend(deferred)
                    deferred.clear()
                    continue
                if window is None:
                    deferred.append(line)
                    continue
                result = replay.feed(line)
                if result is not None:
                    window.validate()
                    return result
        initial = frame()
        require(initial['mask'] == initial['pressed'] == replay.operation == 0, 'negative initial keys')
        require(not list(saves.glob('fps_quicksave*')), 'negative inherited save')
        window.send(2, True)
        held = True
        while True:
            current = frame()
            if current['input'] > initial['input'] and current['mask'] == 2:
                require(current['pressed'] == 2 and current['action'] == 2 and current['result'] == 0 and
                        current['reason'] == 1 and replay.operation == 1, 'negative consumed F3 receipt')
                break
        operation = replay.operations[0]
        require(not list(saves.glob('fps_quicksave*')), 'negative created save')
        window.send(2, False)
        held = False
        anchor = current['input']
        while True:
            current = frame()
            if current['input'] > anchor and current['mask'] == 0:
                require(current['released'] == 2, 'negative missing release')
                break
        window.close_window()
        while (line := child.next()) is not None:
            replay.feed(line)
        require(child.process.wait(timeout=max(.1, deadline-time.monotonic())) == 0, 'negative engine exit')
        require(replay.ended and replay.operation == 1, 'negative terminal/extra operation')
        require(not list(saves.glob('fps_quicksave*')), 'negative save appeared')
        check_lifecycle(child.lines)
        report.update(passed=True, pid=window.pid, processStart=window.start, hwnd=window.hwnd,
                      operation=operation, initialProfile=replay.frames[0]['profile'])
    finally:
        if window:
            if held:
                try:
                    window.send(2,False)
                except Exception:
                    pass
            window.close_handle()
        try:
            child.close()
        finally:
            (root/'game.log').write_text(''.join(child.lines),encoding='utf-8')
            (root/'receipt.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    return report


def check_lifecycle(lines):
    devices = [s.strip() for s in lines if 'SPARK_D3D11_DEVICE' in s]
    require(devices == ['SPARK_D3D11_DEVICE driver=warp certification=software-only'], 'WARP identity')
    lifecycle = [s.strip() for s in lines if 'SPARK_MODULE_LIFECYCLE' in s]
    require(len(lifecycle) == 1, 'lifecycle count')
    match = re.fullmatch(r'SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=(\d+) load=(\d+) update=(\d+) fixed=(\d+) render=(\d+) unload=(\d+) destroy=(\d+) faults=0', lifecycle[0])
    require(match and all(int(x) > 0 for x in match.groups()), 'lifecycle incomplete/faulted')
    require(all(int(match[i]) == 1 for i in (1,2,6,7)), 'duplicate lifecycle create/load/unload/destroy')


def run(args):
    require(os.name == 'nt', 'Windows required')
    start = time.monotonic()
    deadline = start + 85 # Final five seconds reserved for owned cleanup within90s.
    source, host, module, decoder = [plain(p) for p in
                                    (args.source, args.host, args.module, args.decoder)]
    require(subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True, timeout=5).strip() == args.source_sha,
            'source pin mismatch')
    require(not subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], timeout=5), 'dirty source')
    require(host.name == 'SparkEngine.exe' and module.name == 'SparkGameFPS.dll' and module.parent == host.parent,
            'installed actual FPS paths')
    helper_path = plain(args.decoder_helper)
    require(digest(helper_path) == args.decoder_helper_sha256, 'decoder helper pin mismatch')
    root = Path(args.out)
    require(root.is_absolute() and '..' not in root.parts and str(root).isascii(), 'plain output root')
    plain(root.parent)
    root.mkdir(parents=True, exist_ok=False)
    require(not (host.parent / 'Saves').exists(), 'inherited cwd Saves migration source')
    env = os.environ.copy()
    for variable, name in [('LOCALAPPDATA', 'local'), ('APPDATA', 'roaming'), ('TEMP', 'temp'), ('TMP', 'temp')]:
        path = root / name
        path.mkdir(exist_ok=True)
        env[variable] = str(path)
    env.update(SPARK_FPS_INPUT_TRACE='1', SPARK_RHI_BACKEND='d3d11', SPARK_D3D11_DRIVER='warp')
    saves = root / 'local/SparkEngine/Saves'
    replay = Replay(saves)
    identities = {str(p): digest(p) for p in (host, module, decoder)}
    command = [host, '-game', module, '-require-game', '-test-seconds', '30', '-threads', '2',
               '-window-size', '640x360', '-no-subprocess']
    negative = run_missing_case(command, host, source, env, root/'missing-slot', deadline)
    child = Child(command, host.parent, env, source, deadline)
    window, held, sends = None, set(), []
    receipt = {'source': args.source_sha, 'identities': identities, 'scope': 'owned-window-local-profile',
               'fullShippingQualification': False, 'missingSlot': negative, 'passed': False}
    try:
        deferred, pending_lines = [], deque()
        def next_frame():
            nonlocal window
            while True:
                line = pending_lines.popleft() if pending_lines else child.next()
                require(line is not None, 'child ended before acceptance')
                if line.startswith('SPARK_INPUT_CHILD '):
                    require(window is None and re.fullmatch(r'SPARK_INPUT_CHILD [0-9]+\n', line), 'child identity record')
                    window = OwnedWindow(int(line.split()[1]), host)
                    pending_lines.extend(deferred)
                    deferred.clear()
                    continue
                if window is None:
                    deferred.append(line)
                    continue
                frame = replay.feed(line)
                if frame is not None:
                    require(window is not None, 'missing process owner')
                    window.validate()
                    return frame

        current = next_frame()
        require(current['mask'] == 0 and current['pressed'] == 0 and replay.operation == 0, 'initial input not released')

        def transition(bit, down):
            nonlocal current
            anchor = current['input']
            window.send(bit, down)
            held.add(bit) if down else held.discard(bit)
            sends.append({'inputAfter': anchor, 'bit': bit, 'down': down, 'pid': window.pid, 'hwnd': window.hwnd})
            mask = sum(held)
            while True:
                current = next_frame()
                if current['input'] > anchor and current['mask'] == mask:
                    require(current['pressed' if down else 'released'] & bit, 'missing accepted edge')
                    return current

        scout = transition(4, True)['profile']
        require(scout['fps.profile.class'] == SCOUT, 'F5 did not select source-defined Scout')
        transition(4, False)
        saved_frame = transition(1, True)
        require(saved_frame['action'] == 1 and saved_frame['result'] == 1, 'save dispatch failed')
        saved = saved_frame['transfer']
        require(saved['fps.profile.class'] == scout['fps.profile.class'], 'saved wrong class')
        for _ in range(2):
            previous = current
            current = next_frame()
            require(current['input'] == previous['input'] + 1 and
                    current['update'] == previous['update'] + 1, 'nonconsecutive held save frames')
            require(current['mask'] == 1 and current['action'] == 0 and replay.operation == 1, 'held save repeated')
        slot = plain(saves / 'fps_quicksave.spark_save')
        require(slot.is_file() and not slot.with_suffix(slot.suffix + '.bak').exists(), 'missing/fallback save')
        copied = root / 'saved-primary.spark_save'
        slot_hash = copy_primary(slot, copied)
        transition(1, False)
        mutated = transition(8, True)['profile']
        require(mutated['fps.profile.class'] == VANGUARD and mutated['fps.profile.class'] != saved['fps.profile.class'],
                'F9 did not select source-defined Vanguard')
        transition(8, False)
        for load_number in (2, 3):
            loaded = transition(2, True)
            require(loaded['action'] == 2 and loaded['result'] == 1 and replay.operation == load_number, 'load dispatch')
            op = replay.operations[-1]
            require(op['transfer'] == saved and op['profile'] == saved, 'immediate load profile mismatch')
            restored_frame(loaded, saved, loaded['update'])
            for _ in range(2):
                previous = current
                current = next_frame()
                require(current['input'] == previous['input'] + 1 and
                        current['update'] == previous['update'] + 1, 'nonconsecutive held load frames')
                require(current['mask'] == 2 and current['action'] == 0 and replay.operation == load_number, 'held load repeated')
                restored_frame(current, saved, loaded['update'])
            restored_frame(transition(2, False), saved, loaded['update'])
        window.close_window()
        while (line := child.next()) is not None:
            replay.feed(line)
        require(child.process.wait(timeout=max(0.1, deadline-time.monotonic())) == 0, 'engine exit')
        require(replay.ended and [r['action'] for r in replay.operations] == [1, 2, 2], 'terminal/actions')
        check_lifecycle(child.lines)
        require(digest(slot) == digest(copied) == slot_hash, 'load rewrote save')
        receipt.update(pid=window.pid, processStart=window.start, hwnd=window.hwnd, sends=sends,
                       savedProfile=saved, slotSha256=slot_hash)
    finally:
        if window:
            for bit in tuple(held):
                try:
                    window.send(bit, False)
                except Exception:
                    pass # Never redirect cleanup to another window.
            window.close_handle()
        try:
            child.close()
        finally:
            (root / 'game.log').write_text(''.join(child.lines), encoding='utf-8')
            (root / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')

    def run_child(command, cwd, environment, timeout):
        isolated = env.copy()
        isolated.pop('SPARK_FPS_INPUT_TRACE', None)
        isolated.update(environment)
        isolated.pop('SPARK_FPS_INPUT_TRACE', None)
        for key in ('LOCALAPPDATA', 'APPDATA', 'TEMP', 'TMP'):
            isolated[key] = env[key]
        process = Child(command, cwd, isolated, source, min(deadline, time.monotonic() + timeout))
        try:
            while process.next() is not None:
                pass
            code = process.process.wait(timeout=max(.1, deadline-time.monotonic()))
            return subprocess.CompletedProcess(command, code, ''.join(process.lines), '')
        finally:
            process.close()

    helper = load_module('_fps_save_decoder', helper_path)
    decode_work, decoded = helper.decode_owned_primary(input_path=copied, owned_root=root,
                                                       tests_executable=decoder, deadline=deadline, run_child=run_child)
    require(decoded['canonicalProfile'] == saved, 'decoded saved content differs')
    require(all(digest(Path(p)) == h for p, h in identities.items()), 'image identity changed')
    require(time.monotonic() <= deadline, 'owner deadline exceeded during final identity checks')
    receipt.update(passed=True, decoded=decoded, decoderWork=str(decode_work),
                   decoderHelperSha256=args.decoder_helper_sha256, elapsedSeconds=time.monotonic()-start)
    (root / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'source-sha', 'host', 'module', 'decoder', 'decoder-helper', 'decoder-helper-sha256', 'out'):
        parser.add_argument('--' + name, required=True)
    print(json.dumps(run(parser.parse_args()), indent=2))
