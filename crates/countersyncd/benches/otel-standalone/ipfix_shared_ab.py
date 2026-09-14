"""Alternate real IPFIX baseline/shared binaries; fail on checksum/count errors."""
import argparse,json,pathlib,subprocess,time,os,urllib.request
p=argparse.ArgumentParser()
p.add_argument('--old',required=True)
p.add_argument('--new',required=True)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--fields',type=int,nargs='+',default=[2,500,8000])
p.add_argument('--points',type=int,default=400000000)
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--raw-server',help='optional real IPFIX->OTel->raw gRPC integration')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
def stats():return json.load(urllib.request.urlopen('http://127.0.0.1:28889/stats',timeout=5))
for fields in a.fields:
    for round in range(a.rounds):
        order=[('old',a.old),('new',a.new)]
        if round%2:order.reverse()
        for name,binary in order:
            server=None;log=None
            env=dict(os.environ)
            try:
                if a.raw_server:
                    log=(a.output/f'server-{fields}-{round}-{name}.log').open('w')
                    server=subprocess.Popen(['taskset','-c','8,10,12,14',a.raw_server],env=dict(env,GOMAXPROCS='4'),stdout=log,stderr=log)
                    for _ in range(100):
                        if server.poll() is not None:raise RuntimeError('raw server exited')
                        try:assert stats()['requests']==0;break
                        except Exception:time.sleep(.1)
                    else:raise RuntimeError('raw server readiness timeout')
                    env['OTEL_EXTERNAL_ENDPOINT']='http://127.0.0.1:24317'
                run=subprocess.run(['/usr/bin/time','-f','wall=%e user=%U sys=%S maxrss_kib=%M',binary,str(fields),str(a.points),'1'],env=env,text=True,capture_output=True,timeout=240)
                if run.returncode:raise RuntimeError(run.stdout+run.stderr)
                received=stats() if server else None
                if received:
                    # Same object and stat identities as IPFIX helpers: 64 ports,
                    # stat=i%100. Complete per-lane batches plus tails.
                    counts=[0]*6
                    for i in range(fields):
                        h=0xcbf29ce484222325
                        for b in f'Ethernet{i%64}'.encode()+(1).to_bytes(4,'little')+(i%100).to_bytes(4,'little'):h=((h^b)*0x100000001b3)&0xffffffffffffffff
                        counts[h%6]+=1
                    expected=sum((n*(a.points//fields)+9999)//10000 for n in counts)
                    assert received['requests']==expected,(expected,received)
                row={'variant':name,'fields':fields,'points':a.points,'round':round,'stdout':run.stdout,'process_time':run.stderr,'received':received}
                print(json.dumps(row),flush=True)
                (a.output/f'{fields}-{round}-{name}-{time.time_ns()}.json').write_text(json.dumps(row,indent=2))
            finally:
                if server:server.terminate();server.wait(timeout=10);log.close()
