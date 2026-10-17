"""Parser/source and normal capture regressions. Synthetic records are not native evidence."""
import copy
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace
import stat
import importlib.util
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('fps_input_dispatch', ROOT/'Tests/PackageSmoke/RunFPSInputDispatch.py')
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class WrapperCaptureTests(unittest.TestCase):
    def test_normal_child_stdout_stderr_and_stdin_are_captured(self):
        child = Path(sys.executable)
        if os.name == 'nt':
            child = child.with_name('pythonw.exe')
            self.assertTrue(child.is_file(), 'Windows GUI Python child is required')
        payload = ("import sys; print('CAPTURE_STDOUT',flush=True); "
                   "print('CAPTURE_STDERR',file=sys.stderr,flush=True); "
                   "print('CAPTURE_STDIN_EOF='+str(sys.stdin.buffer.read()==b''),flush=True)")
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        result = subprocess.run([sys.executable, '-c', driver.WRAPPER, str(child), '-c', payload],
                                input=b'GO\n', stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                creationflags=flags, timeout=10, check=True)
        lines = result.stdout.decode('utf-8').splitlines()
        self.assertEqual(len([line for line in lines if line.startswith('SPARK_INPUT_CHILD ')]), 1)
        self.assertEqual(sorted(line for line in lines if not line.startswith('SPARK_INPUT_CHILD ')),
                         ['CAPTURE_STDERR', 'CAPTURE_STDIN_EOF=True', 'CAPTURE_STDOUT'])

    def test_inner_launch_explicitly_routes_existing_binary_capture_streams(self):
        captured_out, captured_err = io.BytesIO(), io.BytesIO()
        child = SimpleNamespace(pid=1234, wait=mock.Mock(return_value=0))
        process_module = SimpleNamespace(Popen=mock.Mock(return_value=child), DEVNULL=subprocess.DEVNULL)
        system_module = SimpleNamespace(argv=['wrapper', 'normal-child'],
            stdin=SimpleNamespace(buffer=io.BytesIO(b'GO\n')),
            stdout=SimpleNamespace(buffer=captured_out), stderr=SimpleNamespace(buffer=captured_err),
            exit=mock.Mock(side_effect=SystemExit(0)))
        with mock.patch.dict(sys.modules, subprocess=process_module, sys=system_module), mock.patch('builtins.print'):
            with self.assertRaises(SystemExit) as exit_result:
                exec(driver.WRAPPER, {})
        self.assertEqual(exit_result.exception.code, 0)
        process_module.Popen.assert_called_once_with(['normal-child'], stdin=subprocess.DEVNULL,
                                                     stdout=captured_out, stderr=captured_err)
        child.wait.assert_called_once_with()


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parent / 'synthetic-save-root'
        self.replay = driver.Replay(self.root)

    def record(self, phase='before', **changes):
        p = 'fps.profile.class=0\nfps.profile.playTime=0.000000\n'.encode().hex()
        r = dict(v=1, phase=phase, input=1, update=1, mask=0, pressed=0, released=0,
                 paused=0, action=0, result=-1, reason=0, operation=0, faults=0, profile=p,
                 transfer='-', saves=str(self.root).encode().hex())
        r.update(changes)
        return 'SPARK_FPS_INPUT ' + ' '.join(f'{k}={r[k]}' for k in driver.TRACE_FIELDS) + '\n'

    def idle(self):
        for phase in ('before', 'dispatch', 'complete'):
            self.replay.feed(self.record(phase))

    def test_complete_idle_and_exact_terminal(self):
        self.idle()
        self.assertEqual(len(self.replay.frames), 1)
        self.replay.feed(f'SPARK_FPS_INPUT_END v=1 records=3 bytes={self.replay.byte_count} failed=0\n')
        self.assertTrue(self.replay.ended)

    def test_requires_actual_operation_result_and_transfer(self):
        for mutation in ({'result': 0}, {'transfer': '-'}, {'operation': 2}, {'action': 2}):
            with self.subTest(mutation=mutation):
                replay = driver.Replay(self.root)
                replay.feed(self.record(mask=1, pressed=1))
                fields = dict(mask=1, pressed=1, action=1, result=1, operation=1,
                              transfer='fps.profile.class=0\nfps.profile.playTime=0.000000\n'.encode().hex())
                fields.update(mutation)
                with self.assertRaises(ValueError):
                    replay.feed(self.record('operation', **fields))

    def test_rejects_cross_frame_dispatch(self):
        self.replay.feed(self.record())
        with self.assertRaises(ValueError):
            self.replay.feed(self.record('dispatch', input=2))

    def test_rejects_completion_without_dispatch(self):
        self.replay.feed(self.record())
        with self.assertRaises(ValueError):
            self.replay.feed(self.record('complete'))

    def test_rejects_faults_paused_and_wrong_save_root(self):
        for mutation in ({'faults': 1}, {'paused': 1}, {'saves': str(self.root/'wrong').encode().hex()}):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                driver.Replay(self.root).feed(self.record(**mutation))

    def test_rejects_duplicate_phase_and_stale_frame(self):
        self.replay.feed(self.record())
        with self.assertRaises(ValueError):
            self.replay.feed(self.record())
        self.replay = driver.Replay(self.root)
        self.idle()
        with self.assertRaises(ValueError):
            self.replay.feed(self.record())

    def test_rejects_unobserved_key_dispatch(self):
        self.replay.feed(self.record(mask=1, pressed=1))
        with self.assertRaises(ValueError):
            self.replay.feed(self.record('dispatch', mask=1, pressed=1))

    def test_rejects_nonfinite_and_duplicate_profile(self):
        for payload in ('fps.profile.class=nan\nfps.profile.playTime=0\n',
                        'fps.profile.class=0\nfps.profile.class=0\nfps.profile.playTime=0\n'):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                driver.parse_trace(self.record(profile=payload.encode().hex()))

    def test_rejects_prefixed_or_extra_fields(self):
        for line in ('[info] '+self.record(), self.record().rstrip()+' extra=1\n'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.replay.feed(line)

    def test_rejects_incomplete_or_overflow_terminal(self):
        self.idle()
        for line in ('SPARK_FPS_INPUT_END v=1 records=3 bytes=1 failed=0\n',
                     f'SPARK_FPS_INPUT_END v=1 records=3 bytes={self.replay.byte_count} failed=1\n'):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.replay.feed(line)

    def test_full_successful_save_frame(self):
        self.replay.feed(self.record(mask=1, pressed=1))
        fields = dict(mask=1, pressed=1, action=1, result=1, operation=1,
                      transfer='fps.profile.class=0\nfps.profile.playTime=0.000000\n'.encode().hex())
        for phase in ('operation', 'dispatch', 'complete'):
            self.replay.feed(self.record(phase, **fields))
        self.assertEqual(self.replay.operations[0]['action'], 1)
        self.assertEqual(self.replay.operation, 1)

    def test_missing_slot_requires_failure_reason_and_unchanged_profile(self):
        for change in ({}, {'result':1}, {'reason':0}, {'action':1},
                       {'profile':'fps.profile.class=4\nfps.profile.playTime=0.000000\n'.encode().hex()}):
            with self.subTest(change=change):
                replay = driver.Replay(self.root, missing_only=True)
                replay.feed(self.record(mask=2, pressed=2))
                fields = dict(mask=2, pressed=2, action=2, result=0, reason=1, operation=1)
                fields.update(change)
                if change:
                    with self.assertRaises(ValueError):
                        replay.feed(self.record('operation', **fields))
                else:
                    for phase in ('operation','dispatch','complete'):
                        replay.feed(self.record(phase, **fields))
                    self.assertEqual(replay.operations[0]['profile'], replay.operations[0]['beforeProfile'])

    def test_missing_save_binding_is_rejected(self):
        with self.assertRaises(ValueError):
            self.replay.feed(self.record(saves='-'))

    def test_owned_window_rejection_prevents_message(self):
        window = driver.OwnedWindow.__new__(driver.OwnedWindow)
        window.validate = mock.Mock(side_effect=ValueError('wrong PID/path/HWND'))
        window.u = mock.Mock()
        with self.assertRaises(ValueError):
            window.send(1, True)
        window.u.PostMessageW.assert_not_called()

    def test_expired_deadline_does_not_wait_for_frame(self):
        child = driver.Child.__new__(driver.Child)
        child.deadline = 0
        child.queue = mock.Mock()
        with self.assertRaises(TimeoutError):
            child.next()
        child.queue.get.assert_not_called()

    def test_restored_completed_frame_checks_every_field_and_clock(self):
        saved = {'fps.profile.class':'0', 'fps.profile.xp':'12', 'fps.profile.health':'75.000000',
                 'fps.profile.playTime':'1.000000'}
        valid = dict(saved, **{'fps.profile.playTime':'1.100000'})
        driver.restored_frame({'profile':valid,'update':5},saved,5)
        for changes in ({'fps.profile.xp':'13'}, {'fps.profile.health':'74.000000'},
                        {'fps.profile.playTime':'0.900000'}, {'fps.profile.playTime':'1.300000'},
                        {'fps.profile.playTime':'nan'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                driver.restored_frame({'profile':dict(valid,**changes),'update':5},saved,5)

    def test_oversize_copy_is_rejected_before_file_read(self):
        slot = mock.Mock()
        slot.stat.return_value = SimpleNamespace(st_mode=stat.S_IFREG,st_size=16*1024*1024+1)
        with mock.patch.object(driver,'plain',return_value=slot), self.assertRaises(ValueError):
            driver.copy_primary(self.root/'slot',self.root/'copy')
        slot.open.assert_not_called()

    def test_lifecycle_rejects_each_duplicate_unique_callback(self):
        marker = 'SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=4 fixed=2 render=4 unload=1 destroy=1 faults=0'
        device = 'SPARK_D3D11_DEVICE driver=warp certification=software-only'
        driver.check_lifecycle([device,marker])
        for field in ('create','load','unload','destroy'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                driver.check_lifecycle([device,marker.replace(' '+field+'=1',' '+field+'=2')])


class SourceContracts(unittest.TestCase):
    def test_observers_and_dispatch_are_bound_to_real_calls(self):
        root = ROOT
        game = (root/'GameModules/SparkGameFPS/Source/Game/Game.cpp').read_text(encoding='utf-8')
        systems = (root/'GameModules/SparkGameFPS/Source/Game/GameEngineSystems.cpp').read_text(encoding='utf-8')
        self.assertIn('if (savePressed != loadPressed)', game)
        self.assertLess(game.index('const bool succeeded = savePressed ? QuickSaveProfile'),
                        game.index('RecordInputObservation("operation")'))
        self.assertIn('if (m_inputObservationEnabled && !inputUpdateCompleted)', game)
        self.assertIn('GetRecordsSnapshot()', systems)
        self.assertIn('GetEnvironmentVariableW(L"SPARK_FPS_INPUT_TRACE", setting, 2) == 1', systems)
        self.assertIn("setting[0] == L'1'", systems)
        self.assertIn('profile.WriteTo(fields)', systems)
        self.assertIn('m_inputObservationRecords >= 256', systems)
        self.assertIn('m_inputObservationBytes + text.size() > 98304', systems)

    def test_no_global_input_or_engine_launch_on_import(self):
        source = Path(driver.__file__).read_text(encoding='utf-8')
        self.assertNotIn('.SendInput(', source)
        self.assertNotIn('.SetForegroundWindow(', source)
        self.assertNotIn('time.sleep(', source)
        self.assertIn('self.validate()\n        vk = KEYS[bit]', source)
        self.assertIn("'LOCALAPPDATA', 'APPDATA', 'TEMP', 'TMP'", source)
        self.assertIn("'-test-seconds', '30'", source)
        self.assertIn('deadline = start + 85', source)
        self.assertIn("current['input'] == previous['input'] + 1", source)
        self.assertIn("scout['fps.profile.class'] == SCOUT", source)
        self.assertIn("mutated['fps.profile.class'] == VANGUARD", source)
        self.assertIn('owner deadline exceeded during final identity checks',source)


if __name__ == '__main__':
    unittest.main()
