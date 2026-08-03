#include "gtest/gtest.h"

#include "orch.h"
#include "request_parser.h"
#include "config_request_relaxed.h"
#include "vnetorch.h"
#include "vxlanorch.h"
#include "muxorch.h"
#include "nvgreorch.h"

/*
 * Rows for these tables originate in CONFIG_DB - either read from it directly
 * or copied verbatim into APPL_DB by a *mgrd. CONFIG_DB is the YANG/user-facing
 * database, so it gains fields that orchagent has no schema for.
 *
 * Request::parse() throws on an unrecognized field, and Orch2::doTask() reacts
 * by erasing the entry, which discards the whole row rather than the offending
 * field. One stray field therefore leaves the object completely unprogrammed
 * with nothing but a parse error in the log (UPSW-6663).
 *
 * Coverage lives here (own translation unit) so upstream mock_tests downmerges
 * stay free of this local hardening. The policy itself is
 * orchagent/config_request_relaxed.h.
 */
namespace config_request_relaxed_test
{
    using namespace swss;

    static const char *UNKNOWN_FIELD = "some_field_orchagent_does_not_know";

    TEST(ConfigFacingRequest, VNetSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(vnet_request_description, ':');
        KeyOpFieldsValuesTuple row("Vnet_1", SET_COMMAND,
            { { "vxlan_tunnel", "tunnel_1" },
              { "vni", "1000" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrUint("vni"), 1000u);
    }

    TEST(ConfigFacingRequest, VxlanTunnelSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(vxlan_tunnel_request_description, ':');
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { "src_ip", "10.1.1.1" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrIP("src_ip").to_string(), "10.1.1.1");
    }

    TEST(ConfigFacingRequest, VxlanTunnelMapSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(vxlan_tunnel_map_request_description, ':');
        KeyOpFieldsValuesTuple row("tunnel_1:map_1", SET_COMMAND,
            { { "vni", "1000" },
              { "vlan", "Vlan100" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrUint("vni"), 1000u);
    }

    TEST(ConfigFacingRequest, EvpnNvoSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(evpn_nvo_request_description, ':');
        KeyOpFieldsValuesTuple row("nvo_1", SET_COMMAND,
            { { "source_vtep", "tunnel_1" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrString("source_vtep"), "tunnel_1");
    }

    TEST(ConfigFacingRequest, MuxCfgSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(mux_cfg_request_description, '|');
        KeyOpFieldsValuesTuple row("Ethernet0", SET_COMMAND,
            { { "server_ipv4", "10.1.1.1/32" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrIpPrefix("server_ipv4").to_string(), "10.1.1.1/32");
    }

    TEST(ConfigFacingRequest, NvgreTunnelSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(nvgre_tunnel_request_description, '|');
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { "src_ip", "10.1.1.1" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrIP("src_ip").to_string(), "10.1.1.1");
    }

    TEST(ConfigFacingRequest, NvgreTunnelMapSkipsUnknownField)
    {
        ConfigFacingRequestRelaxed request(nvgre_tunnel_map_request_description, '|');
        KeyOpFieldsValuesTuple row("tunnel_1|map_1", SET_COMMAND,
            { { "vsid", "1000" },
              { "vlan_id", "Vlan100" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrUint("vsid"), 1000u);
    }

    /*
     * Relaxed parsing must not become "accept anything". Mandatory attributes
     * are validated after the attribute loop, independently of the relaxed
     * flag, so a row that is actually malformed still has to be rejected.
     */
    TEST(ConfigFacingRequest, RelaxedParsingStillEnforcesMandatoryAttrs)
    {
        ConfigFacingRequestRelaxed request(vxlan_tunnel_request_description, ':');
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { UNKNOWN_FIELD, "whatever" } });

        EXPECT_THROW(request.parse(row), std::invalid_argument);
    }

    /* An unparsable value for a field the orch does know about is still an error. */
    TEST(ConfigFacingRequest, RelaxedParsingStillRejectsBadKnownValue)
    {
        ConfigFacingRequestRelaxed request(nvgre_tunnel_request_description, '|');
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { "src_ip", "not_an_ip_address" } });

        EXPECT_THROW(request.parse(row), std::invalid_argument);
    }

    /*
     * Request::clear() previously left attr_item_ip_prefix_ (and several other
     * maps) uncleared. Combined with MuxCfgRequest having no mandatory attrs,
     * a partial SET after a prior parse could make getAttrIpPrefix return a
     * stale address. Guard that clear() drops IP-prefix state.
     */
    TEST(ConfigFacingRequest, ClearDropsStaleIpPrefixAttrs)
    {
        ConfigFacingRequestRelaxed request(mux_cfg_request_description, '|');
        KeyOpFieldsValuesTuple first("Ethernet0", SET_COMMAND,
            { { "server_ipv4", "10.1.1.1/32" } });
        ASSERT_NO_THROW(request.parse(first));
        EXPECT_EQ(request.getAttrIpPrefix("server_ipv4").to_string(), "10.1.1.1/32");

        request.clear();

        KeyOpFieldsValuesTuple second("Ethernet0", SET_COMMAND,
            { { UNKNOWN_FIELD, "whatever" } });
        ASSERT_NO_THROW(request.parse(second));
        EXPECT_THROW(request.getAttrIpPrefix("server_ipv4"), std::out_of_range);
    }
}
