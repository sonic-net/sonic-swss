#pragma once

/*
 * Test-only: libsaivs handles SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE by
 * running real `tc qdisc ... ingress` on a tap, which fails in UT (no taps).
 * PortSampleSaiGuard RAII-swaps sai_port_api to emulate the INGRESS sample
 * attrs in memory and delegates everything else to the real libsaivs.
 */
namespace mirror_sample_port_wrap_ut
{
    void install();
    void uninstall();

    struct PortSampleSaiGuard
    {
        PortSampleSaiGuard() { install(); }
        ~PortSampleSaiGuard() { uninstall(); }

        PortSampleSaiGuard(const PortSampleSaiGuard&) = delete;
        PortSampleSaiGuard& operator=(const PortSampleSaiGuard&) = delete;
    };
}
