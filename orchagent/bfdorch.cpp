/*
 * TODO: Add support for additional parameters
 *       make set and del separate functions
 *       add support for set operation
 */

#include "bfdorch.h"
#include "converter.h"
#include "swssnet.h"

using namespace std;
using namespace swss;

#define BFD_SESSION_DEFAULT_TX_INTERVAL 1000 * 1000
#define BFD_SESSION_DEFAULT_RX_INTERVAL 1000 * 1000
#define BFD_SESSION_DEFAULT_DETECT_MULTIPLIER 3

extern sai_bfd_api_t*       sai_bfd_api;
extern sai_object_id_t      gSwitchId;
extern sai_object_id_t      gVirtualRouterId;
extern PortsOrch*           gPortsOrch;

const map<string, sai_bfd_session_type_t> session_type_map =
{
    {"demand_active",       SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE},
    {"demand_passive",      SAI_BFD_SESSION_TYPE_DEMAND_PASSIVE},
    {"async_active",        SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE},
    {"async_passive",       SAI_BFD_SESSION_TYPE_ASYNC_PASSIVE}
};

BfdOrch::BfdOrch(DBConnector *db, string tableName):
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

BfdOrch::~BfdOrch(void)
{
    SWSS_LOG_ENTER();
}

void BfdOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key =  kfvKey(t);
        string op = kfvOp(t);
        auto data = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            if (!create_bfd_session(key, data))
            {
                it++;
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!remove_bfd_session(key))
            {
                it++;
                continue;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool BfdOrch::create_bfd_session(const string& key, const vector<FieldValueTuple>& data)
{
    if (bfd_session_map.find(key) != bfd_session_map.end())
    {
        SWSS_LOG_ERROR("BFD session for %s already exists", key.c_str());
        return true;
    }

    size_t found = key.find(':');
    if (found == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
        return false;
    }

    string alias = key.substr(0, found);
    IpAddress peer_address(key.substr(found+1));

    sai_bfd_session_type_t bfd_session_type = SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE;
    sai_bfd_encapsulation_type_t encapsulation_type = SAI_BFD_ENCAPSULATION_TYPE_NONE;
    IpAddress src_ip;
    uint32_t tx_interval = BFD_SESSION_DEFAULT_TX_INTERVAL;
    uint32_t rx_interval = BFD_SESSION_DEFAULT_RX_INTERVAL;
    uint8_t multiplier = BFD_SESSION_DEFAULT_DETECT_MULTIPLIER;
    bool multihop = false;
    MacAddress dst_mac;
    bool dst_mac_provided = false;

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    for (auto i : data)
    {
        auto value = fvValue(i);

        if (fvField(i) == "tx_interval")
            tx_interval = to_uint<uint32_t>(value);
        else if (fvField(i) == "rx_interval")
            rx_interval = to_uint<uint32_t>(value);
        else if (fvField(i) == "multiplier")
            multiplier = to_uint<uint8_t>(value);
        else if (fvField(i) == "multihop")
            multihop = (value == "true") ? true : false;
        else if (fvField(i) == "local_addr")
            src_ip = IpAddress(value);
        else if (fvField(i) == "type")
        {
            if (session_type_map.find(value) == session_type_map.end())
            {
                SWSS_LOG_ERROR("Invalid BFD session type %s\n", value.c_str());
                continue;
            }
            bfd_session_type = session_type_map.at(value);
        }
        else if (fvField(i) == "dst_mac")
        {
            dst_mac = MacAddress(value);
            dst_mac_provided = true;
        }
        else
            SWSS_LOG_ERROR("Unsupported BFD attribute %s\n", fvField(i).c_str());
    }

    attr.id = SAI_BFD_SESSION_ATTR_TYPE;
    attr.value.s32 = bfd_session_type;
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_LOCAL_DISCRIMINATOR;
    attr.value.u32 = bfd_gen_id();
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_UDP_SRC_PORT;
    attr.value.u32 = bfd_src_port();
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_REMOTE_DISCRIMINATOR;
    attr.value.u32 = 0;
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_BFD_ENCAPSULATION_TYPE;
    attr.value.s32 = encapsulation_type;
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_IPHDR_VERSION;
    attr.value.u8 = src_ip.isV4() ? 4 : 6;
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS;
    copy(attr.value.ipaddr, src_ip);
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS;
    copy(attr.value.ipaddr, peer_address);
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_MIN_TX;
    attr.value.u32 = tx_interval * 1000;
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_MIN_RX;
    attr.value.u32 = rx_interval * 1000;
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_MULTIPLIER;
    attr.value.u8 = multiplier;
    attrs.emplace_back(attr);

    if (multihop)
    {
        attr.id = SAI_BFD_SESSION_ATTR_MULTIHOP;
        attr.value.booldata = true;
        attrs.emplace_back(attr);
    }

    if (alias != "default")
    {
        attr.id = SAI_BFD_SESSION_ATTR_HW_LOOKUP_VALID;
        attr.value.booldata = false;
        attrs.emplace_back(attr);

        Port port;
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_ERROR("Failed to locate port %s", alias.c_str());
            return false;
        }

        attr.id = SAI_BFD_SESSION_ATTR_PORT;
        attr.value.oid = port.m_port_id;
        attrs.emplace_back(attr);

        attr.id = SAI_BFD_SESSION_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, port.m_mac.getMac(), sizeof(sai_mac_t));
        attrs.emplace_back(attr);

        if (!dst_mac_provided)
        {
            SWSS_LOG_ERROR("Failed to create BFD session %s: destination MAC address required when hardware lookup not valid",
                            key.c_str());
            return false;
        }

        attr.id = SAI_BFD_SESSION_ATTR_DST_MAC_ADDRESS;
        memcpy(attr.value.mac, dst_mac.getMac(), sizeof(sai_mac_t));
        attrs.emplace_back(attr);
    }
    else
    {
        attr.id = SAI_BFD_SESSION_ATTR_VIRTUAL_ROUTER;
        attr.value.oid = gVirtualRouterId;
        attrs.emplace_back(attr);

        if (dst_mac_provided)
        {
            SWSS_LOG_ERROR("Failed to create BFD session %s: destination MAC address not supported when hardware lookup valid",
                            key.c_str());
            return false;
        }
    }

    sai_object_id_t bfd_session_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_bfd_api->create_bfd_session(&bfd_session_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create bfd session %s, rv:%d", key.c_str(), status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_BFD, status);
        if (!parseHandleSaiStatusFailure(handle_status))
        {
            return false;
        }
    }

    bfd_session_map[key] = bfd_session_id;

    return true;
}

bool BfdOrch::remove_bfd_session(const string& key)
{
    if (bfd_session_map.find(key) == bfd_session_map.end())
    {
        SWSS_LOG_ERROR("BFD session for %s does not exist", key.c_str());
        return true;
    }

    sai_object_id_t bfd_session_id = bfd_session_map[key];
    sai_status_t status = sai_bfd_api->remove_bfd_session(bfd_session_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove bfd session %s, rv:%d", key.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_BFD, status);
        if (!parseHandleSaiStatusFailure(handle_status))
        {
            return false;
        }
    }

    bfd_session_map.erase(key);

    return true;
}

uint32_t BfdOrch::bfd_gen_id(void)
{
	static uint32_t session_id = 1;
	return (session_id++);
}

uint32_t BfdOrch::bfd_src_port(void)
{
	static uint32_t port = BFD_SRCPORTINIT;
    if (port >= BFD_SRCPORTMAX)
    {
        port = BFD_SRCPORTINIT;
    }

	return (port++);
}
