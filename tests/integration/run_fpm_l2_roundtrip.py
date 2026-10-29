#!/usr/bin/env python3
"""Exercise actual proposed C serializers and SWSS decoder; no zebra getter wiring."""
import argparse
from pathlib import Path
import re
import subprocess
import os
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--buildimage',type=Path,required=True)
p.add_argument('--binary',type=Path,required=True)
p.add_argument('--report-dir',type=Path,required=True)
a=p.parse_args();a.report_dir.mkdir(parents=True,exist_ok=True)
patch=(a.buildimage/'src/sonic-frr/patch/0077-zebra-Add-FPM-EVPN-MH-message-types-and-structures.patch').read_text()
added='\n'.join(x[1:] for x in patch.splitlines() if x.startswith('+') and not x.startswith('+++'))
start=added.index('/* FPM-specific extension values');end=added.index('#define FPM_SHL_IPV4_ADDR')
contract=added[start:end]
header=a.buildimage/'src/sonic-frr/dplane_fpm_sonic/bridge_port_protection_fpm.h'
program=a.report_dir/'encoder.c'
program.write_text('#include <netinet/in.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <linux/netlink.h>\n#include <stdio.h>\n#include <assert.h>\n#include <arpa/inet.h>\n'+contract+'\n#include "'+str(header.resolve())+'"\n'+r'''
static void emit(FILE *out, void *buf, ssize_t size) {
    unsigned char fpm[4]={1,1,0,0};
    uint16_t length=htons((uint16_t)(size+4)); memcpy(fpm+2,&length,2);
    assert(fwrite(fpm,4,1,out)==1); assert(fwrite(buf,(size_t)size,1,out)==1);
}
int main(int argc,char **argv) {
    assert(argc==2); FILE *out=fopen(argv[1],"wb"); assert(out);
    union { struct nlmsghdr align; unsigned char bytes[16408]; } buf;
    struct l2_nhg_member_msg member={0}; member.l2nm_nh_id=0x40000007;
    member.l2nm_type=1; member.l2nm_family=AF_INET;
    inet_pton(AF_INET,"10.0.0.2",&member.l2nm_remote_vtep);
    member.l2nm_router_mac[0]=2; member.l2nm_router_mac[5]=3;
    assert(sonic_fpm_encode_member(&member,false,buf.bytes,43)==-1);
    ssize_t size=sonic_fpm_encode_member(&member,false,buf.bytes,sizeof(buf)); assert(size==44); emit(out,buf.bytes,size);
    uint32_t ids[]={0x40000008,0x40000007};
    assert(sonic_fpm_encode_group(0x80000001,ids,UINT32_MAX,false,buf.bytes,sizeof(buf))==-1);
    assert(sonic_fpm_encode_group(0x80000001,ids,2,false,buf.bytes,31)==-1);
    size=sonic_fpm_encode_group(0x80000001,ids,2,false,buf.bytes,sizeof(buf)); assert(size==32); emit(out,buf.bytes,size);
    uint32_t maximum[L2_NHG_MAX_MEMBERS]; for(uint32_t i=0;i<L2_NHG_MAX_MEMBERS;i++) maximum[i]=i+1;
    assert(sonic_fpm_encode_group(1,maximum,L2_NHG_MAX_MEMBERS,false,buf.bytes,sizeof(buf))==16408);
    uint32_t equal[]={1,1}; assert(sonic_fpm_encode_group(2,equal,2,false,buf.bytes,sizeof(buf))==32);
    size=sonic_fpm_encode_group(0x80000001,NULL,0,true,buf.bytes,sizeof(buf)); emit(out,buf.bytes,size);
    size=sonic_fpm_encode_member(&member,true,buf.bytes,sizeof(buf)); emit(out,buf.bytes,size);
    assert(fclose(out)==0); return 0;
}
''')
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','--coverage','-O0',str(program),'-o',str(a.report_dir/'encoder')],check=True)
frames=a.report_dir/'frames.bin'
subprocess.run([str((a.report_dir/'encoder').resolve()),str(frames.resolve())],check=True)
subprocess.run(['gcov','-b','-c',str(program)],cwd=a.report_dir,check=True)
env=dict(os.environ,SONIC_FPM_L2_FIXTURES=str(frames.resolve()))
subprocess.run([str(a.binary.resolve()),'--gtest_filter=FpmBindingTest.ActualL2SerializersThroughFpmLink'],cwd=a.binary.resolve().parent,env=env,check=True)
