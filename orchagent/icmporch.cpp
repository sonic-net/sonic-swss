/*
 * icmporch.cpp
 *
 *  Created on: Feb 21, 2025
 *      Author: Manas Kumar Mandal
 */

#include "converter.h"
#include "swssnet.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "directory.h"
#include "notifications.h"
#include "icmporch.h"

using namespace std;
using namespace swss;

const std::map<sai_icmp_echo_session_state_t, std::string> IcmpOrch::m_session_state_lkup =
{
    {SAI_ICMP_ECHO_SESSION_STATE_DOWN, "Down"},
    {SAI_ICMP_ECHO_SESSION_STATE_UP,   "Up"}
};

const std::map<std::string, sai_icmp_echo_session_state_t> IcmpOrch::m_session_state_str_lkup =
{
    {"Down", SAI_ICMP_ECHO_SESSION_STATE_DOWN},
    {"Up", SAI_ICMP_ECHO_SESSION_STATE_UP}
};

IcmpOrch::IcmpOrch(DBConnector *db, string tableName, TableConnector stateDbIcmpSessionTable):
    Orch(db, tableName),
    m_stateIcmpSessionTable(stateDbIcmpSessionTable.first, stateDbIcmpSessionTable.second),
    m_register_state_change_notif{false}
{
    SWSS_LOG_ENTER();

    DBConnector *notificationsDb = new DBConnector("ASIC_DB", 0);
    m_icmpStateNotificationConsumer = new swss::NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto icmpStateNotifier = new Notifier(m_icmpStateNotificationConsumer, this, "ICMP_STATE_NOTIFICATIONS");

    // Clean up state database ICMP entries
    vector<string> keys;

    m_stateIcmpSessionTable.getKeys(keys);

    for (auto alias : keys)
    {
        m_stateIcmpSessionTable.del(alias);
    }

    Orch::addExecutor(icmpStateNotifier);
}

IcmpOrch::~IcmpOrch(void)
{
    // do nothing, just log
    SWSS_LOG_ENTER();
}

void IcmpOrch::doTask(Consumer &consumer)
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
            if (m_icmp_session_map.find(key) != m_icmp_session_map.end())
            {
                if (!update_icmp_session(key, data))
                {
                    it++;
                    continue;
                }
            } else {
                if (!create_icmp_session(key, data))
                {
                    it++;
                    continue;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!remove_icmp_session(key))
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

void IcmpOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer != m_icmpStateNotificationConsumer)
    {
        return;
    }

    if (op == "icmp_echo_session_state_change")
    {
        uint32_t count = 0;
        sai_icmp_echo_session_state_notification_t *icmpSessionState = nullptr;

        sai_deserialize_icmp_echo_session_state_ntf(data, count, &icmpSessionState);

        for (uint32_t i = 0; i < count; i++)
        {
            sai_object_id_t id = icmpSessionState[i].icmp_echo_session_id;
            sai_icmp_echo_session_state_t state = icmpSessionState[i].session_state;

            SWSS_LOG_INFO("Got ICMP session state change notification id:%" PRIx64 " state: %s", id, m_session_state_lkup.at(state).c_str());

            if (m_icmp_session_lookup.find(id) == m_icmp_session_lookup.end())
            {
                SWSS_LOG_NOTICE("ICMP session missing for state change notification id:%" PRIx64 " state: %s", id, m_session_state_lkup.at(state).c_str());
                continue;
            }

            // handle state update
            if (state != m_icmp_session_lookup[id].state)
            {
                auto key = m_icmp_session_lookup[id].db_key;
                vector<FieldValueTuple> fvVector;
                m_stateIcmpSessionTable.get(key, fvVector);

                fvVector.push_back({IcmpSaiSessionHandler::m_state_fname, m_session_state_lkup.at(state)});

                m_stateIcmpSessionTable.set(key, fvVector);

                SWSS_LOG_NOTICE("ICMP session state for %s changed from %s to %s", key.c_str(),
                            m_session_state_lkup.at(m_icmp_session_lookup[id].state).c_str(), m_session_state_lkup.at(state).c_str());

                m_icmp_session_lookup[id].state = state;
            }
        }

        sai_deserialize_free_icmp_echo_session_state_ntf(count, icmpSessionState);
    }
}

bool IcmpOrch::create_icmp_session(const string& key, const vector<FieldValueTuple>& data)
{
    IcmpSaiSessionHandler sai_session_handler(*this);

    // initialize the sai session handler
    auto init_status = sai_session_handler.init(sai_icmp_echo_api, key);
    if (init_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        SWSS_LOG_INFO("ICMP session creation failed key(%s), init_status(%s)", key.c_str(),
                SaiOffloadStatusStrMap.at(init_status).c_str());
        return true;
    }

    if (!m_register_state_change_notif)
    {
        if (!sai_session_handler.register_state_change_notification())
        {
            // return false to retry registration
            return false;
        }
        m_register_state_change_notif = true;
    }

    auto create_status = sai_session_handler.create(data);
    if (create_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        SWSS_LOG_INFO("ICMP session creation failed key(%s), create_status(%s)", key.c_str(),
                SaiOffloadStatusStrMap.at(create_status).c_str());
        // do not consume the entry for retries
        bool skip_entry = create_status != SaiOffloadHandlerStatus::RETRY_VALID_ENTRY;
        return skip_entry;
    }

    // update the STATE DB and local session maps
    auto& fvVector = sai_session_handler.get_fv_vector();
    auto state = m_session_state_lkup.at(SAI_ICMP_ECHO_SESSION_STATE_DOWN);
    fvVector.push_back({IcmpSaiSessionHandler::m_state_fname, state});
    auto& state_db_key = sai_session_handler.get_state_db_key();

    m_stateIcmpSessionTable.set(state_db_key, fvVector);
    auto session_id = sai_session_handler.get_session_id();
    IcmpSessionDataCache session_cache{session_id, sai_session_handler.get_fv_map()};
    m_icmp_session_map[key] = session_cache;
    m_icmp_session_lookup[session_id] = {state_db_key, SAI_ICMP_ECHO_SESSION_STATE_DOWN};

    SWSS_LOG_NOTICE("Created ICMP offload session key(%s)", key.c_str());
    return true;
}

bool IcmpOrch::update_icmp_session(const string& key, const fv_vector_t& data)
{
    IcmpSaiSessionHandler sai_session_handler(*this);

    // initialize the sai session handler
    auto init_status = sai_session_handler.init(sai_icmp_echo_api, key);
    if (init_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        SWSS_LOG_INFO("ICMP session update failed key(%s), init_status(%s)", key.c_str(),
                SaiOffloadStatusStrMap.at(init_status).c_str());
        return true;
    }

    auto session_id = m_icmp_session_map[key].session_id;
    auto& fv_map = m_icmp_session_map[key].fv_map;
    auto update_status = sai_session_handler.update(session_id, data, fv_map);
    if (update_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        SWSS_LOG_INFO("ICMP session update failed key(%s), update_status(%s)", key.c_str(),
                SaiOffloadStatusStrMap.at(update_status).c_str());
        // do not consume the entry for retries
        bool skip_entry = update_status != SaiOffloadHandlerStatus::RETRY_VALID_ENTRY;
        return skip_entry;
    }

    // update the STATE DB and local session maps
    auto& fvVector = sai_session_handler.get_fv_vector();
    if (fvVector.size()) {
        auto& state_db_key = sai_session_handler.get_state_db_key();
        auto& fv_map_upd = sai_session_handler.get_fv_map();
        m_stateIcmpSessionTable.set(state_db_key, fvVector);
        IcmpSessionDataCache session_cache{session_id, fv_map_upd};
        m_icmp_session_map[key] = session_cache;

        SWSS_LOG_NOTICE("Updated ICMP offload session key(%s)", key.c_str());
    }
    return true;
}

bool IcmpOrch::remove_icmp_session(const string& key)
{
    if (m_icmp_session_map.find(key) == m_icmp_session_map.end())
    {
        SWSS_LOG_ERROR("Request to remove non-existing ICMP session for %s", key.c_str());
        return true;
    }

    IcmpSaiSessionHandler sai_session_handler(*this);

    // initialize the sai session handler
    auto init_status = sai_session_handler.init(sai_icmp_echo_api, key);
    if (init_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        SWSS_LOG_INFO("ICMP session removal failed key(%s), init_status(%s)", key.c_str(),
                SaiOffloadStatusStrMap.at(init_status).c_str());
        return true;
    }

    sai_object_id_t icmp_session_id = m_icmp_session_map[key].session_id;
    auto remove_status = sai_session_handler.remove(icmp_session_id);
    if ( remove_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        // do not consume the entry for retries
        SWSS_LOG_INFO("ICMP session removal failed key(%s), remove_status(%s)", key.c_str(),
                SaiOffloadStatusStrMap.at(remove_status).c_str());
        bool skip_entry = remove_status != SaiOffloadHandlerStatus::RETRY_VALID_ENTRY;
        return skip_entry;
    }

    // delete the session from state db and remove them from local maps
    m_stateIcmpSessionTable.del(m_icmp_session_lookup[icmp_session_id].db_key);

    m_icmp_session_map.erase(key);
    m_icmp_session_lookup.erase(icmp_session_id);

    SWSS_LOG_NOTICE("Removed ICMP offload session key(%s)", key.c_str());
    return true;
}

const std::string IcmpSaiSessionHandler::m_name = "IcmpOffload";

const std::string IcmpSaiSessionHandler::m_tx_interval_fname        = "tx_interval";
const std::string IcmpSaiSessionHandler::m_rx_interval_fname        = "rx_interval";
const std::string IcmpSaiSessionHandler::m_src_ip_fname             = "src_ip";
const std::string IcmpSaiSessionHandler::m_dst_ip_fname             = "dst_ip";
const std::string IcmpSaiSessionHandler::m_src_mac_fname            = "src_mac";
const std::string IcmpSaiSessionHandler::m_dst_mac_fname            = "dst_mac";
const std::string IcmpSaiSessionHandler::m_tos_fname                = "tos";
const std::string IcmpSaiSessionHandler::m_ttl_fname                = "ttl";
const std::string IcmpSaiSessionHandler::m_state_fname              = "state";
const std::string IcmpSaiSessionHandler::m_session_cookie_fname     = "session_cookie";
const std::string IcmpSaiSessionHandler::m_session_guid_fname       = "session_guid";
const std::string IcmpSaiSessionHandler::m_hw_lookup_fname          = "hw_lookup";
const std::string IcmpSaiSessionHandler::m_nexthop_switchover_fname = "nexthop_switchover";
const std::string IcmpSaiSessionHandler::m_session_type_normal      = "NORMAL";
const std::string IcmpSaiSessionHandler::m_session_type_rx          = "RX";

const std::unordered_set<std::string> IcmpSaiSessionHandler::m_update_fields = {
    m_tx_interval_fname,
    m_rx_interval_fname,
    m_tos_fname,
};

void IcmpSaiSessionHandler::handle_tx_interval_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.u32 = time_msec_to_usec(to_uint<uint32_t>(sval));
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL] = val;
    fvVector.push_back({m_tx_interval_fname, sval});
}

void IcmpSaiSessionHandler::handle_rx_interval_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.u32 = time_msec_to_usec(to_uint<uint32_t>(sval));
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL] = val;
    fvVector.push_back({m_rx_interval_fname, sval});
}

void IcmpSaiSessionHandler::handle_src_ip_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    auto src_ip = IpAddress(sval);
    swss::copy(val.ipaddr, src_ip);
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS] = val;
    fvVector.push_back({m_src_ip_fname, sval});
    sai_attribute_value_t hdr_type;
    hdr_type.u8 = src_ip.isV4() ? 4 : 6;
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION] = hdr_type;
}

void IcmpSaiSessionHandler::handle_dst_ip_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    swss::copy(val.ipaddr, IpAddress(sval));
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS] = val;
    fvVector.push_back({m_dst_ip_fname, sval});
}

void IcmpSaiSessionHandler::handle_src_mac_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    auto mac = MacAddress(sval);
    memcpy(val.mac, mac.getMac(), sizeof(sai_mac_t));
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_SRC_MAC_ADDRESS] = val;
}

void IcmpSaiSessionHandler::handle_dst_mac_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    auto mac = MacAddress(sval);
    memcpy(val.mac, mac.getMac(), sizeof(sai_mac_t));
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS] = val;
}

void IcmpSaiSessionHandler::handle_tos_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.u8 = to_uint<uint8_t>(sval);
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_TOS] = val;
}

void IcmpSaiSessionHandler::handle_ttl_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.u8 = to_uint<uint8_t>(sval);
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_TTL] = val;
}

void IcmpSaiSessionHandler::handle_session_guid_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.u64 = to_uint<uint64_t>(sval);
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_GUID] = val;
    fvVector.push_back({m_session_guid_fname, sval});
}

void IcmpSaiSessionHandler::handle_session_cookie_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.u32 = to_uint<uint32_t>(sval);
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_COOKIE] = val;
    fvVector.push_back({m_session_cookie_fname, sval});
}

void IcmpSaiSessionHandler::handle_hw_lookup_field(std::string& sval, sai_attr_id_val_map_t& id_val_map,
        fv_vector_t& fvVector)
{
    sai_attribute_value_t val;
    val.booldata = (sval == "true") ? true : false;
    id_val_map[SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID] = val;
}

// ICMP Sai session attribute handlers
sai_attr_handler_map_t IcmpSaiSessionHandler::m_handler_map = {

    {m_tx_interval_fname,    std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL,
                                             IcmpSaiSessionHandler::handle_tx_interval_field)},
    {m_rx_interval_fname,    std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL,
                                             IcmpSaiSessionHandler::handle_rx_interval_field)},
    {m_src_ip_fname,         std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS,
                                             IcmpSaiSessionHandler::handle_src_ip_field)},
    {m_dst_ip_fname,         std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS,
                                             IcmpSaiSessionHandler::handle_dst_ip_field)},
    {m_src_mac_fname,        std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_SRC_MAC_ADDRESS,
                                             IcmpSaiSessionHandler::handle_src_mac_field)},
    {m_dst_mac_fname,        std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS,
                                             IcmpSaiSessionHandler::handle_dst_mac_field)},
    {m_tos_fname,            std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_TOS,
                                             IcmpSaiSessionHandler::handle_tos_field)},
    {m_ttl_fname,            std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_TTL,
                                             IcmpSaiSessionHandler::handle_ttl_field)},
    {m_session_guid_fname,   std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_GUID,
                                             IcmpSaiSessionHandler::handle_session_guid_field)},
    {m_session_cookie_fname, std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_COOKIE,
                                             IcmpSaiSessionHandler::handle_session_cookie_field)},
    {m_hw_lookup_fname,      std::make_tuple(SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID,
                                             IcmpSaiSessionHandler::handle_hw_lookup_field)}
};

SaiOffloadHandlerStatus IcmpSaiSessionHandler::do_init(sai_icmp_echo_api_t *api)
{
    size_t vrf_pos = m_key.find(delimiter);
    if (vrf_pos == string::npos)
    {
        SWSS_LOG_ERROR("%s, Failed to parse key %s, no vrf is given", m_name.c_str(), m_key.c_str());
        return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
    }

    size_t ifname_pos = m_key.find(delimiter, vrf_pos + 1);
    if (ifname_pos == string::npos)
    {
        SWSS_LOG_ERROR("%s, Failed to parse key %s, no ifname is given", m_name.c_str(), m_key.c_str());
        return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
    }

    size_t guid_pos = m_key.find(delimiter, ifname_pos + 1);
    if (guid_pos == string::npos)
    {
        SWSS_LOG_ERROR("%s, Failed to parse key %s, no guid is given", m_name.c_str(), m_key.c_str());
        return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
    }

    m_vrf_name = m_key.substr(0, vrf_pos);
    m_alias = m_key.substr(vrf_pos + 1, ifname_pos - vrf_pos - 1);
    m_guid = m_key.substr(ifname_pos + 1, guid_pos - ifname_pos - 1);
    m_session_type = m_key.substr(guid_pos + 1);
    if (m_session_type == "")
    {
        m_session_type = m_session_type_normal;
    }

    m_state_db_key = IcmpOrch::get_state_db_key(m_vrf_name, m_alias, m_guid, m_session_type);

    // initialize the sai icmp echo session function pointers
    sai_create_session = api->create_icmp_echo_session;
    sai_remove_session = api->remove_icmp_echo_session;
    sai_set_session_attrib = api->set_icmp_echo_session_attribute;
    sai_get_session_attrib = api->get_icmp_echo_session_attribute;

    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

SaiOffloadHandlerStatus IcmpSaiSessionHandler::do_create()
{
    // updating the tx_interval to 0 for PEER sessions makes sure
    // that hardware will not send echo requests for the PEER session
    if (m_session_type == m_session_type_rx)
    {
        SWSS_LOG_NOTICE("%s, Tx interval being reset to 0 for RX session, %s", m_name.c_str(), m_key.c_str());
        sai_attribute_value_t val;
        val.u32 = 0;
        m_attr_val_map[SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL] = val;
        m_fv_vector.push_back({m_tx_interval_fname, "0"});
        m_fv_map[m_tx_interval_fname] = "0";
    }

    // update the hw_lookup parameter in fv_vector
    auto& hw_lookup_attr_val = m_attr_val_map[SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID];
    if (hw_lookup_attr_val.booldata)
    {
        m_fv_vector.push_back({m_hw_lookup_fname, "true"});
    }
    else
    {
        m_fv_vector.push_back({m_hw_lookup_fname, "false"});
    }

    // set the guid that we got in key
    auto hsearch = m_handler_map.find(m_session_guid_fname);
    if (hsearch != m_handler_map.end())
    {
        auto& htuple = hsearch->second;
        auto& handler = std::get<1>(htuple);
        handler(m_guid, m_attr_val_map, m_fv_vector);
        m_fv_map[m_session_guid_fname] = m_guid;
    }
    else
    {
        // this should not never happen
        SWSS_LOG_ERROR("%s, GUID handler not found, %s", m_name.c_str(), m_key.c_str());
        return SaiOffloadHandlerStatus::FAILED_VALID_ENTRY;
    }

    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

SaiOffloadHandlerStatus IcmpSaiSessionHandler::do_remove()
{
    // no special handling required
    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

SaiOffloadHandlerStatus IcmpSaiSessionHandler::do_update()
{
    // do not update tx_interval for RX sessions
    if ((m_fv_map.find(m_tx_interval_fname) != m_fv_map.end()) &&
       (m_session_type == m_session_type_rx))
    {
        SWSS_LOG_NOTICE("%s, Not updating Tx interval for RX session, %s", m_name.c_str(), m_key.c_str());
        m_attr_val_map.erase(SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL);

        for (auto it = m_fv_vector.begin(); it != m_fv_vector.end();)
        {
            if (fvField(*it) == m_tx_interval_fname)
            {
                it = m_fv_vector.erase(it);
            } else {
                it++;
            }
        }

        m_fv_map.erase(m_tx_interval_fname);
    }

    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

void IcmpSaiSessionHandler::on_state_change(uint32_t count, sai_icmp_echo_session_state_notification_t *data)
{
    // we do not use this registered notification handler
    // as it is called in a separate thread of sairedis
}

