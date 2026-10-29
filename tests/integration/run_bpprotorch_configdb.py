#!/usr/bin/env python3
"""Compile/run the production protection consumer against isolated native Redis.

Requires configured SWSS dependencies, gtest and Python swsscommon bindings.
No mock Table/SubscriberStateTable, device database, or SAI fixture is used.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from swsscommon import swsscommon as swss

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sai', type=Path, required=True)
parser.add_argument('--runtime', type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="bp-configdb-") as directory:
    work = Path(directory)
    socket = work / "redis.sock"
    config = work / "database_config.json"
    # Full production globals also reference COUNTERS_DB/capability tables.
    # Preserve the standard fixture names/IDs and redirect every instance into
    # this private Redis socket before any native global is constructed.
    database_config = json.loads((root / 'tests/mock_tests/database_config.json').read_text())
    for instance in database_config['INSTANCES'].values():
        instance.update(hostname='127.0.0.1', port=0, unix_socket_path=str(socket))
    config.write_text(json.dumps(database_config))
    redis = subprocess.Popen(["redis-server", "--port", "0", "--unixsocket",
                              str(socket), "--save", "", "--appendonly", "no",
                              "--notify-keyspace-events", "KEA"],
                             stdout=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 5
        while not socket.exists():
            if redis.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Private Redis failed to start")
            time.sleep(0.01)
        swss.SonicDBConfig.initialize(str(config))
        connector = swss.ConfigDBConnector(use_unix_socket_path=True)
        connector.connect(wait_for_init=False)
        connector.set_entry("BRIDGE_PORT_PROTECTION", "PortChannel0", {})
        # Reuse the real production objects: BpProtOrch's vtable now includes
        # attachment reconciliation and its PortsOrch/L2NhgOrch dependencies.
        # No mock Table, SAI, subscriber or dependency stubs are linked here.
        includes = [args.sai / x for x in ('inc', 'experimental', 'meta')]
        includes += [root, root / 'orchagent', root / 'lib',
                     root / 'orchagent/flex_counter', root / 'orchagent/debug_counter',
                     root / 'warmrestart', '/usr/include/swss', '/usr/include/sai', '/usr/include/libnl3']
        obj = work / 'configdb.o'
        subprocess.run(['ccache', 'g++', '-std=c++14', '-O0', '-g',
                        *['-I' + str(path) for path in includes], '-c',
                        str(root / 'tests/integration/bpprotorch_configdb.cpp'),
                        '-o', str(obj)], check=True)
        orch_dir = root / 'orchagent'
        renamed_main = work / 'production_globals.o'
        subprocess.run(['objcopy', '--redefine-sym', 'main=unused_orchagent_main',
                        str(orch_dir / 'orchagent-main.o'), str(renamed_main)], check=True)
        commands = subprocess.check_output(
            ['bash', '/dev-tools/make-with-protection-sai.sh', '-n', '-W', 'main.cpp', 'orchagent'],
            cwd=orch_dir, text=True).splitlines()
        links = [line for line in commands if '--mode=link' in line and ' -o orchagent ' in line]
        if len(links) != 1:
            raise RuntimeError('Build production orchagent first; expected one link command')
        binary = work / 'test'
        import shlex
        link = links[0].replace(' -o orchagent ', ' -o ' + shlex.quote(str(binary)) + ' ')
        link = link.replace(' orchagent-main.o ',
                            ' ' + shlex.quote(str(renamed_main)) + ' ' + shlex.quote(str(obj)) + ' ')
        link += ' -lgtest'
        subprocess.run(link, shell=True, cwd=orch_dir, check=True)
        subprocess.run([str(binary), str(config)], env=dict(os.environ, LD_LIBRARY_PATH=str(args.runtime / 'lib'), BP_PROTECTION_TEST_DB_CONFIG=str(config)), check=True)
    finally:
        redis.terminate()
        redis.wait(timeout=5)
