#!/usr/bin/env python3
"""Actual production FPM objects + private Redis; no mocks, DVS or SAI calls."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--sai',type=Path,required=True)
    p.add_argument('--runtime',type=Path,required=True)
    args=p.parse_args()
    root=Path(__file__).resolve().parents[2]
    directory=root/'fpmsyncd'
    with tempfile.TemporaryDirectory(prefix='fpm-hld-redis-') as temporary:
        work=Path(temporary); socket=work/'redis.sock'; config=work/'database_config.json'
        db_config=json.loads((root/'tests/mock_tests/database_config.json').read_text())
        for instance in db_config['INSTANCES'].values():
            instance.update(hostname='127.0.0.1',port=0,unix_socket_path=str(socket))
        config.write_text(json.dumps(db_config))
        server=subprocess.Popen(['redis-server','--port','0','--unixsocket',str(socket),
                                 '--save','','--appendonly','no'],stdout=subprocess.DEVNULL)
        try:
            due=time.monotonic()+5
            while not socket.exists():
                if server.poll() is not None or time.monotonic()>due: raise RuntimeError('Private Redis not ready')
                time.sleep(0.01)
            obj=work/'driver.o'; binary=work/'test'
            includes=[root,root/'lib',root/'warmrestart',args.sai/'inc',Path('/usr/include/swss'),Path('/usr/include/libnl3')]
            # Match the project's existing header warning exceptions.
            subprocess.run(['c++','-std=c++14','-Wall','-Wextra','-Werror',
                            '-Wno-unused-parameter','-Wno-reorder','-UNDEBUG',
                            *[f'-I{x}' for x in includes],'-c',str(Path(__file__).with_name('fpm_hld_rows_redis.cpp')),
                            '-o',str(obj)],check=True)
            lines=subprocess.check_output(['bash','/dev-tools/make-with-protection-sai.sh','-n','-W','fpmsyncd.cpp','fpmsyncd'],cwd=directory,text=True).splitlines()
            links=[line for line in lines if '--mode=link' in line and ' -o fpmsyncd ' in line]
            assert len(links)==1, lines
            link=links[0].replace(' -o fpmsyncd ',' -o '+shlex.quote(str(binary))+' ')
            link=link.replace(' fpmsyncd-fpmsyncd.o ',' '+shlex.quote(str(obj))+' ')
            subprocess.run(link,shell=True,cwd=directory,check=True)
            result=subprocess.run([str(binary),str(config)],
                                  env=dict(os.environ,LD_LIBRARY_PATH=str(args.runtime/'lib')))
            if result.returncode:
                subprocess.run(['redis-cli','-s',str(socket),'-n','0','--scan'],check=True)
                result.check_returncode()
        finally:
            server.terminate(); server.wait(timeout=5)


if __name__=='__main__': main()
