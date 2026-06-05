#pragma once

/*
 * Test-only: libsaivs handles SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE by
 * running real `tc qdisc ... ingress` on a tap, which fails in UT (no taps).
 * PortSampleSaiGuard RAII-swaps sai_port_api to emulate the INGRESS sample
 * attrs in memory and delegates everything else to the real libsaivs.
 */
namespace mirror_sample_port_wrap_ut
{
    // Test-only fault injection: when set, the wrapped set_port_attribute
    // returns SAI_STATUS_FAILURE for the matching attribute so the error/rollback
    // paths in mirrororch can be exercised. Reset by install()/uninstall().
    extern bool g_fail_mirror_session_set;
    extern bool g_fail_samplepacket_enable_set;
    // Fault injection for the sai_samplepacket_api: fail create/remove so the
    // samplepacket create/remove error paths in mirrororch can be exercised.
    extern bool g_fail_samplepacket_create;
    extern bool g_fail_samplepacket_remove;

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
