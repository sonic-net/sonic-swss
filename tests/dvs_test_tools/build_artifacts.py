#!/usr/bin/env python3
"""Compile DVS-only artifacts; this does NOT create or execute a DVS testbed.

Uses a synthetic reference encoder compiled against the SWSS shared header.
This does not validate a live FRR producer or its dispatcher/getters.
The matching SAI headers must be the same ones used for the DVS overlay.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--sai', required=True, type=Path)
    p.add_argument('--buildimage', type=Path, help='Deprecated; the reference encoder uses the SWSS header')
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    tools = Path(__file__).resolve().parent
    swss = tools.parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    cflags = ['-std=c++14', '-Wall', '-Wextra', '-Werror', '-fPIC']
    includes = [f'-I{args.sai/x}' for x in ('inc','experimental','meta')]
    subprocess.run(['c++', *cflags, *includes, '-shared', str(tools/'sai_fault_shim.cpp'),
                    '-ldl', '-pthread', '-o', str(args.output/'libbpp_dvs_fault.so')], check=True)
    probe = args.output/'sai_constants.cpp'
    constants = {
        'status_failure': 'SAI_STATUS_FAILURE',
        'status_not_supported': 'SAI_STATUS_NOT_SUPPORTED',
        'status_not_implemented': 'SAI_STATUS_NOT_IMPLEMENTED',
        'admin_primary': 'SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY',
        'admin_protection': 'SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION',
        'state_primary': 'SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY',
        'state_protection': 'SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION',
        'attr_admin': 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE',
        'attr_mode': 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE',
        'state_not_applicable': 'SAI_BRIDGE_PORT_PROTECTION_STATE_NOT_APPLICABLE',
        'attr_bp_admin_state': 'SAI_BRIDGE_PORT_ATTR_ADMIN_STATE',
        'attr_group': 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID',
    }
    probe.write_text('#include <sai.h>\n#include <iostream>\nint main(){\n' +
                     '\n'.join(f'std::cout << "{key}=" << {value} << "\\n";' for key,value in constants.items()) + '\n}\n')
    subprocess.run(['c++', *cflags, *includes, str(probe), '-o', str(args.output/'sai_constants')], check=True)
    output = subprocess.check_output([str((args.output/'sai_constants').resolve())], text=True)
    manifest = {'constants': {k:int(v) for k,v in (line.split('=') for line in output.splitlines())}}
    header = swss/'fpmsyncd/fpm/fpm.h'
    encoder = tools/'fpm_encoder.cpp'
    subprocess.run(['c++', '-std=c++14', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(swss), str(encoder), '-o', str(args.output/'fpm_encoder')], check=True)
    manifest['sources'] = {str(path):hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (encoder,header,args.sai/'inc/saibridge.h',tools/'sai_fault_shim.cpp',Path(__file__))}
    manifest['scope'] = 'Synthetic reference wire encoder using the SWSS contract; not live FRR integration.'
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__ == '__main__':
    main()
