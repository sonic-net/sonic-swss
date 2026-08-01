#include "gtest/gtest.h"

#include "orch.h"
#include "request_parser.h"
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
 * Every Request below must skip unknown fields instead of throwing, while still
 * rejecting rows that are genuinely invalid.
 */
namespace config_request_relaxed_test
{
    using namespace swss;

    static const char *UNKNOWN_FIELD = "some_field_orchagent_does_not_know";

    TEST(ConfigFacingRequest, VNetSkipsUnknownField)
    {
        VNetRequest request;
        KeyOpFieldsValuesTuple row("Vnet_1", SET_COMMAND,
            { { "vxlan_tunnel", "tunnel_1" },
              { "vni", "1000" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrUint("vni"), 1000u);
    }

    TEST(ConfigFacingRequest, VxlanTunnelSkipsUnknownField)
    {
        VxlanTunnelRequest request;
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { "src_ip", "10.1.1.1" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrIP("src_ip").to_string(), "10.1.1.1");
    }

    TEST(ConfigFacingRequest, VxlanTunnelMapSkipsUnknownField)
    {
        VxlanTunnelMapRequest request;
        KeyOpFieldsValuesTuple row("tunnel_1:map_1", SET_COMMAND,
            { { "vni", "1000" },
              { "vlan", "Vlan100" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrUint("vni"), 1000u);
    }

    TEST(ConfigFacingRequest, EvpnNvoSkipsUnknownField)
    {
        EvpnNvoRequest request;
        KeyOpFieldsValuesTuple row("nvo_1", SET_COMMAND,
            { { "source_vtep", "tunnel_1" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrString("source_vtep"), "tunnel_1");
    }

    TEST(ConfigFacingRequest, MuxCfgSkipsUnknownField)
    {
        MuxCfgRequest request;
        KeyOpFieldsValuesTuple row("Ethernet0", SET_COMMAND,
            { { "server_ipv4", "10.1.1.1/32" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrIpPrefix("server_ipv4").to_string(), "10.1.1.1/32");
    }

    TEST(ConfigFacingRequest, NvgreTunnelSkipsUnknownField)
    {
        NvgreTunnelRequest request;
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { "src_ip", "10.1.1.1" },
              { UNKNOWN_FIELD, "whatever" } });

        ASSERT_NO_THROW(request.parse(row));
        EXPECT_EQ(request.getAttrIP("src_ip").to_string(), "10.1.1.1");
    }

    TEST(ConfigFacingRequest, NvgreTunnelMapSkipsUnknownField)
    {
        NvgreTunnelMapRequest request;
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
        VxlanTunnelRequest request;
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { UNKNOWN_FIELD, "whatever" } });

        EXPECT_THROW(request.parse(row), std::invalid_argument);
    }

    /* An unparsable value for a field the orch does know about is still an error. */
    TEST(ConfigFacingRequest, RelaxedParsingStillRejectsBadKnownValue)
    {
        NvgreTunnelRequest request;
        KeyOpFieldsValuesTuple row("tunnel_1", SET_COMMAND,
            { { "src_ip", "not_an_ip_address" } });

        EXPECT_THROW(request.parse(row), std::invalid_argument);
    }
}
