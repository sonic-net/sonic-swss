#ifndef SWSS_NEXTHOPKEY_H
#define SWSS_NEXTHOPKEY_H

#include "ipaddress.h"
#include "tokenize.h"
#include "label.h"
#include "intfsorch.h"

#define LABELSTACK_DELIMITER '+'
#define NH_DELIMITER '@'
#define NHG_DELIMITER ','
#define VRF_PREFIX "Vrf"
extern IntfsOrch *gIntfsOrch;

struct NextHopKey
{
    IpAddress           ip_address;     // neighbor IP address
    string              alias;          // incoming interface alias
    uint32_t            vni;            // Encap VNI overlay nexthop
    MacAddress          mac_address;    // Overlay Nexthop MAC.
    LabelStack          label_stack;    // MPLS label stack
    uint32_t            weight;         // NH weight for NHGs
    string              srv6_segment;   // SRV6 segment string
    string              srv6_source;    // SRV6 source address

    NextHopKey() : weight(0) {}
    NextHopKey(const std::string &str, const std::string &alias) :
        alias(alias), vni(0), mac_address(), weight(0)
    {
        std::string ip_str = parseMplsNextHop(str);
        ip_address = ip_str;
    }
    NextHopKey(const IpAddress &ip, const std::string &alias) :
        ip_address(ip), alias(alias), vni(0), mac_address(), weight(0) {}
    NextHopKey(const std::string &str) :
        vni(0), mac_address()
    {
        if (str.find(NHG_DELIMITER) != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        std::string ip_str = parseMplsNextHop(str);
        auto keys = tokenize(ip_str, NH_DELIMITER);
        if (keys.size() == 1)
        {
            ip_address = keys[0];
            alias = gIntfsOrch->getRouterIntfsAlias(ip_address);
        }
        else if (keys.size() == 2)
        {
            ip_address = keys[0];
            alias = keys[1];
            if (!alias.compare(0, strlen(VRF_PREFIX), VRF_PREFIX))
            {
                alias = gIntfsOrch->getRouterIntfsAlias(ip_address, alias);
            }
        }
        else
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        weight = 0;
    }
    NextHopKey(const std::string &str, bool overlay_nh, bool srv6_nh = false)
    {
        if (str.find(NHG_DELIMITER) != string::npos)
        {
            std::string err = "Error converting " + str + " to NextHop";
            throw std::invalid_argument(err);
        }
        if (srv6_nh == true)
        {
            weight = 0;
            vni = 0;
            weight = 0;
            auto keys = tokenize(str, NH_DELIMITER);
            if (keys.size() != 3)
            {
                std::string err = "Error converting " + str + " to Nexthop";
                throw std::invalid_argument(err);
            }
            ip_address = keys[0];
            srv6_segment = keys[1];
            srv6_source = keys[2];
        }
        else
        {
            std::string ip_str = parseMplsNextHop(str);
            auto keys = tokenize(ip_str, NH_DELIMITER);
            if (keys.size() != 4)
            {
                std::string err = "Error converting " + str + " to NextHop";
                throw std::invalid_argument(err);
            }
            ip_address = keys[0];
            alias = keys[1];
            vni = static_cast<uint32_t>(std::stoul(keys[2]));
            mac_address = keys[3];
            weight = 0;
        }
    }

    NextHopKey(const IpAddress &ip, const MacAddress &mac, const uint32_t &vni, bool overlay_nh) : ip_address(ip), alias(""), vni(vni), mac_address(mac), weight(0){}

    const std::string to_string() const
    {
        std::string str = formatMplsNextHop();
        str += ip_address.to_string() + NH_DELIMITER + alias;
        return str;
    }

    const std::string to_string(bool overlay_nh, bool srv6_nh) const
    {
        if (srv6_nh)
        {
            return ip_address.to_string() + NH_DELIMITER + srv6_segment + NH_DELIMITER + srv6_source;
        }
        std::string str = formatMplsNextHop();
        str += (ip_address.to_string() + NH_DELIMITER + alias + NH_DELIMITER +
                std::to_string(vni) + NH_DELIMITER + mac_address.to_string());
        return str;
    }

    bool operator<(const NextHopKey &o) const
    {
        return tie(ip_address, alias, label_stack, vni, mac_address, srv6_segment, srv6_source) <
            tie(o.ip_address, o.alias, o.label_stack, o.vni, o.mac_address, o.srv6_segment, o.srv6_source);
    }

    bool operator==(const NextHopKey &o) const
    {
        return (ip_address == o.ip_address) && (alias == o.alias) &&
            (label_stack == o.label_stack) &&
            (vni == o.vni) && (mac_address == o.mac_address) &&
            (srv6_segment == o.srv6_segment) && (srv6_source == o.srv6_source);
    }

    bool operator!=(const NextHopKey &o) const
    {
        return !(*this == o);
    }

    bool isIntfNextHop() const
    {
        return (ip_address.isZero() && !isSrv6NextHop());
    }

    bool isMplsNextHop() const
    {
        return (!label_stack.empty());
    }

    bool isSrv6NextHop() const
    {
        return (srv6_segment != "");
    }

    std::string parseMplsNextHop(const std::string& str)
    {
        // parseMplsNextHop initializes MPLS-related member data of the NextHopKey
        //   based on content of the input param str.
        //   label_stack may be updated.
        std::string ip_str;
        auto keys = tokenize(str, LABELSTACK_DELIMITER);
        if (keys.size() == 1)
        {
            // No MPLS info to parse
            ip_str = str;
        }
        else if (keys.size() == 2)
        {
            // Expected MPLS format = "<outsegtype><labelstack>+<non-mpls-str>"
            // key[0] = <outsegtype> = "swap" | "push"
            // key[0] = <labelstack> = "<label0>/<label1>/../<labelN>"
            // key[1] = <non-mpls-str> = returned to caller and not parsed here
            // Example = "push10100/10101+10.0.0.3@Ethernet4"
            label_stack = LabelStack(keys[0]);
            ip_str = keys[1];
        }
        else
        {
            // Malformed string
            std::string err = "Error converting " + str + " to MPLS NextHop";
            throw std::invalid_argument(err);
        }
        return ip_str;
    }

    std::string formatMplsNextHop() const
    {
        std::string str;
        if (isMplsNextHop())
        {
            str = label_stack.to_string() + LABELSTACK_DELIMITER;
        }
        return str;
    }
};

#endif /* SWSS_NEXTHOPKEY_H */
