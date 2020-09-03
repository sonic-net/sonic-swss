#include "macsecorch.h"
#include "gearboxutils.h"

#include <macaddress.h>
#include <redisapi.h>
#include <redisclient.h>
#include <sai_serialize.h>

#include <vector>
#include <sstream>
#include <algorithm>
#include <functional>
#include <stack>

/* Only for Debug */

#define MockSAIFunc(FUNC_NAME)  \
sai_status_t FUNC_NAME(...)     \
{                               \
    return SAI_STATUS_SUCCESS;  \
}

struct MockSAIAPI
{
    sai_status_t get_switch_attribute(
        sai_object_id_t switch_id,
        uint32_t attr_count,
        sai_attribute_t *attr_list)
    {
        if (attr_count != 1)
        {
            return SAI_STATUS_FAILURE;
        }
        if (attr_list[0].id == SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY)
        {
            attr_list[0].value.u32 = 100;
        }
        else if (attr_list[0].id == SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY)
        {
            attr_list[0].value.u32 = 0;
        }
        return SAI_STATUS_SUCCESS;
    }
    MockSAIFunc(set_port_attribute);
    MockSAIFunc(create_macsec);
    MockSAIFunc(remove_macsec);
    MockSAIFunc(create_macsec_port);
    MockSAIFunc(remove_macsec_port);
    sai_status_t get_macsec_attribute(
        sai_object_id_t switch_id,
        uint32_t attr_count,
        sai_attribute_t *attr_list)
    {
        if (attr_count != 1)
        {
            return SAI_STATUS_FAILURE;
        }
        if (attr_list[0].id == SAI_MACSEC_ATTR_SCI_IN_INGRESS_MACSEC_ACL)
        {
            attr_list[0].value.booldata = true;
        }
        return SAI_STATUS_SUCCESS;
    }
    MockSAIFunc(create_macsec_flow);
    MockSAIFunc(remove_macsec_flow);
    MockSAIFunc(create_macsec_sc);
    MockSAIFunc(remove_macsec_sc);
    MockSAIFunc(create_macsec_sa);
    MockSAIFunc(remove_macsec_sa);
    MockSAIFunc(create_acl_table);
    MockSAIFunc(remove_acl_table);
    MockSAIFunc(create_acl_entry);
    MockSAIFunc(remove_acl_entry);
    MockSAIFunc(set_acl_entry_attribute);
};

static MockSAIAPI mock_api;

static MockSAIAPI * sai_macsec_api = &mock_api;
static MockSAIAPI * sai_acl_api = &mock_api;
static MockSAIAPI * sai_port_api = &mock_api;
static MockSAIAPI * sai_switch_api = &mock_api;

#define MOCK_RETURN_BOOL return true;

/* Global Variables*/

#define EAPOL_ETHER_TYPE 0x888e
#define MACSEC_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 1000

// extern sai_macsec_api_t *sai_macsec_api;
// extern sai_acl_api_t *sai_acl_api;
// extern sai_port_api_t *sai_port_api;
// extern sai_switch_api_t *sai_switch_api;

static const std::vector<std::string> macsec_egress_sa_stats =
    {
        "SAI_MACSEC_SA_ATTR_XPN",
};

static const std::vector<std::string> macsec_ingress_sa_stats =
    {
        "SAI_MACSEC_SA_ATTR_MINIMUM_XPN",
};

/* Helpers */

template <typename T>
static bool split(const std::string &input, char delimiter, T &output)
{
    if (input.find(delimiter) != std::string::npos)
    {
        return false;
    }
    std::istringstream istream(input);
    istream >> output;
    return true;
}

template <typename T, typename... Args>
static bool split(const std::string &input, char delimiter, T &output, Args &... args)
{
    auto pos = input.find(delimiter);
    if (pos == std::string::npos)
    {
        return false;
    }
    std::istringstream istream(input.substr(0, pos));
    istream >> output;
    return split(input.substr(pos + 1, input.length() - pos - 1), delimiter, args...);
}

template <typename T>
static std::string join(const T &input)
{
    std::ostringstream ostream;
    ostream << input;
    return ostream.str();
}

template <typename T>
static std::string join(char delimiter, const T &input)
{
    return join(input);
}

template <typename T, typename... Args>
static std::string join(char delimiter, const T &input, const Args &... args)
{
    std::ostringstream ostream;
    ostream << input << delimiter << join(delimiter, args...);
    return ostream.str();
}

template <class T>
static bool get_value(
    const MACsecOrch::TaskArgs &ta,
    const std::string &field,
    T &value)
{
    SWSS_LOG_ENTER();

    std::string target_field = field;
    std::transform(
        target_field.begin(),
        target_field.end(),
        target_field.begin(),
        ::tolower);
    auto itr = std::find_if(
        ta.begin(),
        ta.end(),
        [&](const MACsecOrch::TaskArgs::value_type &entry) {
            std::string field = fvField(entry);
            std::transform(
                field.begin(),
                field.end(),
                field.begin(),
                ::tolower);
            return field == target_field;
        });
    if (itr != ta.end())
    {
        std::istringstream istream(fvValue(*itr));
        istream >> value;
        SWSS_LOG_DEBUG(
            "Set field '%s' as '%s'",
            field.c_str(),
            fvValue(*itr).c_str());
        return true;
    }
    SWSS_LOG_WARN("Cannot find field : %s", field.c_str());
    return false;
}

static std::istringstream &operator>>(
    std::istringstream &istream,
    bool &b)
{
    std::string buffer = istream.str();
    std::transform(
        buffer.begin(),
        buffer.end(),
        buffer.begin(),
        ::tolower);
    if (buffer == "true" || buffer == "1")
    {
        b = true;
    }
    else if (buffer == "false" || buffer == "0")
    {
        b = false;
    }
    else
    {
        throw std::invalid_argument("Invalid bool string : " + buffer);
    }

    return istream;
}

static bool hex_to_binary(
    const std::string &hex_str,
    std::uint8_t *buffer,
    size_t buffer_length)
{
    size_t buffer_cur = 0;
    size_t hex_cur = 0;
    while (buffer_cur < buffer_length)
    {
        if ((hex_cur + 1) >= hex_str.length())
        {
            return false;
        }
        std::stringstream stream;
        stream << std::hex;
        stream << hex_str[hex_cur++];
        stream << hex_str[hex_cur++];
        stream >> buffer[buffer_cur++];
    }
    return hex_cur == hex_str.length();
}

struct MACsecSAK
{
    sai_macsec_sak_t m_sak;
    bool m_sak_256_enable;
};

static std::istringstream &operator>>(
    std::istringstream &istream,
    MACsecSAK &sak)
{
    SWSS_LOG_ENTER();
    const std::string &buffer = istream.str();
    bool convert_done = false;
    if (buffer.length() == sizeof(sak.m_sak))
    {
        sak.m_sak_256_enable = false;
        convert_done = hex_to_binary(
            buffer,
            &sak.m_sak[8],
            sizeof(sak.m_sak) / 2);
    }
    else if (buffer.length() == sizeof(sak.m_sak) * 2)
    {
        sak.m_sak_256_enable = true;
        convert_done = hex_to_binary(
            buffer,
            sak.m_sak,
            sizeof(sak.m_sak));
    }
    if (!convert_done)
    {
        throw std::invalid_argument("Invalid SAK : " + buffer);
    }
    return istream;
}

struct MACsecSalt
{
    sai_macsec_salt_t m_salt;
};

static std::istringstream &operator>>(
    std::istringstream &istream,
    MACsecSalt &salt)
{
    SWSS_LOG_ENTER();
    const std::string &buffer = istream.str();

    if (
        (buffer.length() != sizeof(salt.m_salt) * 2) || (!hex_to_binary(buffer, salt.m_salt, sizeof(salt.m_salt))))
    {
        throw std::invalid_argument("Invalid SALT : " + buffer);
    }
    return istream;
}

struct MACsecAuthKey
{
    sai_macsec_auth_key_t m_auth_key;
};

static std::istringstream &operator>>(
    std::istringstream &istream,
    MACsecAuthKey &auth_key)
{
    SWSS_LOG_ENTER();
    const std::string &buffer = istream.str();

    if (
        (buffer.length() != sizeof(auth_key.m_auth_key) * 2) || (!hex_to_binary(
                                                                    buffer,
                                                                    auth_key.m_auth_key,
                                                                    sizeof(auth_key.m_auth_key))))
    {
        throw std::invalid_argument("Invalid Auth Key : " + buffer);
    }
    return istream;
}

/* Recover from a fail action by a serial of pre-defined recover actions */
class RecoverStack
{
public:
    ~RecoverStack()
    {
        pop_all(true);
    }
    void clear()
    {
        pop_all();
    }
    void add_action(std::function<void(void)> action)
    {
        m_recover_actions.push(action);
    }

private:
    void pop_all(bool do_recover = false)
    {
        while (!m_recover_actions.empty())
        {
            if (do_recover)
            {
                m_recover_actions.top()();
            }
            m_recover_actions.pop();
        }
    }
    std::stack<std::function<void(void)>> m_recover_actions;
};

/* MACsec Orchagent */

MACsecOrch::MACsecOrch(
    DBConnector *app_db,
    DBConnector *state_db,
    const std::vector<std::string> &tables,
    PortsOrch *port_orch) : Orch(app_db, tables),
                            m_port_orch(port_orch),
                            m_state_macsec_port(state_db, STATE_MACSEC_PORT_TABLE_NAME),
                            m_state_macsec_egress_sc(state_db, STATE_MACSEC_EGRESS_SC_TABLE_NAME),
                            m_state_macsec_ingress_sc(state_db, STATE_MACSEC_INGRESS_SC_TABLE_NAME),
                            m_state_macsec_egress_sa(state_db, STATE_MACSEC_EGRESS_SA_TABLE_NAME),
                            m_state_macsec_ingress_sa(state_db, STATE_MACSEC_INGRESS_SA_TABLE_NAME),
                            m_counter_db("COUNTERS_DB", 0),
                            m_macsec_counters_map(&m_counter_db, COUNTERS_MACSEC_NAME_MAP),
                            m_macsec_stat_manager(
                                COUNTERS_MACSEC_TABLE,
                                StatsMode::READ,
                                MACSEC_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
                            m_gearbox_table(app_db, "_GEARBOX_TABLE"),
                            m_gearbox_enabled(false)
{
    SWSS_LOG_ENTER();
}

MACsecOrch::~MACsecOrch()
{
    while (!m_macsec_ports.empty())
    {
        auto port = m_macsec_ports.begin();
        const MACsecOrch::TaskArgs temp;
        disableMACsecPort(port->first, temp);
    }
}

void MACsecOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!m_gearbox_enabled)
    {
        if (!initGearbox())
        {
            SWSS_LOG_WARN("Gearbox cannot be initialized.");
            return;
        }
    }

    using TaskType = std::tuple<const std::string, const std::string>;
    using TaskFunc = task_process_status (MACsecOrch::*)(
        const std::string &,
        const TaskArgs &);
    const static std::map<TaskType, TaskFunc> TaskMap = {
        {{APP_MACSEC_PORT_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::updateMACsecPort},
        {{APP_MACSEC_PORT_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::disableMACsecPort},
        {{APP_MACSEC_EGRESS_SC_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::updateEgressSC},
        {{APP_MACSEC_EGRESS_SC_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::deleteEgressSC},
        {{APP_MACSEC_INGRESS_SC_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::updateIngressSC},
        {{APP_MACSEC_INGRESS_SC_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::deleteIngressSC},
        {{APP_MACSEC_EGRESS_SA_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::updateEgressSA},
        {{APP_MACSEC_EGRESS_SA_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::deleteEgressSA},
        {{APP_MACSEC_INGRESS_SA_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::updateIngressSA},
        {{APP_MACSEC_INGRESS_SA_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::deleteIngressSA},
    };

    const std::string &table_name = consumer.getTableName();
    auto itr = consumer.m_toSync.begin();
    while (itr != consumer.m_toSync.end())
    {
        task_process_status task_done = task_failed;
        auto &message = itr->second;
        const std::string &op = kfvOp(message);

        auto task = TaskMap.find(std::make_tuple(table_name, op));
        if (task != TaskMap.end())
        {
            task_done = (this->*task->second)(
                kfvKey(message),
                kfvFieldsValues(message));
        }
        else
        {
            SWSS_LOG_ERROR(
                "Unknown task : %s - %s",
                table_name.c_str(),
                op.c_str());
        }

        if (task_done == task_need_retry)
        {
            SWSS_LOG_DEBUG(
                "Task %s - %s need retry",
                table_name.c_str(),
                op.c_str());
            ++itr;
        }
        else
        {
            if (task_done != task_success)
            {
                SWSS_LOG_WARN("Task %s - %s fail",
                              table_name.c_str(),
                              op.c_str());
            }
            else
            {
                SWSS_LOG_DEBUG(
                    "Task %s - %s success",
                    table_name.c_str(),
                    op.c_str());
            }

            itr = consumer.m_toSync.erase(itr);
        }
    }
}

task_process_status MACsecOrch::updateMACsecPort(
    const std::string &port_name,
    const TaskArgs &port_attr)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    Port port;
    if (!m_port_orch->getPort(port_name, port))
    {
        SWSS_LOG_WARN("Port %s cannot be found.", port_name.c_str());
        return task_need_retry;
    }
    sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
    if (!getGearboxSwitchId(port, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto macsec_obj = initMACsecObject(switch_id);
    if (macsec_obj == m_macsec_objs.end())
    {
        SWSS_LOG_WARN("Cannot init MACsec at the port %s.", port_name.c_str());
        return task_failed;
    }
    recover.add_action([&]() {
        if (macsec_obj->second.m_ports.empty())
        {
            deinitMACsecObject(macsec_obj);
        }
    });

    std::shared_ptr<MACsecPort> macsec_port;
    auto macsec_port_itr = m_macsec_ports.find(port_name);
    if (macsec_port_itr == m_macsec_ports.end())
    {
        macsec_port = createMACsecPort(
            port_name,
            port.m_line_port_id,
            switch_id);
        if (macsec_port == nullptr)
        {
            SWSS_LOG_WARN("Cannot init MACsec port at the port %s.", port_name.c_str());
            return task_failed;
        }
        recover.add_action([&]() { deleteMACsecPort(port_name); });
        macsec_obj->second.m_ports[port_name] = macsec_port;
        recover.add_action([&]() { macsec_obj->second.m_ports.erase(port_name); });
    }
    else
    {
        macsec_port = macsec_port_itr->second;
    }

    get_value(port_attr, "enable_encrypt", macsec_port->m_enable_encrypt);
    get_value(port_attr, "send_sci", macsec_port->m_sci_in_sectag);
    if (get_value(port_attr, "enable", macsec_port->m_enable))
    {
        std::vector<MACsecSC *> scs;
        for (auto &sc : macsec_port->m_egress_scs)
        {
            scs.push_back(&(sc.second));
        }
        for (auto &sc : macsec_port->m_ingress_scs)
        {
            scs.push_back(&(sc.second));
        }
        for (auto &sc : scs)
        {
            // Change the ACL entry action from packet action to MACsec flow
            if (macsec_port->m_enable)
            {
                if (!setACLEntryMACsecFlow(sc->m_entry_id, sc->m_flow_id))
                {
                    SWSS_LOG_WARN("Cannot change the ACL entry action from packet action to MACsec flow");
                    return task_failed;
                }
                recover.add_action([&]() { setACLEntryPacketAction(sc->m_encoding_an, sc->m_flow_id); });
            }
            else
            {
                setACLEntryPacketAction(sc->m_encoding_an, sc->m_flow_id);
            }
        }
    }

    // If hardware matches SCI in ACL, the macsec_flow maps to an IEEE 802.1ae SecY object.
    // Multiple SCs can be associated with such a macsec_flow.
    // Then a specific value of SCI from the SecTAG in the packet is used to identify a specific SC
    // for that macsec_flow.
    // False means one flow can be associated with multiple ACL entries and multiple SC
    if (!macsec_obj->second.m_sci_in_ingress_macsec_acl)
    {
        if (!createMACsecFlow(macsec_port->m_egress_flow_id, switch_id, SAI_MACSEC_DIRECTION_EGRESS))
        {
            SWSS_LOG_WARN("Cannot create MACsec egress flow at the port %s.", port_name.c_str());
            return task_failed;
        }
        recover.add_action([&]() { deleteMACsecFlow(macsec_port->m_egress_flow_id); });

        if (!createMACsecFlow(macsec_port->m_ingress_flow_id, switch_id, SAI_MACSEC_DIRECTION_EGRESS))
        {
            SWSS_LOG_WARN("Cannot create MACsec ingress flow at the port %s.", port_name.c_str());
            return task_failed;
        }
        recover.add_action([&]() { deleteMACsecFlow(macsec_port->m_ingress_flow_id); });
    }

    if (!initACLTable(
            macsec_port->m_egress_acl_table,
            port.m_line_port_id,
            switch_id,
            SAI_MACSEC_DIRECTION_EGRESS,
            macsec_port->m_sci_in_sectag))
    {
        SWSS_LOG_WARN("Cannot init the ACL Table at the port %s.", port_name.c_str());
        return task_failed;
    }
    recover.add_action([&]() { deinitACLTable(macsec_port->m_egress_acl_table, port.m_line_port_id); });

    if (!initACLTable(
            macsec_port->m_ingress_acl_table,
            port.m_line_port_id,
            switch_id,
            SAI_MACSEC_DIRECTION_INGRESS,
            macsec_port->m_sci_in_sectag))
    {
        SWSS_LOG_WARN("Cannot init the ACL Table at the port %s.", port_name.c_str());
        return task_failed;
    }
    recover.add_action([&]() { deinitACLTable(macsec_port->m_ingress_acl_table, port.m_line_port_id); });

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    m_state_macsec_port.set(port_name, fvVector);

    recover.clear();

    return task_success;
}

task_process_status MACsecOrch::disableMACsecPort(
    const std::string &port_name,
    const TaskArgs &port_attr)
{
    SWSS_LOG_ENTER();

    sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto macsec_port = m_macsec_ports.find(port_name);
    if (macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec wasn't enabled at the port %s", port_name.c_str());
        return task_success;
    }

    Port port;
    if (!m_port_orch->getPort(port_name, port))
    {
        SWSS_LOG_INFO("Port %s cannot be found.", port_name.c_str());
        return task_success;
    }

    task_process_status result = task_success;

    if (!deinitACLTable(macsec_port->second->m_ingress_acl_table, port.m_line_port_id))
    {
        SWSS_LOG_WARN("Cannot deinit ingress ACL table at the port %s.", port_name.c_str());
        result = task_failed;
    }

    if (!deinitACLTable(macsec_port->second->m_egress_acl_table, port.m_line_port_id))
    {
        SWSS_LOG_WARN("Cannot deinit egress ACL table at the port %s.", port_name.c_str());
        result = task_failed;
    }

    if (!deleteMACsecPort(port_name))
    {
        SWSS_LOG_WARN("Cannot delete macsec port at the port %s.", port_name.c_str());
        result = task_failed;
    }

    auto macsec_obj = m_macsec_objs.find(switch_id);
    if (macsec_obj != m_macsec_objs.end())
    {
        macsec_obj->second.m_ports.erase(port_name);
        // All ports on this macsec object have been deleted.
        if (macsec_obj->second.m_ports.empty())
        {
            if (!deinitMACsecObject(macsec_obj))
            {
                SWSS_LOG_WARN("Cannot deinit macsec at the port %s.", port_name.c_str());
                result = task_failed;
            }
        }
    }

    m_state_macsec_port.del(port_name);

    return result;
}

task_process_status MACsecOrch::updateEgressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return updateMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::deleteEgressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::updateIngressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return updateMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_INGRESS);
}

task_process_status MACsecOrch::deleteIngressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_INGRESS);
}

task_process_status MACsecOrch::updateEgressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    std::string port_name;
    sai_uint64_t sci;
    macsec_an_t an;
    if (!split(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }
    auto port = m_macsec_ports.find(port_name);
    if (port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        // return task_need_retry;
        return task_failed;
    }
    auto sc = port->second->m_egress_scs.find(sci);
    if (sc == port->second->m_egress_scs.end())
    {
        SWSS_LOG_INFO("The MACsec SC %lu hasn't been created at the port %s.", sci, port_name.c_str());
        return task_need_retry;
    }
    if (sc->second.m_encoding_an == an)
    {
        return createMACsecSA(port_sci_an, sa_attr, SAI_MACSEC_DIRECTION_EGRESS);
    }
    return task_need_retry;
}

task_process_status MACsecOrch::deleteEgressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSA(port_sci_an, SAI_MACSEC_DIRECTION_EGRESS);
    ;
}

task_process_status MACsecOrch::updateIngressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    bool active = false;
    if (!get_value(sa_attr, "active", active))
    {
        SWSS_LOG_WARN("Active filed isn't existed at the SA %s.", port_sci_an.c_str());
        return task_failed;
    }
    if (active)
    {
        return createMACsecSA(port_sci_an, sa_attr, SAI_MACSEC_DIRECTION_INGRESS);
    }
    else
    {
        return deleteMACsecSA(port_sci_an, SAI_MACSEC_DIRECTION_INGRESS);
    }

    return task_success;
}

task_process_status MACsecOrch::deleteIngressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    return task_success;
}

bool MACsecOrch::initGearbox()
{
    SWSS_LOG_ENTER();

    GearboxUtils gearbox;
    m_gearbox_enabled = gearbox.isGearboxEnabled(&m_gearbox_table);
    if (m_gearbox_enabled)
    {
        m_gearbox_phy_map = gearbox.loadPhyMap(&m_gearbox_table);
        m_gearbox_interface_map = gearbox.loadInterfaceMap(&m_gearbox_table);

        SWSS_LOG_NOTICE("BOX: m_gearbox_phy_map size       = %d.", static_cast<int>(m_gearbox_phy_map.size()));
        SWSS_LOG_NOTICE("BOX: m_gearbox_interface_map size = %d.", static_cast<int>(m_gearbox_interface_map.size()));
    }
    return m_gearbox_enabled;
}

bool MACsecOrch::getGearboxSwitchId(const std::string &port_name, sai_object_id_t &switch_id) const
{
    SWSS_LOG_ENTER();

    Port port;
    if (!m_port_orch->getPort(port_name, port))
    {
        SWSS_LOG_WARN("Port %s cannot be found.", port_name.c_str());
        return false;
    }
    return getGearboxSwitchId(port, switch_id);
}

bool MACsecOrch::getGearboxSwitchId(const Port &port, sai_object_id_t &switch_id) const
{
    MOCK_RETURN_BOOL;
    SWSS_LOG_ENTER();

    auto phy_id = m_gearbox_interface_map.find(port.m_index);
    if (phy_id == m_gearbox_interface_map.end())
    {
        SWSS_LOG_INFO("The port %s doesn't bind to any gearbox.", port.m_alias.c_str());
        return false;
    }
    auto phy_oid_str = m_gearbox_phy_map.find(phy_id->second.phy_id);
    if (phy_oid_str == m_gearbox_phy_map.end())
    {
        SWSS_LOG_ERROR("Cannot find phy object (%d).", phy_id->second.phy_id);
        return false;
    }
    if (phy_oid_str->second.phy_oid.size() == 0)
    {
        SWSS_LOG_ERROR("BOX: Gearbox PHY phy_id:%d has an invalid phy_oid", phy_id->second.phy_id);
        return false;
    }
    sai_deserialize_object_id(phy_oid_str->second.phy_oid, switch_id);
    return true;
}

map<sai_object_id_t, MACsecOrch::MACsecObject>::iterator MACsecOrch::initMACsecObject(sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    auto macsec_obj = m_macsec_objs.emplace(switch_id, MACsecObject());
    if (!macsec_obj.second)
    {
        SWSS_LOG_INFO("The MACsec has been initialized at the switch %lu", switch_id);
        return macsec_obj.first;
    }
    recover.add_action([&]() { m_macsec_objs.erase(macsec_obj.first); });

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_ATTR_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_EGRESS;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec(
            &macsec_obj.first->second.m_egress_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot initialize MACsec egress object at the switch %lu", switch_id);
        return m_macsec_objs.end();
    }
    recover.add_action([&]() { sai_macsec_api->remove_macsec_port(macsec_obj.first->second.m_egress_id); });

    attrs.clear();
    attr.id = SAI_MACSEC_ATTR_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_INGRESS;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec(
            &macsec_obj.first->second.m_ingress_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot initialize MACsec ingress object at the switch %lu", switch_id);
        return m_macsec_objs.end();
    }
    recover.add_action([&]() { sai_macsec_api->remove_macsec_port(macsec_obj.first->second.m_ingress_id); });

    attrs.clear();
    attr.id = SAI_MACSEC_ATTR_SCI_IN_INGRESS_MACSEC_ACL;
    attrs.push_back(attr);
    if (sai_macsec_api->get_macsec_attribute(
            macsec_obj.first->second.m_ingress_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN(
            "Cannot get MACsec attribution SAI_MACSEC_ATTR_SCI_IN_INGRESS_MACSEC_ACL at the switch %lu",
            switch_id);
        return m_macsec_objs.end();
    }
    macsec_obj.first->second.m_sci_in_ingress_macsec_acl = attrs.front().value.booldata;

    recover.clear();
    return macsec_obj.first;
}

bool MACsecOrch::deinitMACsecObject(sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    auto macsec_obj = m_macsec_objs.find(switch_id);
    if (macsec_obj == m_macsec_objs.end())
    {
        SWSS_LOG_INFO("The MACsec wasn't initialized at the switch %lu", switch_id);
        return true;
    }

    return deinitMACsecObject(macsec_obj);
}

bool MACsecOrch::deinitMACsecObject(map<sai_object_id_t, MACsecObject>::iterator macsec_obj)
{
    SWSS_LOG_ENTER();

    bool result = true;

    if (sai_macsec_api->remove_macsec(
            macsec_obj->second.m_egress_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot deinitialize MACsec egress object at the switch %lu", macsec_obj->first);
        result &= false;
    }

    if (sai_macsec_api->remove_macsec(
            macsec_obj->second.m_ingress_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot deinitialize MACsec ingress object at the switch %lu", macsec_obj->first);
        result &= false;
    }

    m_macsec_objs.erase(macsec_obj);
    return result;
}

std::shared_ptr<MACsecOrch::MACsecPort> MACsecOrch::createMACsecPort(
    const std::string &port_name,
    sai_object_id_t port_id,
    sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    auto macsec_port = m_macsec_ports.emplace(port_name, std::make_shared<MACsecPort>());
    if (!macsec_port.second)
    {
        SWSS_LOG_INFO("The MACsec port has been initialized at the port %s.", port_name.c_str());
        return macsec_port.first->second;
    }
    recover.add_action([&]() { m_macsec_ports.erase(macsec_port.first); });

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_EGRESS;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_PORT_ATTR_PORT_ID;
    attr.value.oid = port_id;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_port(
            &macsec_port.first->second->m_egress_port_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec egress port at the port %s", port_name.c_str());
        return nullptr;
    }
    recover.add_action([&]() { sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_egress_port_id); });

    attrs.clear();
    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_INGRESS;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_PORT_ATTR_PORT_ID;
    attr.value.oid = port_id;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_port(
            &macsec_port.first->second->m_ingress_port_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec ingress port at the port %s", port_name.c_str());
        return nullptr;
    }
    recover.add_action([&]() { sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_ingress_port_id); });

    macsec_port.first->second->m_enable_encrypt = true;
    macsec_port.first->second->m_sci_in_sectag = true;
    macsec_port.first->second->m_enable = false;

    recover.clear();
    return macsec_port.first->second;
}

bool MACsecOrch::deleteMACsecPort(const std::string &port_name)
{
    SWSS_LOG_ENTER();

    auto macsec_port = m_macsec_ports.find(port_name);
    if (macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_WARN("The MACsec port wasn't initialized at the port %s", port_name.c_str());
        return true;
    }

    for (auto &sc : macsec_port->second->m_egress_scs)
    {
        const std::string port_sci = join(':', port_name, sc.first);
        deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_EGRESS);
    }
    for (auto &sc : macsec_port->second->m_ingress_scs)
    {
        const std::string port_sci = join(':', port_name, sc.first);
        deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_INGRESS);
    }

    bool result = true;

    if (sai_macsec_api->remove_macsec_port(
            macsec_port->second->m_egress_port_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec egress port at the port %s", port_name.c_str());
        result &= false;
    }

    if (sai_macsec_api->remove_macsec_port(
            macsec_port->second->m_ingress_port_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec ingress port at the port %s", port_name.c_str());
        result &= false;
    }

    if (sai_macsec_api->remove_macsec_flow(
            macsec_port->second->m_egress_flow_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec egress flow at the port %s", port_name.c_str());
        result &= false;
    }

    if (sai_macsec_api->remove_macsec_flow(
            macsec_port->second->m_ingress_flow_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec ingress flow at the port %s", port_name.c_str());
        result &= false;
    }

    m_macsec_ports.erase(macsec_port);
    return true;
}

bool MACsecOrch::createMACsecFlow(
    sai_object_id_t &flow_id,
    sai_object_id_t switch_id,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    if (flow_id != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = direction;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_flow(
            &flow_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::deleteMACsecFlow(sai_object_id_t flow_id)
{
    if (sai_macsec_api->remove_macsec_flow(
            flow_id) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

task_process_status MACsecOrch::updateMACsecSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    sai_uint64_t sci;
    if (!split(port_sci, ':', port_name, sci))
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci.c_str());
        return task_failed;
    }

    auto m_macsec_port = m_macsec_ports.find(port_name);
    if (m_macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_need_retry;
    }
    sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto macsec_obj = m_macsec_objs.find(switch_id);
    if (macsec_obj == m_macsec_objs.end())
    {
        SWSS_LOG_INFO("The MACsec wasn't initialized at the switch %lu", switch_id);
        return task_failed;
    }

    auto &scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? m_macsec_port->second->m_egress_scs
            : m_macsec_port->second->m_ingress_scs;
    auto sc = scs.find(sci);
    // If SC has been created
    if (sc != scs.end())
    {
        return setEncodingAN(sc->second, sc_attr, direction);
    }

    return createMACsecSC(
        *(m_macsec_port->second),
        port_name,
        sc_attr,
        macsec_obj->second,
        sci,
        switch_id,
        direction);
}

task_process_status MACsecOrch::setEncodingAN(
    MACsecSC &sc,
    const TaskArgs &sc_attr,
    sai_int32_t direction)
{
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        if (!get_value(sc_attr, "encoding_an", sc.m_encoding_an))
        {
            SWSS_LOG_WARN("Wrong parameter, the encoding AN cannot be found");
            return task_failed;
        }
        return task_success;
    }
    else
    {
        SWSS_LOG_WARN("Cannot set encoding AN for the ingress SC");
        return task_failed;
    }
}

task_process_status MACsecOrch::createMACsecSC(
    MACsecPort &macsec_port,
    const std::string &port_name,
    const TaskArgs &sc_attr,
    const MACsecObject &macsec_obj,
    sai_uint64_t sci,
    sai_object_id_t switch_id,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    const std::string port_sci = join(':', port_name, sci);

    auto &scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? macsec_port.m_egress_scs
            : macsec_port.m_ingress_scs;
    auto sc_itr = scs.emplace(sci, MACsecSC());
    if (!sc_itr.second)
    {
        SWSS_LOG_ERROR("The SC %s has been created.", port_sci.c_str());
        return task_failed;
    }
    recover.add_action([&]() { scs.erase(sc_itr.first->first); });
    auto &sc = sc_itr.first->second;

    sai_uint32_t ssci;
    sc.m_xpn64_enable = false;
    if (get_value(sc_attr, "ssci", ssci) && ssci)
    {
        sc.m_xpn64_enable = true;
    }
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        get_value(sc_attr, "encoding_an", sc.m_encoding_an);
    }

    sc.m_flow_id = SAI_NULL_OBJECT_ID;

    // If SCI can only be used as ACL field
    // Which means one ingress flow can be associated with only one ACL entry and one SC.
    if (macsec_obj.m_sci_in_ingress_macsec_acl)
    {
        if (!createMACsecFlow(sc.m_flow_id, switch_id, direction))
        {
            SWSS_LOG_WARN("Cannot create MACsec Flow");
            return task_failed;
        }
        recover.add_action([&]() { deleteMACsecFlow(sc.m_flow_id); });
    }
    else
    {
        sc.m_flow_id =
            (direction == SAI_MACSEC_DIRECTION_EGRESS)
                ? macsec_port.m_egress_flow_id
                : macsec_port.m_ingress_flow_id;
    }

    if (!createMACsecSC(
            sc.m_sc_id,
            switch_id,
            direction,
            sc.m_flow_id,
            sci,
            ssci,
            sc.m_xpn64_enable))
    {
        SWSS_LOG_WARN("Create MACsec SC %s fail.", port_sci.c_str());
        return task_failed;
    }
    recover.add_action([&]() { deleteMACsecSC(sc.m_sc_id); });

    auto &table =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? macsec_port.m_egress_acl_table
            : macsec_port.m_ingress_acl_table;
    if (table.m_available_acl_priorities.empty())
    {
        SWSS_LOG_WARN("Available ACL priorities have been exhausted.");
        return task_failed;
    }
    sc.m_acl_priority = *(table.m_available_acl_priorities.begin());
    table.m_available_acl_priorities.erase(table.m_available_acl_priorities.begin());
    if (!createACLDataEntry(
        sc.m_entry_id,
        table.m_table_id,
        switch_id,
        macsec_port.m_sci_in_sectag,
        sci,
        sc.m_acl_priority))
    {
        SWSS_LOG_WARN("Cannot create ACL Data entry");
        return task_failed;
    }
    recover.add_action([&]()
    {
        deleteACLEntry(sc.m_entry_id);
        table.m_available_acl_priorities.insert(sc.m_acl_priority);
    });

    SWSS_LOG_NOTICE("MACsec SC %s is created.", port_sci.c_str());

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sc.set(join('|', port_name, sci), fvVector);
    }
    else
    {
        m_state_macsec_ingress_sc.set(join('|', port_name, sci), fvVector);
    }
    return task_success;
}

task_process_status MACsecOrch::deleteMACsecSC(
    const std::string &port_sci,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    sai_uint64_t sci;
    if (!split(port_sci, ':', port_name, sci))
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci.c_str());
        return task_failed;
    }

    auto macsec_port = m_macsec_ports.find(port_name);
    if (macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_WARN("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_failed;
    }

    sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto macsec_obj = m_macsec_objs.find(switch_id);
    if (macsec_obj == m_macsec_objs.end())
    {
        SWSS_LOG_INFO("The MACsec wasn't initialized at the switch %lu", switch_id);
        return task_failed;
    }

    auto &scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? macsec_port->second->m_egress_scs
            : macsec_port->second->m_ingress_scs;
    auto sc = scs.find(sci);
    if (sc == scs.end())
    {
        SWSS_LOG_WARN("The MACsec SC %s wasn't created", port_sci.c_str());
        return task_failed;
    }

    auto result = task_success;

    for (macsec_an_t an = 0; an < sizeof(sc->second.m_sa_ids) / sizeof(sc->second.m_sa_ids[0]); an++)
    {
        if (sc->second.m_sa_ids[an] != SAI_NULL_OBJECT_ID)
        {
            const std::string port_sci_an = join(':', port_sci, an);
            deleteMACsecSA(port_sci_an, direction);
        }
    }

    deleteACLEntry(sc->second.m_entry_id);
    auto &table =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? macsec_port->second->m_egress_acl_table
            : macsec_port->second->m_ingress_acl_table;
    table.m_available_acl_priorities.insert(sc->second.m_acl_priority);

    if (!deleteMACsecSC(sc->second.m_sc_id))
    {
        SWSS_LOG_WARN("The MACsec SC %s cannot be deleted", port_sci.c_str());
        result = task_failed;
    }
    macsec_port->second->m_egress_scs.erase(sc);

    SWSS_LOG_NOTICE("MACsec SC %s is deleted.", port_sci.c_str());

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sc.del(join('|', port_name, sci));
    }
    else
    {
        m_state_macsec_ingress_sc.del(join('|', port_name, sci));
    }

    return result;
}

bool MACsecOrch::createMACsecSC(
    sai_object_id_t &sc_id,
    sai_object_id_t switch_id,
    sai_int32_t direction,
    sai_object_id_t flow_id,
    sai_uint64_t sci,
    sai_uint32_t ssci,
    bool xpn64_enable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_SC_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = direction;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_SC_ATTR_FLOW_ID;
    attr.value.oid = flow_id;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_SC_ATTR_MACSEC_SCI;
    attr.value.u64 = sci;
    attrs.push_back(attr);
    if (xpn64_enable)
    {
        attr.id = SAI_MACSEC_SC_ATTR_MACSEC_SSCI;
        attr.value.u32 = ssci;
        attrs.push_back(attr);
    }
    attr.id = SAI_MACSEC_SC_ATTR_MACSEC_XPN64_ENABLE;
    attr.value.booldata = xpn64_enable;
    attrs.push_back(attr);

    if (sai_macsec_api->create_macsec_sc(
            &sc_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec egress SC %lu", sci);
        return false;
    }
    return true;
}

bool MACsecOrch::deleteMACsecSC(sai_object_id_t sc_id)
{
    SWSS_LOG_ENTER();

    if (sai_macsec_api->remove_macsec_sc(
            sc_id) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

task_process_status MACsecOrch::createMACsecSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    sai_uint64_t sci;
    macsec_an_t an;
    if (!split(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    auto macsec_port = m_macsec_ports.find(port_name);
    if (macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_need_retry;
    }
    sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto &scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? macsec_port->second->m_egress_scs
            : macsec_port->second->m_ingress_scs;
    auto sc = scs.find(sci);
    if (sc == scs.end())
    {
        SWSS_LOG_INFO("The MACsec SC %lu hasn't been created at the port %s.", sci, port_name.c_str());
        return task_need_retry;
    }

    MACsecSAK sak;
    MACsecSalt salt;
    MACsecAuthKey auth_key;
    try
    {
        if (!get_value(sa_attr, "sak", sak))
        {
            SWSS_LOG_WARN("The SAK isn't existed at SA %s", port_sci_an.c_str());
            return task_failed;
        }
        if (sc->second.m_xpn64_enable)
        {
            if (!get_value(sa_attr, "salt", salt))
            {
                SWSS_LOG_WARN("The salt isn't existed at SA %s", port_sci_an.c_str());
                return task_failed;
            }
        }
        if (!get_value(sa_attr, "auth_key", auth_key))
        {
            SWSS_LOG_WARN("The auth key isn't existed at SA %s", port_sci_an.c_str());
            return task_failed;
        }
    }
    catch (const std::invalid_argument &e)
    {
        SWSS_LOG_WARN("Invalid argument : %s", e.what());
        return task_failed;
    }
    sai_uint64_t pn;
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        if (!get_value(sa_attr, "init_pn", pn))
        {
            SWSS_LOG_WARN("The init pn isn't existed at SA %s", port_sci_an.c_str());
            return task_failed;
        }
    }
    else
    {
        if (!get_value(sa_attr, "lowest_acceptable_pn", pn))
        {
            SWSS_LOG_WARN("The init pn isn't existed at SA %s", port_sci_an.c_str());
            return task_failed;
        }
    }

    RecoverStack recover;

    if (!createMACsecSA(
            sc->second.m_sa_ids[an],
            switch_id,
            direction,
            sc->second.m_sc_id,
            an,
            macsec_port->second->m_enable_encrypt,
            sak.m_sak_256_enable,
            sak.m_sak,
            sc->second.m_xpn64_enable,
            salt.m_salt,
            auth_key.m_auth_key,
            pn))
    {
        SWSS_LOG_WARN("Cannot create the SA %s", port_sci_an.c_str());
        return task_failed;
    }
    recover.add_action([&]()
    {
        deleteMACsecSA(sc->second.m_sa_ids[an]);
        sc->second.m_sa_ids[an] = SAI_NULL_OBJECT_ID;
    });

    // If this SA is the first SA
    // change the ACL entry action from packet action to MACsec flow
    if (macsec_port->second->m_enable && get_active_sa_count(sc->second) == 1)
    {
        if (!setACLEntryMACsecFlow(sc->second.m_entry_id, sc->second.m_flow_id))
        {
            SWSS_LOG_WARN("Cannot change the ACL entry action from packet action to MACsec flow");
            return task_failed;
        }
        recover.add_action([&]() { setACLEntryPacketAction(sc->second.m_encoding_an, sc->second.m_flow_id); });
    }

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        installCounter(port_sci_an, sc->second.m_sa_ids[an], macsec_egress_sa_stats);
        m_state_macsec_egress_sa.set(join('|', port_name, sci, an), fvVector);
    }
    else
    {
        installCounter(port_sci_an, sc->second.m_sa_ids[an], macsec_ingress_sa_stats);
        m_state_macsec_ingress_sa.set(join('|', port_name, sci, an), fvVector);
    }

    SWSS_LOG_NOTICE("MACsec SA %s is created.", port_sci_an.c_str());
    return task_success;
}

task_process_status MACsecOrch::deleteMACsecSA(
    const std::string &port_sci_an,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    sai_uint64_t sci;
    macsec_an_t an;
    if (!split(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    auto macsec_port = m_macsec_ports.find(port_name);
    if (macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_WARN("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto &scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? macsec_port->second->m_egress_scs
            : macsec_port->second->m_ingress_scs;

    auto sc = scs.find(sci);
    if (sc == scs.end())
    {
        SWSS_LOG_WARN("The MACsec SC %lu hasn't been created at the port %s.", sci, port_name.c_str());
        return task_need_retry;
    }

    if (sc->second.m_sa_ids[an] == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("The MACsec SA %s hasn't been created.", port_sci_an.c_str());
        return task_failed;
    }

    // If this SA is the last SA
    // change the ACL entry action from MACsec flow to packet action
    if (get_active_sa_count(sc->second) == 1)
    {
        if (!setACLEntryPacketAction(sc->second.m_entry_id, sc->second.m_flow_id))
        {
            SWSS_LOG_WARN("Cannot change the ACL entry action from MACsec flow to packet action");
        }
    }

    uninstallCounter(port_sci_an, sc->second.m_sa_ids[an]);
    if (!deleteMACsecSA(sc->second.m_sa_ids[an]))
    {
        SWSS_LOG_WARN("Cannot delete the MACsec SA %s.", port_sci_an.c_str());
    }
    sc->second.m_sa_ids[an] = SAI_NULL_OBJECT_ID;

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sa.del(join('|', port_name, sci, an));
    }
    else
    {
        m_state_macsec_ingress_sa.del(join('|', port_name, sci, an));
    }

    SWSS_LOG_NOTICE("MACsec SA %s is deleted.", port_sci_an.c_str());
    return task_success;
}

bool MACsecOrch::createMACsecSA(
    sai_object_id_t &sa_id,
    sai_object_id_t switch_id,
    sai_int32_t direction,
    sai_object_id_t sc_id,
    macsec_an_t an,
    bool encryption_enable,
    bool sak_256_bit,
    sai_macsec_sak_t sak,
    bool xpn64_enable,
    sai_macsec_salt_t salt,
    sai_macsec_auth_key_t auth_key,
    sai_uint64_t pn)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = direction;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_SA_ATTR_SC_ID;
    attr.value.oid = sc_id;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_SA_ATTR_AN;
    attr.value.u8 = static_cast<sai_uint8_t>(an);
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_SA_ATTR_ENCRYPTION_ENABLE;
    attr.value.u8 = encryption_enable;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_SA_ATTR_SAK_256_BITS;
    attr.value.u8 = sak_256_bit;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_SA_ATTR_SAK;
    std::copy(sak, sak + sizeof(attr.value.macsecsak), attr.value.macsecsak);
    attrs.push_back(attr);

    if (xpn64_enable)
    {
        attr.id = SAI_MACSEC_SA_ATTR_SALT;
        std::copy(salt, salt + sizeof(attr.value.macsecsalt), attr.value.macsecsalt);
        attrs.push_back(attr);
    }

    attr.id = SAI_MACSEC_SA_ATTR_AUTH_KEY;
    std::copy(auth_key, auth_key + sizeof(attr.value.macsecauthkey), attr.value.macsecauthkey);
    attrs.push_back(attr);

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        attr.id = SAI_MACSEC_SA_ATTR_XPN;
        attr.value.u64 = pn;
        attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_MACSEC_SA_ATTR_MINIMUM_XPN;
        attr.value.u64 = pn;
        attrs.push_back(attr);
    }

    if (sai_macsec_api->create_macsec_sa(
            &sa_id,
            switch_id,
            static_cast<uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::deleteMACsecSA(sai_object_id_t sa_id)
{
    SWSS_LOG_ENTER();
    if (sai_macsec_api->remove_macsec_sa(
            sa_id) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

std::uint8_t MACsecOrch::get_active_sa_count(const MACsecSC &sc) const
{
    std::uint8_t count = 0;
    for (macsec_an_t an = 0; an < sizeof(sc.m_sa_ids) / sizeof(sc.m_sa_ids[0]); an++)
    {
        if (sc.m_sa_ids[an] != SAI_NULL_OBJECT_ID)
        {
            count++;
        }
    }
    return count;
}

void MACsecOrch::installCounter(
    const std::string &obj_name,
    sai_object_id_t obj_id,
    const std::vector<std::string> &stats)
{
    FieldValueTuple tuple(obj_name, sai_serialize_object_id(obj_id));
    vector<FieldValueTuple> fields;
    fields.push_back(tuple);
    m_macsec_counters_map.set("", fields);

    std::unordered_set<std::string> counter_stats;
    for (const auto &stat : stats)
    {
        counter_stats.emplace(stat);
    }
    m_macsec_stat_manager.setCounterIdList(obj_id, CounterType::MACSEC, counter_stats);
}

void MACsecOrch::uninstallCounter(const std::string &obj_name, sai_object_id_t obj_id)
{
    m_macsec_stat_manager.clearCounterIdList(obj_id);

    RedisClient redisClient(&m_counter_db);
    redisClient.hdel(COUNTERS_PORT_NAME_MAP, obj_name);
}

bool MACsecOrch::initACLTable(
    ACLTable &acl_table,
    sai_object_id_t port_id,
    sai_object_id_t switch_id,
    sai_int32_t direction,
    bool sci_in_sectag)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    if (acl_table.m_table_id != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("The ACL table has been created");
        return true;
    }

    if (!createACLTable(acl_table.m_table_id, switch_id, direction, sci_in_sectag))
    {
        SWSS_LOG_WARN("Cannot create ACL table");
        return false;
    }
    recover.add_action([&]() { deleteACLTable(acl_table.m_table_id); });

    if (!createACLEAPOLEntry(
            acl_table.m_eapol_packet_forward_entry_id,
            acl_table.m_table_id,
            switch_id))
    {
        SWSS_LOG_WARN("Cannot create ACL EAPOL entry");
        return false;
    }
    recover.add_action([&]() { deleteACLEntry(acl_table.m_eapol_packet_forward_entry_id); });

    if (!bindACLTabletoPort(acl_table.m_table_id, port_id))
    {
        SWSS_LOG_WARN("Cannot bind ACL table");
        return false;
    }
    recover.add_action([&]() { unbindACLTable(port_id); });

    sai_uint32_t minimum_priority;
    if (!get_acl_minimum_priority(switch_id, minimum_priority))
    {
        return false;
    }
    sai_uint32_t maximum_priority;
    if (!get_acl_maximum_priority(switch_id, maximum_priority))
    {
        return false;
    }
    sai_uint32_t priority = minimum_priority + 1;
    while (priority < maximum_priority)
    {
        acl_table.m_available_acl_priorities.insert(priority);
        priority += 1;
    }

    recover.clear();
    return true;
}

bool MACsecOrch::deinitACLTable(ACLTable &acl_table, sai_object_id_t port_id)
{
    bool result = true;

    if (!unbindACLTable(port_id))
    {
        SWSS_LOG_WARN("Cannot unbind ACL table");
        result &= false;
    }
    // if (!deleteACLEntry(acl_table.m_data_packet_entry_id))
    // {
    //     SWSS_LOG_WARN("Cannot delete ACL entry at the port %s", port.m_alias.c_str());
    //     result &= false;
    // }
    if (!deleteACLEntry(acl_table.m_eapol_packet_forward_entry_id))
    {
        SWSS_LOG_WARN("Cannot delete ACL entry");
        result &= false;
    }
    if (!deleteACLTable(acl_table.m_table_id))
    {
        SWSS_LOG_WARN("Cannot delete ACL table");
        result &= false;
    }

    return result;
}

bool MACsecOrch::createACLTable(
    sai_object_id_t &table_id,
    sai_object_id_t switch_id,
    sai_int32_t direction,
    bool sci_in_sectag)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    // Create ingress MACsec ACL table for port_id1
    attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        attr.value.s32 = SAI_ACL_STAGE_EGRESS_MACSEC;
    }
    else
    {
        attr.value.s32 = SAI_ACL_STAGE_INGRESS_MACSEC;
    }
    attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    vector<std::int32_t> bpoint_list = {SAI_ACL_BIND_POINT_TYPE_PORT};
    attr.value.s32list.count = static_cast<std::uint32_t>(bpoint_list.size());
    attr.value.s32list.list = bpoint_list.data();
    attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_MAC;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_MACSEC_SCI;
    attr.value.booldata = sci_in_sectag;
    attrs.push_back(attr);

    if (sai_acl_api->create_acl_table(
            &table_id,
            switch_id,
            static_cast<std::uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::deleteACLTable(sai_object_id_t table_id)
{
    if (sai_acl_api->remove_acl_table(table_id) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::bindACLTabletoPort(sai_object_id_t table_id, sai_object_id_t port_id)
{
    sai_attribute_t attr;

    attr.id = SAI_PORT_ATTR_INGRESS_MACSEC_ACL;
    attr.value.oid = table_id;

    if (sai_port_api->set_port_attribute(
            port_id,
            &attr) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::unbindACLTable(sai_object_id_t port_id)
{
    sai_attribute_t attr;

    attr.id = SAI_PORT_ATTR_INGRESS_MACSEC_ACL;
    attr.value.oid = SAI_NULL_OBJECT_ID;

    if (sai_port_api->set_port_attribute(
            port_id,
            &attr) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::createACLEAPOLEntry(
    sai_object_id_t &entry_id,
    sai_object_id_t table_id,
    sai_object_id_t switch_id)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = table_id;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    if (!get_acl_maximum_priority(switch_id, attr.value.u32))
    {
        return false;
    }
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC;
    static const MacAddress nearest_non_tpmr_bridge("01:80:c2:00:00:03");
    nearest_non_tpmr_bridge.getMac(attr.value.aclfield.data.mac);
    attrs.push_back(attr);
    static const MacAddress mac_address_mask("ff:ff:ff:ff:ff:ff");
    mac_address_mask.getMac(attr.value.aclfield.mask.mac);
    attr.value.aclfield.enable = true;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE;
    attr.value.aclfield.data.u16 = EAPOL_ETHER_TYPE;
    attr.value.aclfield.mask.u16 = 0xFFFF;
    attr.value.aclfield.enable = true;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.parameter.s32 = SAI_PACKET_ACTION_FORWARD;
    attr.value.aclaction.enable = true;
    attrs.push_back(attr);
    if (sai_acl_api->create_acl_entry(
            &entry_id,
            switch_id,
            static_cast<std::uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::createACLDataEntry(
    sai_object_id_t &entry_id,
    sai_object_id_t table_id,
    sai_object_id_t switch_id,
    bool sci_in_sectag,
    sai_uint64_t sci,
    sai_uint32_t priority)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = table_id;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = priority;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DROP;
    attr.value.aclaction.enable = true;
    attrs.push_back(attr);
    attr.id = SAI_ACL_TABLE_ATTR_FIELD_MACSEC_SCI;
    attr.value.booldata = sci_in_sectag;
    attrs.push_back(attr);
    if (sci_in_sectag)
    {
        attr.id = SAI_ACL_ENTRY_ATTR_FIELD_MACSEC_SCI;
        attr.value.u64 = sci;
        attrs.push_back(attr);
    }
    if (sai_acl_api->create_acl_entry(
            &entry_id,
            switch_id,
            static_cast<std::uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::setACLEntryPacketAction(sai_object_id_t entry_id, sai_object_id_t flow_id)
{
    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.enable = true;
    if (sai_acl_api->set_acl_entry_attribute(
            entry_id,
            &attr) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_MACSEC_FLOW;
    attr.value.aclaction.parameter.oid = flow_id;
    attr.value.aclaction.enable = false;

    if (sai_acl_api->set_acl_entry_attribute(
            entry_id,
            &attr) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::setACLEntryMACsecFlow(sai_object_id_t entry_id, sai_object_id_t flow_id)
{
    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_MACSEC_FLOW;
    attr.value.aclaction.parameter.oid = flow_id;
    attr.value.aclaction.enable = true;

    if (sai_acl_api->set_acl_entry_attribute(
            entry_id,
            &attr) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.enable = false;
    if (sai_acl_api->set_acl_entry_attribute(
            entry_id,
            &attr) != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    return true;
}

bool MACsecOrch::deleteACLEntry(sai_object_id_t entry_id)
{
    if (sai_acl_api->remove_acl_entry(
            entry_id) != SAI_STATUS_SUCCESS)
    {
        return false;
    }
    return true;
}

bool MACsecOrch::get_acl_maximum_priority(sai_object_id_t switch_id, sai_uint32_t &priority) const
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY;
    attrs.push_back(attr);
    if (sai_switch_api->get_switch_attribute(
        switch_id,
        static_cast<std::uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Cannot fetch ACL maximum Priority from switch");
        return false;
    }
    priority = attrs.front().value.u32;

    return true;
}

bool MACsecOrch::get_acl_minimum_priority(sai_object_id_t switch_id, sai_uint32_t &priority) const
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY;
    attrs.push_back(attr);
    if (sai_switch_api->get_switch_attribute(
        switch_id,
        static_cast<std::uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Cannot fetch ACL maximum Priority from switch");
        return false;
    }
    priority = attrs.front().value.u32;

    return true;
}

static void wait_break()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void break_point()
{
    int state = 1;
    while (state != 0)
    {
        wait_break();
    }
}

