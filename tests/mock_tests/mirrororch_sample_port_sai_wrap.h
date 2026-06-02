#pragma once

/**
 * Test-only emulation of the SAI port-attribute calls used by the sampled
 * mirror PHY-direct path (orchagent/mirrororch.cpp,
 * MirrorOrch::setUnsetSampledMirrorOnPhyPort).
 *
 * Why this exists:
 *   The mock_tests run against the real virtual-switch SAI (libsaivs). Its
 *   handler for SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE (sonic-sairedis
 *   vslib VirtualSwitchSaiInterface::preSetPort) performs real work -- it
 *   resolves a tap netdev for the port and runs `tc qdisc ... ingress`.
 *   The unit-test environment has no tap interfaces, so libsaivs returns
 *   SAI_STATUS_FAILURE for that attribute, which makes the production
 *   sampled-mirror path return false and the tests fail. EGRESS sampling has
 *   no such special handling, which is why the sflow egress test passes.
 *
 * This guard temporarily swaps the global sai_port_api pointer for a copy
 * whose set/get_port_attribute emulate the two INGRESS sample attributes in
 * memory (returning SUCCESS) and delegate every other attribute to the real
 * libsaivs implementation. Install via RAII around a test body; the real
 * pointer is restored on scope exit.
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
