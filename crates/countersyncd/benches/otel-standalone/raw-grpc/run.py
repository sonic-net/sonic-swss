"""Linux raw gRPC transport benchmark; counts requests/bytes, NOT decoded points."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.request

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--server',required=True)
p.add_argument('--client',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--batch',type=int,default=10000)
p.add_argument('--points',type=int,default=40000000)
p.add_argument('--repeats',type=int,default=2)
p.add_argument('--inflight',type=int,nargs='+',default=[1,2,4,8,16])
p.add_argument('--server-cpus',default='14')
p.add_argument('--procs',default='1')
p.add_argument('--client-cpus',default='0')
p.add_argument('--multithread',action='store_true')
p.add_argument('--lanes',type=int,default=1)
p.add_argument('--pool',action='store_true')
p.add_argument('--router-cpu',type=int,default=0)
a=p.parse_args()
assert a.points%a.batch==0
a.output.mkdir(parents=True,exist_ok=True)
def stats():
    return json.load(urllib.request.urlopen('http://127.0.0.1:28889/stats',timeout=5))
for n in a.inflight:
    name=f'b{a.batch}-n{n}-p{a.procs}-{time.time_ns()}'
    with (a.output/(name+'.log')).open('w') as log:
        server=subprocess.Popen(['taskset','-c',a.server_cpus,a.server],env=dict(os.environ,GOMAXPROCS=a.procs),stdout=log,stderr=log)
        try:
            for _ in range(100):
                if server.poll() is not None:raise RuntimeError('raw server exited')
                try:before=stats();break
                except Exception:time.sleep(.1)
            else:raise RuntimeError('raw server readiness timed out')
            assert before=={'requests':0,'bytes':0},before
            start=time.monotonic()
            client_args=[str(a.batch),str(a.points),str(a.repeats)]
            if a.pool:
                client_args=['--threads',str(len(a.client_cpus.split(','))),'--cpus',a.client_cpus,'--in-flight-per-worker',str(n),
                             '--router-cpu',str(a.router_cpu),'--batch',str(a.batch),'--points',str(a.points),'--repeats',str(a.repeats)]
            result=subprocess.run([a.client,*client_args],env=dict(os.environ,OTEL_EXTERNAL_ENDPOINT='http://127.0.0.1:24317',OTEL_MAX_IN_FLIGHT=str(n),OTEL_CLIENT_CPUS=a.client_cpus,OTEL_LANES_PER_WORKER=str(a.lanes)),capture_output=True,text=True,timeout=600)
            result.check_returncode()
            after=stats()
            expected_requests=a.points//a.batch*a.repeats
            if a.multithread or a.pool:
                workers=len(a.client_cpus.split(','))*(n if a.pool else a.lanes);counts=[0]*workers
                for i in range(500):
                    h=0xcbf29ce484222325
                    for byte in f'Ethernet{i}'.encode()+(1).to_bytes(4,'little')+i.to_bytes(4,'little'):
                        h=((h^byte)*0x100000001b3)&0xffffffffffffffff
                    counts[h%workers]+=1
                expected_requests=sum((c*(a.points//500)+a.batch-1)//a.batch for c in counts)*a.repeats
            assert after['requests']==expected_requests,(after,expected_requests)
            # Independent byte-length comparison against prior FULLY DECODING
            # Collector runs of the exact same 500-series tagged workload.
            batch_bytes={10000:960333,100000:9201735,1000000:91605735}
            if not a.multithread and not a.pool and a.batch in batch_bytes:assert after['bytes']==batch_bytes[a.batch]*expected_requests,(after,batch_bytes[a.batch]*expected_requests)
            row={**vars(a),'output':str(a.output),'inflight':n,'stdout':result.stdout,'counters':after,'whole_run_wall_s':time.monotonic()-start}
            print(result.stdout,end='',flush=True);print(json.dumps(row),flush=True)
            (a.output/(name+'.json')).write_text(json.dumps(row,indent=2))
        finally:
            server.terminate();server.wait(timeout=20)
