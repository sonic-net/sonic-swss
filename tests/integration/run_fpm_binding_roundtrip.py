#!/usr/bin/env python3
"""Compile the actual buildimage binding encoder with synthetic ctx getters.

No FRR daemon, privileges or network namespace is needed. The function and
contract are extracted unchanged from buildimage, rather than reimplemented.
FPM framing uses the header from buildimage's pinned upstream FRR revision.
This does not build the entire FRR module or exercise live zebra replay.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--buildimage', required=True, type=Path)
    parser.add_argument('--frr-fpm-header', required=True, type=Path)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--report-dir', required=True, type=Path)
    args = parser.parse_args()
    source = args.buildimage / 'src/sonic-frr/dplane_fpm_sonic/dplane_fpm_sonic.c'
    patch = args.buildimage / 'src/sonic-frr/patch/0077-zebra-Add-FPM-EVPN-MH-message-types-and-structures.patch'
    text = source.read_text()
    start = text.index('/* Keep the native EVPN-MH binding layout')
    end = text.index('\nstatic ssize_t\ndplane_fpm_nl_handle_br_port_update', start)
    encoder = text[start:end]
    additions = '\n'.join(line[1:] for line in patch.read_text().splitlines()
                          if line.startswith('+') and not line.startswith('+++'))
    enum = re.search(r'typedef enum \{.*?\} rtm_fpm_msg_types_et;', additions, re.S).group()
    wire = re.search(r'struct evpn_backup_nhg_msg \{.*?\};', additions, re.S).group()
    # Fail closed if the supplied producer framing header is not the pinned one.
    framing = args.frr_fpm_header.read_bytes()
    framing_sha = hashlib.sha256(framing).hexdigest()
    expected_sha = '2fa0efff07287abf7ddaa17e8fa1592a7d5ccac3096ca9c2732db768abad0834'
    if framing_sha != expected_sha:
        raise ValueError('FRR FPM header does not match pinned revision f8c0b08d')
    args.report_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fpm-binding-') as temporary:
        directory = Path(temporary)
        header = directory / 'fpm.h'
        header.write_bytes(framing)
        program = directory / 'encoder.c'
        prelude = r'''
#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdarg.h>
static void zlog_warn(const char *format, ...) { (void)format; }
#define MULTIPATH_NUM 64
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#include "fpm.h"
struct zebra_dplane_ctx { uint32_t index; uint32_t group; unsigned short vlan; };
static uint32_t dplane_ctx_get_ifindex(const struct zebra_dplane_ctx *c) { return c->index; }
static uint32_t dplane_ctx_get_br_port_backup_nhg_id(const struct zebra_dplane_ctx *c) { return c->group; }
static unsigned short dplane_ctx_get_br_port_vlan_id(const struct zebra_dplane_ctx *c) { return c->vlan; }
'''
        checks = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    union { struct nlmsghdr align; unsigned char bytes[64]; } buffer;
    struct zebra_dplane_ctx ctx = {40, 5000, 0};
    assert(dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, 23) == -1);
    ctx.vlan = 100;
    assert(dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, sizeof(buffer)) == 0);
    ctx.vlan = 0;
    ctx.index = 0;
    assert(dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, sizeof(buffer)) == -1);
    ctx.index = UINT32_MAX;
    assert(dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, sizeof(buffer)) == -1);
    ctx.index = 40; ctx.group = UINT32_MAX;
    assert(dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, sizeof(buffer)) == 24);
    ctx.index = INT_MAX; ctx.group = 1;
    assert(dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, sizeof(buffer)) == 24);
    ctx.index = 40;
    FILE *output = fopen(argv[1], "wb"); assert(output);
    uint32_t groups[] = {5000, INT_MAX, 0x80000001u, UINT32_MAX, 0, 0};
    for (unsigned i = 0; i < sizeof(groups)/sizeof(groups[0]); ++i) {
        ctx.group = groups[i];
        ssize_t size = dplane_fpm_nl_send_br_port_backup_nhg(&ctx, buffer.bytes, 24);
        assert(size == 24);
        struct nlmsghdr *n = (void *)buffer.bytes;
        struct evpn_backup_nhg_msg *payload = NLMSG_DATA(n);
        assert(n->nlmsg_type == (ctx.group > 0 ? RTM_FPM_ADD_EVPN_ES_BACKUP_NHG : RTM_FPM_DEL_EVPN_ES_BACKUP_NHG));
        assert(n->nlmsg_flags == (NLM_F_CREATE | NLM_F_REQUEST));
        assert(payload->ebnm_ifindex == (int)ctx.index && payload->ebnm_backup_nhg_id == ctx.group);
        fpm_msg_hdr_t hdr = {FPM_PROTO_VERSION, FPM_MSG_TYPE_NETLINK, htons(sizeof(hdr) + size)};
        assert(fwrite(&hdr, sizeof(hdr), 1, output) == 1);
        assert(fwrite(buffer.bytes, size, 1, output) == 1);
    }
    assert(fclose(output) == 0);
    return 0;
}
'''
        program.write_text(prelude + enum + '\n' + wire + '\n' + encoder + checks)
        executable = directory / 'encoder'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '--coverage',
                        '-O0', '-g', str(program), '-o', str(executable)], check=True)
        fixtures = args.report_dir / 'producer-frames.bin'
        subprocess.run([str(executable), str(fixtures)], check=True)
        subprocess.run(['gcov', '-b', '-c', str(program)], cwd=directory, check=True)
        gcov = directory / 'encoder.c.gcov'
        # Retain actual encoder coverage, distinct from harness assertions/getters.
        args.report_dir.joinpath('producer-encoder.gcov').write_bytes(gcov.read_bytes())
        args.report_dir.joinpath('producer-encoder.c').write_text(program.read_text())
        environment = dict(os.environ, SONIC_FPM_BINDING_FIXTURES=str(fixtures))
        subprocess.run([str(args.binary.resolve()), '--gtest_filter=FpmBindingTest.*:FpmsyncdEvpnMhTest.*:FpmLinkTest.*',
                        '--gtest_output=xml:' + str(args.report_dir / 'fpm-tests.xml')],
                       env=environment, cwd=args.binary.resolve().parent, check=True)
        args.report_dir.joinpath('producer-provenance.json').write_text(json.dumps({
            'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'encoder_sha256': hashlib.sha256(encoder.encode()).hexdigest(),
            'contract_patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
            'framing_revision': 'f8c0b08dcb0c78f9e42b9b86ae70b049e4e617c1',
            'framing_sha256': framing_sha,
            'scope': 'Actual binding encoder with synthetic context getters; FPM header wrapper; actual FpmLink/RouteSync decoder. Full FRR module/live zebra not run.'
        }, indent=2) + '\n')


if __name__ == '__main__':
    main()
