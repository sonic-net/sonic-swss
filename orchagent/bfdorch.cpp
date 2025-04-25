#include "bfdorch.h"
#include "intfsorch.h"
#include "vrforch.h"
#include "converter.h"
#include "swssnet.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "directory.h"
#include "notifications.h"
#include "schema.h"

using namespace std;
using namespace swss;

#define BFD_SESSION_DEFAULT_TX_INTERVAL 1000
#define BFD_SESSION_DEFAULT_RX_INTERVAL 1000
#define BFD_SESSION_DEFAULT_DETECT_MULTIPLIER 10
// TOS: default 6-bit DSCP value 48, default 2-bit ecn value 0. 48<<2 = 192
#define BFD_SESSION_DEFAULT_TOS 192
#define BFD_SESSION_MILLISECOND_TO_MICROSECOND 1000
#define BFD_SRCPORTINIT 49152
#define BFD_SRCPORTMAX 65536
#define NUM_BFD_SRCPORT_RETRIES 3

extern sai_bfd_api_t*       sai_bfd_api;
extern sai_object_id_t      gSwitchId;
extern sai_object_id_t      gVirtualRouterId;
extern PortsOrch*           gPortsOrch;
extern sai_switch_api_t*    sai_switch_api;
extern Directory<Orch*>     gDirectory;
extern string               gMySwitchType;

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

    m_stateDbConnector = std::make_unique<swss::DBConnector>("STATE_DB", 0);
    m_stateSoftBfdSessionTable = std::make_unique<swss::Table>(m_stateDbConnector.get(), STATE_BFD_SOFTWARE_SESSION_TABLE_NAME);

    SWSS_LOG_NOTICE("Switch type is: %s", gMySwitchType.c_str());

    vector<string> keys;

    // Clean up state database BFD entries
    m_stateBfdSessionTable.getKeys(keys);
    for (auto alias : keys)
    {
        m_stateBfdSessionTable.del(alias);
    }
    // Clean up state database software BFD entries
    m_stateSoftBfdSessionTable->getKeys(keys);
    for (auto alias : keys)
    {
        m_stateSoftBfdSessionTable->del(alias);
    }
    Orch::addExecutor(bfdStateNotificatier);
    register_state_change_notif = false;
}

BfdOrch::~BfdOrch(void)
{
    SWSS_LOG_ENTER();
}

std::string BfdOrch::createStateDBKey(const std::string &input) {
    // Replace ':' with '|' to convert key to StateDB format.
    std::string result = input;
    size_t pos = result.find(':'); // Find the first colon
    if (pos != std::string::npos) {
        result[pos] = '|'; // Replace the first colon with '|'

        // Find the second colon
        pos = result.find(':', pos + 1);
        if (pos != std::string::npos) {
            result[pos] = '|'; // Replace the second colon with '|'
        }
    }
    return result;
}

void BfdOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    BgpGlobalStateOrch* bgp_global_state_orch = gDirectory.get<BgpGlobalStateOrch*>();
    bool tsa_enabled = false;
    bool use_software_bfd = true;
    if (bgp_global_state_orch)
    {
        tsa_enabled = bgp_global_state_orch->getTsaState();
        use_software_bfd = bgp_global_state_orch->getSoftwareBfd();
    }
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key =  kfvKey(t);
        string op = kfvOp(t);
        auto data = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            if (use_software_bfd)
            {
                //program entry in software BFD table
                m_stateSoftBfdSessionTable->set(createStateDBKey(key), data);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            bool tsa_shutdown_enabled = false;
            for (auto i : data)
            {
                auto value = fvValue(i);
                //shutdown_bfd_during_tsa parameter is used by the BFD session creator to ensure that the the
                //specified session gets removed when the device goes into TSA state.
                //if this parameter is not specified or set to false for a session, the
                // corrosponding BFD session would be maintained even in TSA state.
                if (fvField(i) == "shutdown_bfd_during_tsa" && value == "true" )
                {
                    tsa_shutdown_enabled = true;
                    break;
                }
            }
            if (tsa_shutdown_enabled)
            {
                bfd_session_cache[key] = data;
                if (!tsa_enabled)
                {
                    if (!create_bfd_session(key, data))
                    {
                        it++;
                        continue;
                    }
                }
                else
                {
                    notify_session_state_down(key);
                }
            }
            else
            {
                if (!create_bfd_session(key, data))
                {
                    it++;
                    continue;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (use_software_bfd)
            {
                //delete entry from software BFD table
                m_stateSoftBfdSessionTable->del(createStateDBKey(key));
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (bfd_session_cache.find(key) != bfd_session_cache.end() )
            {
                bfd_session_cache.erase(key);
                if (!tsa_enabled)
                {
                    if (!remove_bfd_session(key))
                    {
                        it++;
                        continue;
                    }
                }
            }
            else
            {
                if (!remove_bfd_session(key))
                {
                    it++;
                    continue;
                }
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
    uint8_t tos = BFD_SESSION_DEFAULT_TOS;
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
        else if (fvField(i) == "tos")
        {
            tos = to_uint<uint8_t>(value);
        }
        else if (fvField(i) == "shutdown_bfd_during_tsa")
        {
            //since we are handling shutdown_bfd_during_tsa in the caller function, we need to ignore it here.
            //failure to ignore this parameter would cause error log.
            continue;
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

    uint32_t local_discriminator = bfd_gen_id();
    attr.id = SAI_BFD_SESSION_ATTR_LOCAL_DISCRIMINATOR;
    attr.value.u32 = local_discriminator;
    attrs.emplace_back(attr);
    fvVector.emplace_back("local_discriminator", to_string(local_discriminator));

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

    attr.id = SAI_BFD_SESSION_ATTR_TOS;
    attr.value.u8 = tos;
    attrs.emplace_back(attr);

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
        status = retry_create_bfd_session(bfd_session_id, attrs);
    }

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

void BfdOrch::update_port_number(vector<sai_attribute_t> &attrs)
{
    for (uint32_t attr_idx = 0; attr_idx < (uint32_t)attrs.size(); attr_idx++)
    {
       if (attrs[attr_idx].id ==  SAI_BFD_SESSION_ATTR_UDP_SRC_PORT)
       {
           auto old_num = attrs[attr_idx].value.u32;
           attrs[attr_idx].value.u32 = bfd_src_port();
           SWSS_LOG_WARN("BFD create using port number %d failed. Retrying with port number %d",
                         old_num, attrs[attr_idx].value.u32);
           return;
       }
    }
}

sai_status_t BfdOrch::retry_create_bfd_session(sai_object_id_t &bfd_session_id, vector<sai_attribute_t> attrs)
{
    sai_status_t status = SAI_STATUS_FAILURE;

    for (int retry = 0; retry < NUM_BFD_SRCPORT_RETRIES; retry++)
    {
        update_port_number(attrs);
        status = sai_bfd_api->create_bfd_session(&bfd_session_id, gSwitchId,
                                                 (uint32_t)attrs.size(), attrs.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            return status;
        }
    }
    return status;
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

void BfdOrch::notify_session_state_down(const string& key)
{
    SWSS_LOG_ENTER();
    size_t found_vrf = key.find(delimiter);
    if (found_vrf == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s, no vrf is given", key.c_str());
        return;
    }

    size_t found_ifname = key.find(delimiter, found_vrf + 1);
    if (found_ifname == string::npos)
    {
        SWSS_LOG_ERROR("Failed to parse key %s, no ifname is given", key.c_str());
        return;
    }
    string vrf_name = key.substr(0, found_vrf);
    string alias = key.substr(found_vrf + 1, found_ifname - found_vrf - 1);
    IpAddress peer_address(key.substr(found_ifname + 1));
    BfdUpdate update;
    update.peer = get_state_db_key(vrf_name, alias, peer_address);
    update.state = SAI_BFD_SESSION_STATE_DOWN;
    notify(SUBJECT_TYPE_BFD_SESSION_STATE_CHANGE, static_cast<void *>(&update));
}

void BfdOrch::handleTsaStateChange(bool tsaState)
{
    SWSS_LOG_ENTER();
    for (auto it : bfd_session_cache)
    {
        if (tsaState == true)
        {
            if (bfd_session_map.find(it.first) != bfd_session_map.end())
            {
                notify_session_state_down(it.first);
                remove_bfd_session(it.first);
            }
        }
        else
        {
            if (bfd_session_map.find(it.first) == bfd_session_map.end())
            {
                create_bfd_session(it.first, it.second);
            }
        }
    }
}

BgpGlobalStateOrch::BgpGlobalStateOrch(DBConnector *db, string tableName):
    Orch(db, tableName)
{
    SWSS_LOG_ENTER();
    tsa_enabled = false;
    bool ipv6 = true;
    bfd_offload = (offload_supported(!ipv6) && offload_supported(ipv6));
}

BgpGlobalStateOrch::~BgpGlobalStateOrch(void)
{
    SWSS_LOG_ENTER();
}

bool BgpGlobalStateOrch::getTsaState()
{
    SWSS_LOG_ENTER();
    return tsa_enabled;
}

bool BgpGlobalStateOrch::getSoftwareBfd()
{
    SWSS_LOG_ENTER();
    return !bfd_offload;
}

bool BgpGlobalStateOrch::offload_supported(bool get_ipv6)
{
    sai_attribute_t attr;
    sai_status_t status;
    sai_attr_capability_t capability;

    attr.id = SAI_SWITCH_ATTR_SUPPORTED_IPV4_BFD_SESSION_OFFLOAD_TYPE;
    if(get_ipv6)
    {
        attr.id = SAI_SWITCH_ATTR_SUPPORTED_IPV6_BFD_SESSION_OFFLOAD_TYPE;
    }

    status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH,
                                            attr.id, &capability);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to query BFD offload capability");
        return false;
    }
    if (!capability.get_implemented)
    {
        SWSS_LOG_ERROR("BFD offload type not implemented");
        return false;
    }

    uint32_t list[1] = { 1 };
    attr.value.u32list.count = 1;
    attr.value.u32list.list = list;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if(status == SAI_STATUS_SUCCESS && attr.value.u32list.count > 0)
    {
        SWSS_LOG_INFO("BFD offload type: %d", attr.value.u32list.list[0]);
        return (attr.value.u32list.list[0] != SAI_BFD_SESSION_OFFLOAD_TYPE_NONE);
    }
    SWSS_LOG_ERROR("Could not get supported BFD offload type, rv: %d", status);
    return false;
}

void BgpGlobalStateOrch::doTask(Consumer &consumer)
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
            for (auto i : data)
            {
                auto value = fvValue(i);
                auto type = fvField(i);
                SWSS_LOG_INFO("SET on key %s, data T %s, V %s\n", key.c_str(), type.c_str(), value.c_str());
                if (type == "tsa_enabled")
                {
                    bool state = true ? value == "true" : false;
                    if (tsa_enabled != state)
                    {
                        SWSS_LOG_NOTICE("BgpGlobalStateOrch TSA state Changed to %d from %d.\n", int(state), int(tsa_enabled));
                        tsa_enabled = state;

                        BfdOrch* bfd_orch = gDirectory.get<BfdOrch*>();
                        if (bfd_orch)
                        {
                            bfd_orch->handleTsaStateChange(state);
                        }
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_ERROR("DEL on key %s is not expected.\n", key.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

