"""Run native suites with the project-local C++ compiler, optionally at baseline."""
import argparse
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = '6e4f9e33c5c02eca4e83cd8a59a24a2802a5c3d8'
CXX = ROOT/'_tools/native-toolchain/w64devkit/bin/g++.exe'
UNITY = ROOT/'.pio/libdeps/native/Unity/src'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--baseline',action='store_true')
    args=parser.parse_args()
    out=ROOT/'.tests'/('native-baseline' if args.baseline else 'native-current')
    out.mkdir(parents=True,exist_ok=True)
    tree=ROOT
    if args.baseline:
        tree=out/'source'
        archive=subprocess.check_output(['git','archive',BASE,'include','test'],cwd=ROOT)
        with tarfile.open(fileobj=io.BytesIO(archive)) as src:
            src.extractall(tree,filter='data')
    report=[]
    for suite in sorted((tree/'test').glob('test_native_*')):
        if suite.name=='test_native_frame_coordinator': continue # dedicated contract runner
        flags=['-DISA_SPEED_CHIME_SUPPRESS','-DEMERGENCY_VEHICLE_DETECTION','-DENHANCED_AUTOPILOT']
        if suite.name.endswith('dashboard'): flags+=['-DESP32_DASHBOARD','-DBYPASS_TLSSC_REQUIREMENT','-DNAG_KILLER']
        if suite.name.endswith('bypass_tlssc_requirement'): flags+=['-DBYPASS_TLSSC_REQUIREMENT']
        if suite.name.endswith('injection_after_ap'): flags=['-DENHANCED_AUTOPILOT','-DINJECTION_AFTER_AP']
        if suite.name.endswith('nag'): flags=['-DNAG_KILLER']
        if suite.name.endswith('mcp2515_recovery'): flags=['-DPIN_CAN_INTERRUPT=2','-I'+str(suite)]
        exe=out/(suite.name+'.exe')
        cmd=[str(CXX),'-std=c++17','-DNATIVE_BUILD',*flags,'-I'+str(tree/'include'),'-I'+str(UNITY),
             *map(str,suite.glob('*.cpp')),str(UNITY/'unity.c'),'-o',str(exe)]
        compile_result=subprocess.run(cmd,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        result=compile_result if compile_result.returncode else subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        text=result.stdout.decode('utf-8',errors='replace')
        (out/(suite.name+'.log')).write_text(text,encoding='utf-8')
        report.append({'suite':suite.name,'exit':result.returncode,'compiled':compile_result.returncode==0})
        print(suite.name,'PASS' if result.returncode==0 else 'FAIL')
        if result.returncode: print('\n'.join(text.splitlines()[:12]))
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    raise SystemExit(any(entry['exit'] for entry in report))

if __name__=='__main__': main()
