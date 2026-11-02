#!/usr/bin/env python3
"""Native SAI BRIDGE_PORT NH/NHG lifecycle check; no DVS, network namespaces or privileged runner."""
import argparse
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sai',type=Path,required=True)
p.add_argument('--runtime',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
source=Path(__file__).with_name('bpprot_nhg_vs.cpp')
subprocess.run(['c++','-std=c++14','-Wall','-Wextra','-Werror',*[f'-I{a.sai/x}' for x in ['inc','experimental','meta']],str(source),f'-L{a.runtime/"lib"}',f'-Wl,-rpath,{a.runtime/"lib"}','-lsaivs','-lsaimetadata','-o',str(a.output)],check=True)
subprocess.run([str(a.output)],env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime/'lib')),check=True)
