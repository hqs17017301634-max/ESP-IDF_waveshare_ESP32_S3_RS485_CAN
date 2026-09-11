"""Offline differential verification. Uses only this project's isolated compiler.

Baseline is extracted from the exact source commit, never from another working
tree or the original project's test expectations. Outputs cover each real RX,
including repeated equal payloads and all supported vehicle modes.
"""
from pathlib import Path
import hashlib
import io
import json
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = '6e4f9e33c5c02eca4e83cd8a59a24a2802a5c3d8'
OUT = ROOT / '.tests' / 'frame-migration'
CXX = ROOT / '_tools/native-toolchain/w64devkit/bin/g++.exe'

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True, **kwargs)

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    baseline = OUT / 'baseline'
    archive = run(['git', 'archive', BASE, 'include'], stdout=subprocess.PIPE).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as src:
        src.extractall(baseline, filter='data')
    report = {'baseline_commit': BASE, 'compiler_sha256': hashlib.sha256(CXX.read_bytes()).hexdigest(), 'variants': []}
    for name, flags in [('dashboard_fsd252', ['-DESP32_DASHBOARD', '-DDASH_FSD_252_COMPAT=1']),
                        ('dashboard_builtin', ['-DESP32_DASHBOARD']),
                        ('native_optional', ['-DISA_SPEED_CHIME_SUPPRESS', '-DEMERGENCY_VEHICLE_DETECTION', '-DENHANCED_AUTOPILOT'])]:
        results = []
        for version in ('baseline', 'current'):
            exe = OUT / f'{name}-{version}.exe'
            includes = baseline/'include' if version == 'baseline' else ROOT/'include'
            run([CXX, '-std=c++17', '-O1', '-DNATIVE_BUILD', *flags,
                 *(['-DBASELINE'] if version == 'baseline' else []), '-I'+str(includes),
                 ROOT/'test/frame_sequence_probe.cpp', '-o', exe])
            data = run([exe], stdout=subprocess.PIPE).stdout
            (OUT/f'{name}-{version}.txt').write_bytes(data)
            results.append(data)
        if results[0] != results[1]:
            differences = [(i, a, b) for i, (a,b) in enumerate(zip(results[0].splitlines(),results[1].splitlines())) if a != b]
            raise AssertionError(f'{name}: output mismatch {differences[:5]}')
        entry = {'name': name, 'rx_events': len(results[0].splitlines()), 'output_sha256': hashlib.sha256(results[1]).hexdigest(), 'equal': True}
        report['variants'].append(entry)
        print(json.dumps(entry))
    contracts = OUT/'contracts.exe'
    run([CXX, '-std=c++17', '-Wall', '-Wextra', '-DNATIVE_BUILD', '-DESP32_DASHBOARD', '-DDASH_FSD_252_COMPAT=1',
         '-I'+str(ROOT/'include'), ROOT/'test/test_native_frame_coordinator/test_frame_coordinator.cpp', '-o', contracts])
    run([contracts])
    report['contract_tests'] = 'passed'
    # Assert the architectural boundary in addition to exercising runtime output.
    handlers = (ROOT/'include/handlers.h').read_text(encoding='utf-8')
    dashboard = (ROOT/'include/web/mcp2515_dashboard.h').read_text(encoding='utf-8')
    assert 'CanDriver' not in handlers and 'sendCritical(' not in handlers
    assert 'sendCritical(' not in dashboard
    assert 'driver.sendCritical(' in (ROOT/'include/tx_broker.h').read_text(encoding='utf-8')
    (OUT/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print('Migration comparison and contract checks passed')

if __name__ == '__main__':
    main()
