#!/usr/bin/env python3
"""Check the approved Primary WARP images and three disabled-pass controls."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, re

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exe',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('capture',root/'Tools/rhi210_capture.py')
capture=importlib.util.module_from_spec(spec); spec.loader.exec_module(capture)
capture.TIMEOUT_SECONDS=120
approved={'Primary_DeferredGeometry':'6e25c22bd2c2d929bf5e851292b333e5bd3df31d4236b826697ad16a7db420ac','Primary_DeferredLighting':'6f88f8c2a024afe456a6d1923493a4baa70cd11a90f46eea65f4b46dd63680ae','Primary_ShadowDepth':'b083f6cec0dab16b593b127b3ee43c7ab3828a9665762fb2da1ce2fb3b7451b3'}
# Exact hashes retain the owner's approved images; no baseline generation.
for scene,digest in approved.items():
    assert hashlib.sha256((root/'Tests/GoldenImages/d3d11-warp'/f'{scene}.png').read_bytes()).hexdigest()==digest,scene
exe=args.exe.resolve(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=False)
records=[]
for scene in [None,*approved]:
    token=None if scene is None else capture.MUTANTS[scene][0]
    folder=out/('normal' if scene is None else token); folder.mkdir()
    result=capture._run_one(exe,folder,'TestRHI210D3D11PrimaryGoldenReal.cpp',4,disable=token,junit_path=folder/'tests.xml')
    capture._write_result(folder/'tests.log',result)
    capture._validate_capture_result(result,folder.name,'TestRHI210D3D11PrimaryGoldenReal.cpp',4,mutant_scene=scene)
    verdicts=[line for line in result['stdout'].splitlines() if '[RHI-210 GOLDEN] scene=' in line]
    assert len(verdicts)==3,verdicts
    if scene is None:
        assert result['returncode']==0 and all('matched=yes' in line and 'threshold=0.00' in line for line in verdicts)
    else:
        target=next(line for line in verdicts if 'scene='+scene+' ' in line)
        assert result['returncode']==1 and re.search(r'matched=no differing=[1-9]\d*/',target) and 'threshold=0.00' in target,target
        assert all('matched=yes' in line for line in verdicts if 'scene='+scene+' ' not in line),verdicts
    records.append({'scene':scene,'exitCode':result['returncode'],'verdicts':verdicts})
    print(*verdicts,sep='\n',flush=True)
(out/'results.json').write_text(json.dumps({'binarySha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'runs':records},indent=2)+'\n',encoding='utf-8')
