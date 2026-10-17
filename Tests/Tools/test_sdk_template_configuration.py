"""Exercise production SDK consumer arguments without enabling any compiler."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


RUNNER = Path(__file__).resolve().parents[1] / 'PackageSmoke/RunInstalledSDKTemplate.cmake'
CONFIG_ARGUMENT = '    "-DCMAKE_CONFIGURATION_TYPES=${SPARK_CONFIG}"\n'


class InstalledSDKConfiguration(unittest.TestCase):
    def test_requested_minsizerel_is_generated_by_production_arguments(self):
        cmake = shutil.which('cmake')
        ninja = shutil.which('ninja')
        self.assertIsNotNone(cmake, 'CMake is required for this compiler-free regression')
        self.assertIsNotNone(ninja, 'Ninja is required for this compiler-free regression')
        production = RUNNER.read_text(encoding='utf-8')
        start = production.index('set(_configure\n')
        end = production.index('_run_checked("Configure the installed', start)
        arguments = production[start:end]
        self.assertEqual(arguments.count(CONFIG_ARGUMENT), 1)
        env = os.environ.copy()
        # Even accidental language enablement must not select a real compiler.
        env.update(CC='spark-no-compiler-allowed', CXX='spark-no-compiler-allowed')
        with tempfile.TemporaryDirectory(prefix='spark-sdk-config-') as directory:
            root = Path(directory)
            source = root / 'source'
            source.mkdir()
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(SDKConfigurationRegression LANGUAGES NONE)\n'
                'add_custom_target(config_proof ALL COMMAND "${CMAKE_COMMAND}" -E touch '
                '"${CMAKE_BINARY_DIR}/proof-$<CONFIG>.txt")\n', encoding='utf-8')
            for case, actual_arguments in [('before', arguments.replace(CONFIG_ARGUMENT, '')),
                                           ('after', arguments)]:
                build = root / case
                script = root / (case + '.cmake')
                script.write_text(
                    'set(_source "' + source.as_posix() + '")\n'
                    'set(_build "' + build.as_posix() + '")\n'
                    'set(_prefix "' + (root / 'prefix').as_posix() + '")\n'
                    'set(SPARK_CONFIG MinSizeRel)\n'
                    'set(SPARK_CONSUMER_GENERATOR "Ninja Multi-Config")\n'
                    'set(SPARK_CONSUMER_MAKE_PROGRAM "' + Path(ninja).as_posix() + '")\n'
                    + actual_arguments + '\n'
                    'execute_process(COMMAND ${_configure} RESULT_VARIABLE result)\n'
                    'if(NOT result EQUAL 0)\n message(FATAL_ERROR "Configure failed: ${result}")\nendif()\n',
                    encoding='utf-8')
                configured = subprocess.run([cmake, '-P', str(script)], env=env, capture_output=True,
                                            text=True, timeout=30)
                self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                generated = (build / 'build-MinSizeRel.ninja').is_file()
                built = subprocess.run([cmake, '--build', str(build), '--config', 'MinSizeRel'],
                                       env=env, capture_output=True, text=True, timeout=30)
                if case == 'before':
                    self.assertFalse(generated)
                    self.assertNotEqual(built.returncode, 0)
                    self.assertIn('build-MinSizeRel.ninja', built.stdout + built.stderr)
                    print('BEFORE: configure succeeds; build-MinSizeRel.ninja absent; MinSizeRel build rejected')
                else:
                    self.assertTrue(generated)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    self.assertTrue((build / 'proof-MinSizeRel.txt').is_file())
                    print('AFTER: build-MinSizeRel.ninja exists; compiler-free MinSizeRel target executes')
                cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
                self.assertNotIn('CMAKE_CXX_COMPILER:', cache)
                self.assertNotIn('CMAKE_C_COMPILER:', cache)


if __name__ == '__main__':
    unittest.main()
