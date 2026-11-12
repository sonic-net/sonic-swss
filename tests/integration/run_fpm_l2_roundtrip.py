#!/usr/bin/env python3
"""Exercise the shared-header reference wire encoder through the SWSS decoder."""
import argparse
from pathlib import Path
import os
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--buildimage', type=Path, help='Deprecated; no FRR source is used')
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--report-dir', type=Path, required=True)
a = p.parse_args()
a.report_dir.mkdir(parents=True, exist_ok=True)
swss = Path(__file__).resolve().parents[2]
encoder = a.report_dir/'fpm_encoder'
subprocess.run(['c++', '-std=c++14', '-Wall', '-Wextra', '-Werror', '-I'+str(swss),
                str(swss/'tests/dvs_test_tools/fpm_encoder.cpp'), '-o', str(encoder)], check=True)
commands = [
    ['member','0x40000007','add','1','10.0.0.2','02:00:00:00:00:03'],
    ['group','0x80000001','add','0x40000008@2001:db8::2@02:00:00:00:00:03',
                               '0x40000007@10.0.0.2@02:00:00:00:00:03'],
    ['group','0x80000001','del'],
    ['member','0x40000007','del','1','10.0.0.2','02:00:00:00:00:03'],
]
frames = a.report_dir/'frames.bin'
frames.write_bytes(b''.join(subprocess.check_output([str(encoder.resolve()),*c]) for c in commands))
subprocess.run([str(a.binary.resolve()), '--gtest_filter=FpmBindingTest.ReferenceGroupSerializerThroughFpmLink'],
    cwd=a.binary.resolve().parent if (a.binary.resolve().parent/'database_config.json').exists() else swss/'tests/mock_tests',
    env=dict(os.environ, SONIC_FPM_L2_FIXTURES=str(frames.resolve())), check=True)
