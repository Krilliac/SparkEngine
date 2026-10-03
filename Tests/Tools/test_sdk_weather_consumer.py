"""Compiler-free checks using the actual CMake parsers and consumer injection."""
import hashlib
import json
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT/'Tests/PackageSmoke/RunInstalledSDKTemplate.cmake'
LIFECYCLE = ROOT/'cmake/RunSparkModuleProfileLifecycle.cmake'
DEVICE = 'SPARK_D3D11_DEVICE driver=warp certification=software-only\n'
COUNTS = ' create=1 load=1 update=3 fixed=2 render=3 unload=1 destroy=1 faults=0\n'

def bracket(value):
    return '[===['+value+']===]'


class WeatherConsumerContracts(unittest.TestCase):
    def cmake(self, body, good=True):
        with tempfile.TemporaryDirectory() as tmp:
            script=Path(tmp)/'test.cmake'
            script.write_text(body,encoding='utf-8')
            result=subprocess.run(['cmake','-P',str(script)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode==0,good,result.stdout+result.stderr)

    def test_weather_records_fail_closed(self):
        lines=['set(SPARK_SDK_WEATHER_PARSER_ONLY ON)', 'include('+bracket(RUNNER.as_posix())+')']
        null='SPARK_SDK_WEATHER module=SparkGeneratedGame available=0 accepted=0 invalid_rejected=0 clear_accepted=0 callback=OnLoad\n'
        live=null.replace('=0','=1')
        cases=[(null,0,True),(live,1,True),(null,1,False),(live,0,False),
               ('',1,False),(live+live,1,False),('[log] '+live,1,False),
               (live.replace('accepted=1','accepted=0'),1,False),
               (live.replace('SparkGeneratedGame','SparkGameFPS'),1,False),
               (live+'SPARK_SDK_WEATHER broken\n',1,False)]
        for text,available,good in cases:
            lines.append('_spark_sdk_template_weather_result('+bracket(text)+' "" '+str(available)+' error)')
            lines += [('if(NOT error STREQUAL "")' if good else 'if(error STREQUAL "")'),
                      'message(FATAL_ERROR "weather case verdict mismatch")','endif()']
        self.cmake('\n'.join(lines))

    def test_real_lifecycle_parser_keeps_default_and_checks_named_consumer(self):
        lines=['set(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY ON)', 'include('+bracket(LIFECYCLE.as_posix())+')']
        fps=DEVICE+'SPARK_MODULE_LIFECYCLE module=SparkGameFPS'+COUNTS
        generated=DEVICE+'SPARK_MODULE_LIFECYCLE module=SparkGeneratedGame'+COUNTS
        cases=[(fps,'',True),(generated,'SparkGeneratedGame',True),(generated,'',False),
               (fps,'SparkGeneratedGame',False),(DEVICE,'SparkGeneratedGame',False),
               (generated+generated,'SparkGeneratedGame',False),
               (generated.replace('fixed=2','fixed=0'),'SparkGeneratedGame',False),
               (generated.replace('faults=0','faults=1'),'SparkGeneratedGame',False),
               (generated,'bad.*',False)]
        for text,module,good in cases:
            arg=' '+bracket(module) if module else ''
            lines.append('_spark_validate_lifecycle_result(0 '+bracket(text)+' "" ok reason'+arg+')')
            lines += [('if(NOT ok)' if good else 'if(ok)'), 'message(FATAL_ERROR "lifecycle case verdict mismatch")','endif()']
        self.cmake('\n'.join(lines))

    def test_production_injection_uses_public_types_and_rejects_ambiguous_hooks(self):
        source=RUNNER.read_text(encoding='utf-8')
        start=source.index('if(SPARK_SDK_WEATHER_CONSUMER)\n    set(_consumer_header')
        block=source[start:source.index('\nset(_configure',start)]
        for original,good in [('m_context = context;\n',True),('no hook\n',False),('m_context = context;\nm_context = context;\n',False)]:
            with tempfile.TemporaryDirectory() as tmp:
                project=Path(tmp)
                (project/'Source').mkdir()
                header=project/'Source/GameModule.h'
                header.write_text(original)
                self.cmake('set(SPARK_SDK_WEATHER_CONSUMER ON)\nset(_source '+bracket(project.as_posix())+')\n'+block,good)
                if good:
                    text=header.read_text()
                    self.assertIn('#include <Spark/IWeatherService.h>',text)
                    self.assertIn('context->GetWeatherService()',text)
                    self.assertIn('Spark::WeatherPreset::Rain, 0.25f, 0.5f',text)
                    self.assertIn('static_cast<Spark::WeatherPreset>(99)',text)
                    self.assertNotIn('Graphics/WeatherSystem.h',text)

    def test_optional_route_preserves_headless_and_real_windowed_requirements(self):
        source=RUNNER.read_text(encoding='utf-8')
        self.assertIn('NOT CMAKE_HOST_WIN32 OR NOT _sdk_version STREQUAL "10"',source)
        self.assertIn('"${_engine}" -headless -game "${_images}" -require-game',source)
        windowed=source[source.index('# The real windowed host registers WeatherSystem'):]
        self.assertNotIn('-headless',windowed)
        self.assertIn('"SPARK_D3D11_DRIVER=warp"',windowed)
        self.assertIn('_weather_sidecar_before STREQUAL _weather_sidecar_after',windowed)
        self.assertIn('_weather_host_before STREQUAL _weather_host_after',windowed)
        self.assertIn('_image_sha256 STREQUAL _weather_module_after',windowed)

    def test_windows_producer_uses_real_unambiguous_teardown_record(self):
        source=(ROOT/'SparkEngine/Source/Core/SparkEngineWindows.cpp').read_text(encoding='utf-8')
        block=source[source.index('const ModuleManager::LifecycleEvidence evidence = ModuleManager::GetLastTeardownLifecycleEvidence();'):]
        self.assertIn('evidence.FindGameModule()',block)
        self.assertIn('ModuleManager::FormatLifecycleRecord(*record)',block)
        self.assertNotIn('FindModule("Spark Arena - Engine Showcase")',block)
        self.assertNotIn('module=SparkGeneratedGame',block)

    def test_weather_streams_are_complete_proof_not_diagnostic_tails(self):
        spec=importlib.util.spec_from_file_location('weather_collector',ROOT/'.github/scripts/qualify-installed-native.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        for size,good in [(20000,True),(131073,False)]:
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);(root/'sdk').mkdir()
                raw=b'w'*size
                (root/'sdk/runtime-weather-stdout.log').write_bytes(raw)
                if good:
                    module.compact(root)
                    self.assertEqual((root/'diagnostics-text/sdk/runtime-weather-stdout.log').read_bytes(),raw)
                else:
                    with self.assertRaises(ValueError):module.compact(root)
                    self.assertFalse((root/'diagnostics-text').exists())

    def test_mode_bound_complete_receipt_and_tamper_rejection(self):
        spec=importlib.util.spec_from_file_location('receipt_collector',ROOT/'.github/scripts/qualify-installed-native.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        source=RUNNER.read_text()
        start=source.index('    # Complete identity proof')
        writer=source[start:source.index('    message(STATUS "SPARK_SDK_WEATHER_RUNTIME',start)]
        # This unit owns consumer proof only; the permanent workflow tests exercise
        # the real combined focused+consumer collector, including missing proofs.
        with patch.object(module, 'focused_proof', return_value=None, create=True):
            for mutation in ('none','missing-receipt','missing-stream','tampered-stream','wrong-source','wrong-binding','wrong-result','duplicate-key','mode-omitted','missing-binding','failed-sdk'):
                with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as tmp:
                    root=Path(tmp);sdk=root/'sdk';sdk.mkdir()
                    identity=dict(source_sha='a'*40,workflow_sha='b'*40,run_id='123',run_attempt='1',host_sha256='c'*64,weather_consumer=True,focused={})
                    module.save(root/'identity.json',identity)
                    for weather,available in (('',0),('-weather',1)):
                        (sdk/f'runtime{weather}-stdout.log').write_text(f'SPARK_SDK_WEATHER module=SparkGeneratedGame available={available} accepted={available} invalid_rejected={available} clear_accepted={available} callback=OnLoad\n')
                        (sdk/f'runtime{weather}-stderr.log').write_text('x'*20000)
                    variables=dict(_scanned='2',SPARK_TEST_ROOT=sdk.as_posix(),_module_name='SparkGeneratedGame',SPARK_WEATHER_SOURCE_SHA='a'*40,SPARK_WEATHER_WORKFLOW_SHA='b'*40,SPARK_WEATHER_RUN_ID='123',SPARK_WEATHER_RUN_ATTEMPT='1',_weather_consumer_sha256='d'*64,_weather_host_after='c'*64,_image_sha256='e'*64,_weather_sidecar_after='f'*64)
                    self.cmake('\n'.join('set('+key+' '+bracket(value)+')' for key,value in variables.items())+'\n'+writer)
                    receipt=sdk/'weather-identity.json'
                    boundary=json.loads(receipt.read_text())
                    self.assertIs(type(boundary['boundary_scanned']),int)
                    self.assertEqual(boundary['boundary_scanned'],2)
                    self.assertIs(type(boundary['boundary_violations']),int)
                    self.assertEqual(boundary['boundary_violations'],0)
                    binding=dict(passed=True,source_sha='a'*40,host_sha256='c'*64,weather_identity_sha256=module.digest(receipt))
                    module.save(root/'sdk-binding.json',binding)
                    if mutation=='missing-receipt':receipt.unlink()
                    if mutation=='missing-stream':(sdk/'runtime-weather-stderr.log').unlink()
                    if mutation=='tampered-stream':(sdk/'runtime-weather-stderr.log').write_text('tampered')
                    if mutation=='wrong-source':
                        value=json.loads(receipt.read_text());value['source_sha']='0'*40;module.save(receipt,value)
                    if mutation=='wrong-binding':binding['weather_identity_sha256']='0'*64;module.save(root/'sdk-binding.json',binding)
                    if mutation=='wrong-result':
                        path=sdk/'runtime-weather-stdout.log';path.write_text(path.read_text().replace('accepted=1','accepted=0'))
                        value=json.loads(receipt.read_text());value['streams'][path.name]=module.digest(path);module.save(receipt,value)
                    if mutation=='duplicate-key':receipt.write_text(receipt.read_text().replace('{','{"module":"bad",',1))
                    if mutation=='missing-binding':(root/'sdk-binding.json').unlink()
                    if mutation=='failed-sdk':
                        (root/'sdk-binding.json').unlink();receipt.unlink();module.save(root/'sdk-failure.json',dict(error='fixture failure'))
                    if mutation in ('none','failed-sdk'):
                        module.compact(root,True,'a'*40)
                        self.assertEqual((root/'diagnostics-text/sdk/runtime-weather-stderr.log').stat().st_size,20000)
                        if mutation=='none':self.assertEqual((root/'diagnostics-text/sdk/weather-identity.json').read_bytes(),receipt.read_bytes())
                    else:
                        with self.assertRaises((ValueError,FileNotFoundError)):
                            module.compact(root,mutation!='mode-omitted','a'*40)
                        self.assertFalse((root/'diagnostics-text').exists())

    def test_current_weather_source_and_run_cannot_drift(self):
        spec=importlib.util.spec_from_file_location('identity_collector',ROOT/'.github/scripts/qualify-installed-native.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        identity=dict(source_sha='a'*40,workflow_sha='b'*40,run_id='123',run_attempt='1')
        env=dict(GITHUB_SHA='b'*40,GITHUB_RUN_ID='123',GITHUB_RUN_ATTEMPT='1')
        for mutation in ('none','head','dirty','workflow','run','attempt'):
            changed=dict(env)
            if mutation in ('workflow','run','attempt'):
                changed[dict(workflow='GITHUB_SHA',run='GITHUB_RUN_ID',attempt='GITHUB_RUN_ATTEMPT')[mutation]]='wrong'
            def git(*args):
                if args[0]=='rev-parse':return ('c' if mutation=='head' else 'a')*40
                return ' M tracked.cpp' if mutation=='dirty' else ''
            with patch.dict(module.os.environ,changed):
                if mutation=='none':module.weather_source_identity(identity,git)
                else:
                    with self.assertRaises(ValueError):module.weather_source_identity(identity,git)

if __name__=='__main__': unittest.main()
