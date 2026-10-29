#!/usr/bin/env python3
"""Compile DVS-only artifacts; this does NOT create or execute a DVS testbed.

Requires a separately supplied companion FRR serializer checkout via
--buildimage; the schema-only buildimage PR does not supply those changes.
Uses its binding/Member/Group serializers with synthetic getters.
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
    p.add_argument('--buildimage', required=True, type=Path)
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
    source = args.buildimage/'src/sonic-frr/dplane_fpm_sonic/dplane_fpm_sonic.c'
    member_header = source.with_name('bridge_port_protection_fpm.h')
    body = source.read_text()
    binding = body[body.index('/* Keep the native EVPN-MH binding layout'):body.index('\nstatic ssize_t\ndplane_fpm_nl_handle_br_port_update')]
    header = swss/'fpmsyncd/fpm/fpm.h'
    program = args.output/'fpm_encoder.c'
    prelude = r'''
#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stddef.h>
#define MULTIPATH_NUM 64
#define MAX(a,b) ((a) > (b) ? (a) : (b))
static void zlog_warn(const char *format, ...) { (void)format; }
struct zebra_dplane_ctx { uint32_t index; uint32_t group; unsigned short vlan; };
static uint32_t dplane_ctx_get_ifindex(const struct zebra_dplane_ctx *c) { return c->index; }
static uint32_t dplane_ctx_get_br_port_backup_nhg_id(const struct zebra_dplane_ctx *c) { return c->group; }
static unsigned short dplane_ctx_get_br_port_vlan_id(const struct zebra_dplane_ctx *c) { return c->vlan; }
'''
    driver = r'''
int main(int argc, char **argv) {
    assert(argc >= 4);
    union { struct nlmsghdr align; unsigned char bytes[16432]; } buffer;
    ssize_t length=-1;
    uint32_t id=(uint32_t)strtoul(argv[2],NULL,0);
    bool deleting=strcmp(argv[3],"del")==0;
    if (strcmp(argv[1],"binding")==0) {
        assert(argc==5);
        struct zebra_dplane_ctx context={id,(uint32_t)strtoul(argv[4],NULL,0),0};
        length=dplane_fpm_nl_send_br_port_backup_nhg(&context,buffer.bytes,sizeof(buffer));
    } else if (strcmp(argv[1],"member")==0) {
        assert(argc==7);
        struct l2_nhg_member_msg member={0}; member.l2nm_nh_id=id;
        member.l2nm_type=(uint8_t)strtoul(argv[4],NULL,0);
        member.l2nm_family=strchr(argv[5],':') ? AF_INET6 : AF_INET;
        assert(inet_pton(member.l2nm_family,argv[5],&member.l2nm_remote_vtep)==1);
        unsigned mac[6]; assert(sscanf(argv[6],"%x:%x:%x:%x:%x:%x",&mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5])==6);
        for (unsigned i=0;i<6;++i) member.l2nm_router_mac[i]=(uint8_t)mac[i];
        length=sonic_fpm_encode_member(&member,deleting,buffer.bytes,sizeof(buffer));
    } else if (strcmp(argv[1],"group")==0) {
        uint32_t ids[L2_NHG_MAX_MEMBERS]; assert((unsigned)(argc-4)<=L2_NHG_MAX_MEMBERS);
        for (int i=4;i<argc;++i) ids[i-4]=(uint32_t)strtoul(argv[i],NULL,0);
        length=sonic_fpm_encode_group(id,ids,(uint32_t)(argc-4),deleting,buffer.bytes,sizeof(buffer));
    }
    assert(length>0 && length+4<=FPM_MAX_MSG_LEN);
    fpm_msg_hdr_t fpm={FPM_PROTO_VERSION,FPM_MSG_TYPE_NETLINK,htons((uint16_t)(length+4))};
    assert(fwrite(&fpm,sizeof(fpm),1,stdout)==1);
    assert(fwrite(buffer.bytes,(size_t)length,1,stdout)==1);
}
'''
    # fpm.h also contains C++ convenience casts; keep its actual ABI section
    # verbatim so the real FRR functions are still compiled as C.
    contract = header.read_text()
    contract = contract[contract.index('#define FPM_MAX_MSG_LEN'):contract.index('/* End of FPM extensions */')]
    program.write_text(prelude+'\n'+contract+'\n'+binding+
                       f'\n#include "{member_header.resolve()}"\n'+driver)
    subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(program),
                    '-o', str(args.output/'fpm_encoder')], check=True)
    manifest['sources'] = {str(path):hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (source,member_header,header,args.sai/'inc/saibridge.h',tools/'sai_fault_shim.cpp',Path(__file__))}
    manifest['scope'] = 'Actual serializer functions with synthetic getters; not live zebra dispatcher/getter integration.'
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__ == '__main__':
    main()
