#include "bfdorch.h"
#include "intfsorch.h"
#include "vrforch.h"
#include "converter.h"
#include "swssnet.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "directory.h"
#include "notifications.h"

using namespace std;
using namespace swss;

#define BFD_SESSION_DEFAULT_TX_INTERVAL 1000
#define BFD_SESSION_DEFAULT_RX_INTERVAL 1000
#define BFD_SESSION_DEFAULT_DETECT_MULTIPLIER 3
#define BFD_SESSION_MILLISECOND_TO_MICROSECOND 1000
#define BFD_SRCPORTINIT 49152
#define BFD_SRCPORTMAX 65536

extern sai_bfd_api_t*       sai_bfd_api;
extern sai_object_id_t      gSwitchId;
extern sai_object_id_t      gVirtualRouterId;
extern PortsOrch*           gPortsOrch;
extern sai_switch_api_t*    sai_switch_api;
extern Directory<Orch*>     gDirectory;

const map<string, sai_bfd_session_type_t> session_type_map =
{
    {"demand_active",       SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE},
    {"demand_passive",      SAI_BFD_SESSION_TYPE_DEMAND_PASSIVE},
    {"async_active",        SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE},
    {"async_passive",       SAI_BFD_SESSION_TYPE_ASYNC_PASSIVE}
};

const map<sai_bfd_session_type_t, string> session_type_lookup =
{
    {SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE,    "demand_active"},
    {SAI_BFD_SESSION_TYPE_DEMAND_PASSIVE,   "demand_passive"},
    {SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE,     "async_active"},
    {SAI_BFD_SESSION_TYPE_ASYNC_PASSIVE,    "async_passive"}
};

const map<sai_bfd_session_state_t, string> session_state_lookup =
{
    {SAI_BFD_SESSION_STATE_ADMIN_DOWN,  "Admin_Down"},
    {SAI_BFD_SESSION_STATE_DOWN,        "Down"},
    {SAI_BFD_SESSION_STATE_INIT,        "Init"},
    {SAI_BFD_SESSION_STATE_UP,          "Up"}
};

BfdOrch::BfdOrch(DBConnector *db, string tableName, TableConnector stateDbBfdSessionTable):
    Orch(db, tableName),
    m_stateBfdSessionTable(stateDbBfdSessionTable.first, stateDbBfdSessionTable.second)
{
    SWSS_LOG_ENTER();

    DBConnector *notificationsDb = new DBConnector("ASIC_DB", 0);
    m_bfdStateNotificationConsumer = new swss::NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto bfdStateNotificatier = new Notifier(m_bfdStateNotificationConsumer, this, "BFD_STATE_NOTIFICATIONS");
    Orch::addExecutor(bfdStateNotificatier);
    register_state_change_notif = false;
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

void BfdOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer != m_bfdStateNotificationConsumer)
    {
        return;
    }

    if (op == "bfd_session_state_change")
    {
        uint32_t count;
        sai_bfd_session_state_notification_t *bfdSessionState = nullptr;

        sai_deserialize_bfd_session_state_ntf(data, count, &bfdSessionState);

        for (uint32_t i = 0; i < count; i++)
        {
            sai_object_id_t id = bfdSessionState[i].bfd_session_id;
            sai_bfd_session_state_t state = bfdSessionState[i].session_state;

            SWSS_LOG_INFO("Get BFD session state change notification id:%" PRIx64 " state: %s", id, session_state_lookup.at(state).c_str());

            if (state != bfd_session_lookup[id].state)
            {
                auto key = bfd_session_lookup[id].peer;
                m_stateBfdSessionTable.hset(key, "state", session_state_lookup.at(state));

                SWSS_LOG_NOTICE("BFD session state for %s changed from %s to %s", key.c_str(),
                            session_state_lookup.at(bfd_session_lookup[id].state).c_str(), session_state_lookup.at(state).c_str());

                BfdUpdate update;
                update.peer = key;
                update.state = state;
                notify(SUBJECT_TYPE_BFD_SESSION_STATE_CHANGE, static_cast<void *>(&update));

                bfd_session_lookup[id].state = state;
            }
        }

        sai_deserialize_free_bfd_session_state_ntf(count, bfdSessionState);
    }
}

bool BfdOrch::register_bfd_state_change_notification(void)
{
    sai_attribute_t  attr;
    sai_status_t status;
    sai_attr_capability_t capability;

    status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH, 
                                            SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY,
                                            &capability);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to query the BFD change notification capability");
        return false;
    }

    if (!capability.set_implemented)
    {
        SWSS_LOG_ERROR("BFD register change notification not supported");
        return false;
    }

    attr.id = SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY;
    attr.value.ptr = (void *)on_bfd_session_state_change;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to register BFD notification handler");
        return false;
    }
    return true;
}

bool BfdOrch::create_bfd_session(const string& key, const vector<FieldValueTuple>& data)
{
    if (!register_state_change_notif)
    {
        if (!register_bfd_state_change_notification())
        {
            SWSS_LOG_ERROR("BFD session for %s cannot be created", key.c_str());
            return false;
        }
        register_state_change_notif = true;
    }
    if (bfd_session_map.find(key) != bfd_session_map.end())
    {
        SWSS_LOG_ERROR("BFD session for %s already exists", key.c_str());
        return true;
    }

    size_t found_vrf = key.find(delimiter);
    if (found_vrf == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s, no vrf is given", key.c_str());
        return true;
    }

    size_t found_ifname = key.find(delimiter, found_vrf + 1);
    if (found_ifname == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s, no ifname is given", key.c_str());
        return true;
    }

    string vrf_name = key.substr(0, found_vrf);
    string alias = key.substr(found_vrf + 1, found_ifname - found_vrf - 1);
    IpAddress peer_address(key.substr(found_ifname + 1));

    sai_bfd_session_type_t bfd_session_type = SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE;
    sai_bfd_encapsulation_type_t encapsulation_type = SAI_BFD_ENCAPSULATION_TYPE_NONE;
    IpAddress src_ip;
    uint32_t tx_interval = BFD_SESSION_DEFAULT_TX_INTERVAL;
    uint32_t rx_interval = BFD_SESSION_DEFAULT_RX_INTERVAL;
    uint8_t multiplier = BFD_SESSION_DEFAULT_DETECT_MULTIPLIER;
    bool multihop = false;
    MacAddress dst_mac;
    bool dst_mac_provided = false;
    bool src_ip_provided = false;

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    vector<FieldValueTuple> fvVector;

    for (auto i : data)
    {
        auto value = fvValue(i);

        if (fvField(i) == "tx_interval")
        {
            tx_interval = to_uint<uint32_t>(value);
        }
        else if (fvField(i) == "rx_interval")
        {
            rx_interval = to_uint<uint32_t>(value);
        }
        else if (fvField(i) == "multiplier")
        {
            multiplier = to_uint<uint8_t>(value);
        }
        else if (fvField(i) == "multihop")
        {
            multihop = (value == "true") ? true : false;
        }
        else if (fvField(i) == "local_addr")
        {
            src_ip = IpAddress(value);
            src_ip_provided = true;
        }
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

    if (!src_ip_provided)
    {
        SWSS_LOG_ERROR("Failed to create BFD session %s because source IP is not provided", key.c_str());
        return true;
    }

    attr.id = SAI_BFD_SESSION_ATTR_TYPE;
    attr.value.s32 = bfd_session_type;
    attrs.emplace_back(attr);
    fvVector.emplace_back("type", session_type_lookup.at(bfd_session_type));

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
    fvVector.emplace_back("local_addr", src_ip.to_string());

    attr.id = SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS;
    copy(attr.value.ipaddr, peer_address);
    attrs.emplace_back(attr);

    attr.id = SAI_BFD_SESSION_ATTR_MIN_TX;
    attr.value.u32 = tx_interval * BFD_SESSION_MILLISECOND_TO_MICROSECOND;
    attrs.emplace_back(attr);
    fvVector.emplace_back("tx_interval", to_string(tx_interval));

    attr.id = SAI_BFD_SESSION_ATTR_MIN_RX;
    attr.value.u32 = rx_interval * BFD_SESSION_MILLISECOND_TO_MICROSECOND;
    attrs.emplace_back(attr);
    fvVector.emplace_back("rx_interval", to_string(rx_interval));

    attr.id = SAI_BFD_SESSION_ATTR_MULTIPLIER;
    attr.value.u8 = multiplier;
    attrs.emplace_back(attr);
    fvVector.emplace_back("multiplier", to_string(multiplier));

    if (multihop)
    {
        attr.id = SAI_BFD_SESSION_ATTR_MULTIHOP;
        attr.value.booldata = true;
        attrs.emplace_back(attr);
        fvVector.emplace_back("multihop", "true");
    }
    else
    {
        fvVector.emplace_back("multihop", "false");
    }

    if (alias != "default")
    {
        Port port;
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_ERROR("Failed to locate port %s", alias.c_str());
            return false;
        }

        if (!dst_mac_provided)
        {
            SWSS_LOG_ERROR("Failed to create BFD session %s: destination MAC address required when hardware lookup not valid",
                            key.c_str());
            return true;
        }

        if (vrf_name != "default")
        {
            SWSS_LOG_ERROR("Failed to create BFD session %s: vrf is not supported when hardware lookup not valid",
                            key.c_str());
            return true;
        }

        attr.id = SAI_BFD_SESSION_ATTR_HW_LOOKUP_VALID;
        attr.value.booldata = false;
        attrs.emplace_back(attr);

        attr.id = SAI_BFD_SESSION_ATTR_PORT;
        attr.value.oid = port.m_port_id;
        attrs.emplace_back(attr);

        attr.id = SAI_BFD_SESSION_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, port.m_mac.getMac(), sizeof(sai_mac_t));
        attrs.emplace_back(attr);

        attr.id = SAI_BFD_SESSION_ATTR_DST_MAC_ADDRESS;
        memcpy(attr.value.mac, dst_mac.getMac(), sizeof(sai_mac_t));
        attrs.emplace_back(attr);
    }
    else
    {
        if (dst_mac_provided)
        {
            SWSS_LOG_ERROR("Failed to create BFD session %s: destination MAC address not supported when hardware lookup valid",
                            key.c_str());
            return true;
        }

        attr.id = SAI_BFD_SESSION_ATTR_VIRTUAL_ROUTER;
        if (vrf_name == "default")
        {
            attr.value.oid = gVirtualRouterId;
        }
        else
        {
            VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();
            attr.value.oid = vrf_orch->getVRFid(vrf_name);
        }

        attrs.emplace_back(attr);
    }

    fvVector.emplace_back("state", session_state_lookup.at(SAI_BFD_SESSION_STATE_DOWN));

    sai_object_id_t bfd_session_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_bfd_api->create_bfd_session(&bfd_session_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create bfd session %s, rv:%d", key.c_str(), status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_BFD, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    const string state_db_key = get_state_db_key(vrf_name, alias, peer_address);
    m_stateBfdSessionTable.set(state_db_key, fvVector);
    bfd_session_map[key] = bfd_session_id;
    bfd_session_lookup[bfd_session_id] = {state_db_key, SAI_BFD_SESSION_STATE_DOWN};

    BfdUpdate update;
    update.peer = state_db_key;
    update.state = SAI_BFD_SESSION_STATE_DOWN;
    notify(SUBJECT_TYPE_BFD_SESSION_STATE_CHANGE, static_cast<void *>(&update));

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
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    m_stateBfdSessionTable.del(bfd_session_lookup[bfd_session_id].peer);
    bfd_session_map.erase(key);
    bfd_session_lookup.erase(bfd_session_id);

    return true;
}

string BfdOrch::get_state_db_key(const string& vrf_name, const string& alias, const IpAddress& peer_address)
{
    return vrf_name + state_db_key_delimiter + alias + state_db_key_delimiter + peer_address.to_string();
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
