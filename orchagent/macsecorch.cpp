#include "macsecorch.h"

#include <macaddress.h>
#include <sai_serialize.h>
#include <swss/stringutility.h>
#include <swss/redisutility.h>
#include <swss/boolean.h>

#include <boost/algorithm/string.hpp>
#include <vector>
#include <sstream>
#include <algorithm>
#include <functional>
#include <stack>
#include <memory>
#include <typeinfo>
#include <byteswap.h>
#include <cstdint>

/* Global Variables*/

#define AVAILABLE_ACL_PRIORITIES_LIMITATION             (32)
#define EAPOL_ETHER_TYPE                                (0x888e)
#define PAUSE_ETHER_TYPE                                (0x8808)
#define MACSEC_STAT_XPN_POLLING_INTERVAL_MS             (1000)
#define MACSEC_STAT_POLLING_INTERVAL_MS                 (10000)
#define PFC_MODE_BYPASS                                 "bypass"
#define PFC_MODE_ENCRYPT                                "encrypt"
#define PFC_MODE_STRICT_ENCRYPT                         "strict_encrypt"
#define PFC_MODE_DEFAULT                                 PFC_MODE_BYPASS

extern sai_object_id_t   gSwitchId;
extern sai_macsec_api_t *sai_macsec_api;
extern sai_acl_api_t *sai_acl_api;
extern sai_port_api_t *sai_port_api;
extern sai_switch_api_t *sai_switch_api;

constexpr bool DEFAULT_ENABLE_ENCRYPT = true;
constexpr bool DEFAULT_SCI_IN_SECTAG = false;
constexpr sai_macsec_cipher_suite_t DEFAULT_CIPHER_SUITE = SAI_MACSEC_CIPHER_SUITE_GCM_AES_128;

static const std::vector<std::string> macsec_sa_attrs =
    {
        "SAI_MACSEC_SA_ATTR_CURRENT_XPN",
};
static const std::vector<std::string> macsec_sa_ingress_stats =
    {
        "SAI_MACSEC_SA_STAT_OCTETS_ENCRYPTED",
        "SAI_MACSEC_SA_STAT_OCTETS_PROTECTED",
        "SAI_MACSEC_SA_STAT_IN_PKTS_UNCHECKED",
        "SAI_MACSEC_SA_STAT_IN_PKTS_DELAYED",
        "SAI_MACSEC_SA_STAT_IN_PKTS_LATE",
        "SAI_MACSEC_SA_STAT_IN_PKTS_INVALID",
        "SAI_MACSEC_SA_STAT_IN_PKTS_NOT_VALID",
        "SAI_MACSEC_SA_STAT_IN_PKTS_NOT_USING_SA",
        "SAI_MACSEC_SA_STAT_IN_PKTS_UNUSED_SA",
        "SAI_MACSEC_SA_STAT_IN_PKTS_OK",
};
static const std::vector<std::string> macsec_sa_egress_stats =
    {
        "SAI_MACSEC_SA_STAT_OCTETS_ENCRYPTED",
        "SAI_MACSEC_SA_STAT_OCTETS_PROTECTED",
        "SAI_MACSEC_SA_STAT_OUT_PKTS_ENCRYPTED",
        "SAI_MACSEC_SA_STAT_OUT_PKTS_PROTECTED",
};
static const std::vector<std::string> macsec_flow_ingress_stats =
    {
        "SAI_MACSEC_FLOW_STAT_OTHER_ERR",
        "SAI_MACSEC_FLOW_STAT_OCTETS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_OCTETS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_UCAST_PKTS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_UCAST_PKTS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_MULTICAST_PKTS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_MULTICAST_PKTS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_BROADCAST_PKTS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_BROADCAST_PKTS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_CONTROL_PKTS",
        "SAI_MACSEC_FLOW_STAT_PKTS_UNTAGGED",
        "SAI_MACSEC_FLOW_STAT_IN_TAGGED_CONTROL_PKTS",
        "SAI_MACSEC_FLOW_STAT_IN_PKTS_NO_TAG",
        "SAI_MACSEC_FLOW_STAT_IN_PKTS_BAD_TAG",
        "SAI_MACSEC_FLOW_STAT_IN_PKTS_NO_SCI",
        "SAI_MACSEC_FLOW_STAT_IN_PKTS_UNKNOWN_SCI",
        "SAI_MACSEC_FLOW_STAT_IN_PKTS_OVERRUN",
};
static const std::vector<std::string> macsec_flow_egress_stats =
    {
        "SAI_MACSEC_FLOW_STAT_OTHER_ERR",
        "SAI_MACSEC_FLOW_STAT_OCTETS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_OCTETS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_OUT_OCTETS_COMMON",
        "SAI_MACSEC_FLOW_STAT_UCAST_PKTS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_UCAST_PKTS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_MULTICAST_PKTS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_MULTICAST_PKTS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_BROADCAST_PKTS_UNCONTROLLED",
        "SAI_MACSEC_FLOW_STAT_BROADCAST_PKTS_CONTROLLED",
        "SAI_MACSEC_FLOW_STAT_CONTROL_PKTS",
        "SAI_MACSEC_FLOW_STAT_PKTS_UNTAGGED",
        "SAI_MACSEC_FLOW_STAT_OUT_PKTS_TOO_LONG",
};

template <typename T, typename... Args>
static bool extract_variables(const std::string &input, char delimiter, T &output, Args &... args)
{
    const auto tokens = swss::tokenize(input, delimiter);
    try
    {
        swss::lexical_convert(tokens, output, args...);
        return true;
    }
    catch(const std::exception& e)
    {
        return false;
    }
}

template<class T>
static bool get_value(
    const MACsecOrch::TaskArgs & ta,
    const std::string & field,
    T & value)
{
    SWSS_LOG_ENTER();

    auto value_opt = swss::fvsGetValue(ta, field, true);
    if (!value_opt)
    {
        SWSS_LOG_DEBUG("Cannot find field : %s", field.c_str());
        return false;
    }

    try
    {
        lexical_convert(*value_opt, value);
        return true;
    }
    catch(const std::exception &err)
    {
        SWSS_LOG_DEBUG("Cannot convert field : %s to type : %s", field.c_str(), typeid(T).name());
        return false;
    }
}

struct MACsecSAK
{
    sai_macsec_sak_t    m_sak;
    bool                m_sak_256_enable;
};

static void lexical_convert(const std::string &buffer, MACsecSAK &sak)
{
    SWSS_LOG_ENTER();

    bool convert_done = false;
    memset(&sak, 0, sizeof(sak));
    // One hex indicates 4 bits 
    size_t bit_count = buffer.length() * 4;
    // 128 bits SAK
    if (bit_count == 128)
    {
        // 128-bit SAK uses only Bytes 16..31.
        sak.m_sak_256_enable = false;
        convert_done = swss::hex_to_binary(
            buffer,
            &sak.m_sak[16],
            16);
    }
    // 256 bits SAK
    else if (bit_count == 256)
    {
        sak.m_sak_256_enable = true;
        convert_done = swss::hex_to_binary(
            buffer,
            sak.m_sak,
            32);
    }

    if (!convert_done)
    {
        SWSS_LOG_THROW("Invalid SAK %s", buffer.c_str());
    }
}

struct MACsecSalt
{
    sai_macsec_salt_t m_salt;
};

static void lexical_convert(const std::string &buffer, MACsecSalt &salt)
{
    SWSS_LOG_ENTER();

    memset(&salt, 0, sizeof(salt));
    if (
        (buffer.length() != sizeof(salt.m_salt) * 2) || (!swss::hex_to_binary(buffer, salt.m_salt, sizeof(salt.m_salt))))
    {
        SWSS_LOG_THROW("Invalid SALT %s", buffer.c_str());
    }
}

struct MACsecAuthKey
{
    sai_macsec_auth_key_t m_auth_key;
};

static void lexical_convert(const std::string &buffer, MACsecAuthKey &auth_key)
{
    SWSS_LOG_ENTER();

    memset(&auth_key, 0, sizeof(auth_key));
    if (
        (buffer.length() != sizeof(auth_key.m_auth_key) * 2) || (!swss::hex_to_binary(
                                                                    buffer,
                                                                    auth_key.m_auth_key,
                                                                    sizeof(auth_key.m_auth_key))))
    {
        SWSS_LOG_THROW("Invalid Auth Key %s", buffer.c_str());
    }
}

class MACsecSCI
{
public:
    operator sai_uint64_t () const
    {
        SWSS_LOG_ENTER();

        return m_sci;
    }

    std::string str() const
    {
        SWSS_LOG_ENTER();

        return boost::algorithm::to_lower_copy(swss::binary_to_hex(&m_sci, sizeof(m_sci)));
    }

    MACsecSCI& operator= (const std::string &buffer)
    {
        SWSS_LOG_ENTER();

        if (!swss::hex_to_binary(buffer, reinterpret_cast<std::uint8_t *>(&m_sci), sizeof(m_sci)))
        {
            SWSS_LOG_THROW("Invalid SCI %s", buffer.c_str());
        }

        return *this;
    }

    MACsecSCI() = default;

    MACsecSCI(const sai_uint64_t sci)
    {
        SWSS_LOG_ENTER();

        this->m_sci = sci;
    }

private:
    sai_uint64_t m_sci;
};

namespace swss {

template<>
inline void lexical_convert(const std::string &buffer, MACsecSCI &sci)
{
    SWSS_LOG_ENTER();

    sci = buffer;
}

}

std::ostream& operator<<(std::ostream& stream, const MACsecSCI& sci)
{
    SWSS_LOG_ENTER();

    stream << sci.str();
    return stream;
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

/* Get MACsecOrch Context from port_name, sci or an */

class MACsecOrchContext
{
public:
    MACsecOrchContext(
        MACsecOrch *orch,
        const std::string &port_name) : MACsecOrchContext(orch)
    {
        m_port_name.reset(new std::string(port_name));
    }
    MACsecOrchContext(
        MACsecOrch *orch,
        const std::string &port_name,
        sai_macsec_direction_t direction,
        sai_uint64_t sci) : MACsecOrchContext(orch, port_name)
    {
        m_direction = direction;
        m_sci.reset(new sai_uint64_t(sci));
    }
    MACsecOrchContext(
        MACsecOrch *orch,
        const std::string &port_name,
        sai_macsec_direction_t direction,
        sai_uint64_t sci,
        macsec_an_t an) : MACsecOrchContext(orch, port_name, direction, sci)
    {
        m_an.reset(new macsec_an_t(an));
    }

    sai_object_id_t *get_port_id()
    {
        if (m_port_id == nullptr)
        {
            auto port = get_port();
            if (port == nullptr)
            {
                return nullptr;
            }
            if (port->m_line_side_id != SAI_NULL_OBJECT_ID)
            {
                m_port_id = std::make_unique<sai_object_id_t>(port->m_line_side_id);
            }
            else
            {
                m_port_id = std::make_unique<sai_object_id_t>(port->m_port_id);
            }
        }
        return m_port_id.get();
    }

    sai_object_id_t *get_switch_id()
    {
        if (m_switch_id == nullptr)
        {
            auto port = get_port();
            sai_object_id_t switchId;
            if (port == nullptr || port->m_switch_id == SAI_NULL_OBJECT_ID)
            {
                switchId = gSwitchId;
            }
            else
            {
                switchId = port->m_switch_id;
            }
            if (switchId == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("Switch ID cannot be found");
                return nullptr;
            }
            m_switch_id = std::make_unique<sai_object_id_t>(switchId);
        }
        return m_switch_id.get();
    }

    MACsecOrch::MACsecObject *get_macsec_obj()
    {
        if (m_macsec_obj == nullptr)
        {
            auto switch_id = get_switch_id();
            if (switch_id == nullptr)
            {
                return nullptr;
            }
            auto macsec_port = m_orch->m_macsec_objs.find(*switch_id);
            if (macsec_port == m_orch->m_macsec_objs.end())
            {
                SWSS_LOG_INFO("Cannot find the MACsec Object at the port %s.", m_port_name->c_str());
                return nullptr;
            }
            m_macsec_obj = &macsec_port->second;
        }
        return m_macsec_obj;
    }

    MACsecOrch::MACsecPort *get_macsec_port()
    {
        if (m_macsec_port == nullptr)
        {
            if (m_orch == nullptr)
            {
                SWSS_LOG_ERROR("MACsec orch wasn't provided");
                return nullptr;
            }
            if (m_port_name == nullptr || m_port_name->empty())
            {
                SWSS_LOG_ERROR("Port name wasn't provided.");
                return nullptr;
            }
            auto macsec_port = m_orch->m_macsec_ports.find(*m_port_name);
            if (macsec_port == m_orch->m_macsec_ports.end())
            {
                SWSS_LOG_INFO("Cannot find the MACsec Object at the port %s.", m_port_name->c_str());
                return nullptr;
            }
            m_macsec_port = macsec_port->second.get();
        }
        return m_macsec_port;
    }

    MACsecOrch::MACsecACLTable *get_acl_table()
    {
        if (m_acl_table == nullptr)
        {
            auto port = get_macsec_port();
            if (port == nullptr)
            {
                return nullptr;
            }
            m_acl_table =
                (m_direction == SAI_MACSEC_DIRECTION_EGRESS)
                    ? &port->m_egress_acl_table
                    : &port->m_ingress_acl_table;
        }
        return m_acl_table;
    }

    MACsecOrch::MACsecSC *get_macsec_sc()
    {
        if (m_macsec_sc == nullptr)
        {
            if (m_sci == nullptr)
            {
                SWSS_LOG_ERROR("SCI wasn't provided");
                return nullptr;
            }
            auto port = get_macsec_port();
            if (port == nullptr)
            {
                return nullptr;
            }
            auto &scs =
                (m_direction == SAI_MACSEC_DIRECTION_EGRESS)
                    ? port->m_egress_scs
                    : port->m_ingress_scs;
            auto sc = scs.find(*m_sci);
            if (sc == scs.end())
            {
                SWSS_LOG_INFO("Cannot find the MACsec SC 0x%" PRIx64 " at the port %s.", *m_sci, m_port_name->c_str());
                return nullptr;
            }
            m_macsec_sc = &sc->second;
        }
        return m_macsec_sc;
    }

    sai_object_id_t *get_macsec_sa()
    {
        if (m_macsec_sa == nullptr)
        {
            if (m_an == nullptr)
            {
                SWSS_LOG_ERROR("AN wasn't provided");
                return nullptr;
            }
            auto sc = get_macsec_sc();
            if (sc == nullptr)
            {
                return nullptr;
            }
            auto an = sc->m_sa_ids.find(*m_an);
            if (an == sc->m_sa_ids.end())
            {
                SWSS_LOG_INFO(
                    "Cannot find the MACsec SA %u of SC 0x%" PRIx64 " at the port %s.",
                    *m_an,
                    *m_sci,
                    m_port_name->c_str());
                return nullptr;
            }
            m_macsec_sa = &an->second;
        }
        return m_macsec_sa;
    }

    const gearbox_phy_t* get_gearbox_phy()
    {
        if (m_gearbox_phy)
        {
            return m_gearbox_phy;
        }
        auto switch_id = get_switch_id();
        if (switch_id == nullptr || get_port() == nullptr)
        {
            SWSS_LOG_ERROR("Switch/Port wasn't provided");
            return nullptr;
        }
        if (*switch_id == gSwitchId)
        {
            return nullptr;
        }
        m_gearbox_phy = m_orch->m_port_orch->getGearboxPhy(*get_port());
        return m_gearbox_phy;
    }

    Port *get_port()
    {
        if (m_port == nullptr)
        {
            if (m_orch == nullptr)
            {
                SWSS_LOG_ERROR("MACsec orch wasn't provided");
                return nullptr;
            }
            if (m_port_name == nullptr || m_port_name->empty())
            {
                SWSS_LOG_ERROR("Port name wasn't provided.");
                return nullptr;
            }
            auto port = std::make_unique<Port>();
            if (!m_orch->m_port_orch->getPort(*m_port_name, *port))
            {
                SWSS_LOG_INFO("Cannot find the port %s.", m_port_name->c_str());
                return nullptr;
            }
            m_port = std::move(port);
        }
        return m_port.get();
    }

private:
    MACsecOrchContext(MACsecOrch *orch) : m_orch(orch),
                                          m_port_name(nullptr),
                                          m_direction(SAI_MACSEC_DIRECTION_EGRESS),
                                          m_sci(nullptr),
                                          m_an(nullptr),
                                          m_port(nullptr),
                                          m_macsec_obj(nullptr),
                                          m_port_id(nullptr),
                                          m_switch_id(nullptr),
                                          m_macsec_port(nullptr),
                                          m_acl_table(nullptr),
                                          m_macsec_sc(nullptr),
                                          m_macsec_sa(nullptr),
                                          m_gearbox_phy(nullptr)
    {
    }

    MACsecOrch                          *m_orch;
    std::shared_ptr<std::string>        m_port_name;
    sai_macsec_direction_t              m_direction;
    std::unique_ptr<sai_uint64_t>       m_sci;
    std::unique_ptr<macsec_an_t>        m_an;

    std::unique_ptr<Port>               m_port;
    MACsecOrch::MACsecObject            *m_macsec_obj;
    std::unique_ptr<sai_object_id_t>    m_port_id;
    std::unique_ptr<sai_object_id_t>    m_switch_id;

    MACsecOrch::MACsecPort              *m_macsec_port;
    MACsecOrch::MACsecACLTable          *m_acl_table;

    MACsecOrch::MACsecSC                *m_macsec_sc;
    sai_object_id_t                     *m_macsec_sa;
    const gearbox_phy_t                 *m_gearbox_phy;
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
                            m_applPortTable(app_db, APP_PORT_TABLE_NAME),
                            m_counter_db("COUNTERS_DB", 0),
                            m_macsec_counters_map(&m_counter_db, COUNTERS_MACSEC_NAME_MAP),
                            m_gb_counter_db("GB_COUNTERS_DB", 0),
                            m_gb_macsec_counters_map(&m_gb_counter_db, COUNTERS_MACSEC_NAME_MAP),
                            m_macsec_sa_attr_manager(
                                COUNTERS_MACSEC_SA_ATTR_GROUP,
                                StatsMode::READ,
                                MACSEC_STAT_XPN_POLLING_INTERVAL_MS, true),
                            m_macsec_sa_stat_manager(
                                COUNTERS_MACSEC_SA_GROUP,
                                StatsMode::READ,
                                MACSEC_STAT_POLLING_INTERVAL_MS, true),
                            m_macsec_flow_stat_manager(
                                COUNTERS_MACSEC_FLOW_GROUP,
                                StatsMode::READ,
                                MACSEC_STAT_POLLING_INTERVAL_MS, true),
                            m_gb_macsec_sa_attr_manager(
                                "GB_FLEX_COUNTER_DB",
                                COUNTERS_MACSEC_SA_ATTR_GROUP,
                                StatsMode::READ,
                                MACSEC_STAT_XPN_POLLING_INTERVAL_MS, true),
                            m_gb_macsec_sa_stat_manager(
                                "GB_FLEX_COUNTER_DB",
                                COUNTERS_MACSEC_SA_GROUP,
                                StatsMode::READ,
                                MACSEC_STAT_POLLING_INTERVAL_MS, true),
                            m_gb_macsec_flow_stat_manager(
                                "GB_FLEX_COUNTER_DB",
                                COUNTERS_MACSEC_FLOW_GROUP,
                                StatsMode::READ,
                                MACSEC_STAT_POLLING_INTERVAL_MS, true)
{
    SWSS_LOG_ENTER();
}

MACsecOrch::~MACsecOrch()
{
    while (!m_macsec_ports.empty())
    {
        auto port = m_macsec_ports.begin();
        const MACsecOrch::TaskArgs temp;
        taskDisableMACsecPort(port->first, temp);
    }
}

void MACsecOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    using TaskType = std::tuple<const std::string, const std::string>;
    using TaskFunc = task_process_status (MACsecOrch::*)(
        const std::string &,
        const TaskArgs &);
    const static std::map<TaskType, TaskFunc> TaskMap = {
        {{APP_MACSEC_PORT_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::taskUpdateMACsecPort},
        {{APP_MACSEC_PORT_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::taskDisableMACsecPort},
        {{APP_MACSEC_EGRESS_SC_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::taskUpdateEgressSC},
        {{APP_MACSEC_EGRESS_SC_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::taskDeleteEgressSC},
        {{APP_MACSEC_INGRESS_SC_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::taskUpdateIngressSC},
        {{APP_MACSEC_INGRESS_SC_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::taskDeleteIngressSC},
        {{APP_MACSEC_EGRESS_SA_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::taskUpdateEgressSA},
        {{APP_MACSEC_EGRESS_SA_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::taskDeleteEgressSA},
        {{APP_MACSEC_INGRESS_SA_TABLE_NAME, SET_COMMAND},
         &MACsecOrch::taskUpdateIngressSA},
        {{APP_MACSEC_INGRESS_SA_TABLE_NAME, DEL_COMMAND},
         &MACsecOrch::taskDeleteIngressSA},
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

task_process_status MACsecOrch::taskUpdateMACsecPort(
    const std::string &port_name,
    const TaskArgs &port_attr)
{
    SWSS_LOG_ENTER();

    MACsecOrchContext ctx(this, port_name);
    RecoverStack recover;

    if (ctx.get_port_id() == nullptr || ctx.get_switch_id() == nullptr)
    {
        return task_need_retry;
    }
    if (ctx.get_macsec_obj() == nullptr)
    {
        if (!initMACsecObject(*ctx.get_switch_id()))
        {
            SWSS_LOG_WARN("Cannot init MACsec Object at the port %s.", port_name.c_str());
            return task_failed;
        }
        auto switch_id = *ctx.get_switch_id();
        recover.add_action([this, switch_id]() { this->deinitMACsecObject(switch_id); });
    }
    if (ctx.get_macsec_port() == nullptr)
    {
        auto macsec_port_itr = m_macsec_ports.emplace(port_name, std::make_shared<MACsecPort>()).first;
        recover.add_action([this, macsec_port_itr]() { this->m_macsec_ports.erase(macsec_port_itr); });

        if (!createMACsecPort(
                *macsec_port_itr->second,
                port_name,
                port_attr,
                *ctx.get_macsec_obj(),
                *ctx.get_port_id(),
                *ctx.get_switch_id(),
                *ctx.get_port(),
                ctx.get_gearbox_phy()))
        {
            return task_failed;
        }
        ctx.get_macsec_obj()->m_macsec_ports[port_name] = macsec_port_itr->second;
        recover.add_action([this, &port_name, &ctx, macsec_port_itr]() {
            this->deleteMACsecPort(
                *macsec_port_itr->second,
                port_name,
                *ctx.get_macsec_obj(),
                *ctx.get_port_id(),
                *ctx.get_port(),
                ctx.get_gearbox_phy());
        });
    }
    if (!updateMACsecPort(*ctx.get_macsec_port(), port_attr))
    {
        return task_failed;
    }

    recover.clear();
    return task_success;
}

task_process_status MACsecOrch::taskDisableMACsecPort(
    const std::string &port_name,
    const TaskArgs &port_attr)
{
    SWSS_LOG_ENTER();

    MACsecOrchContext ctx(this, port_name);

    if (ctx.get_switch_id() == nullptr || ctx.get_macsec_port() == nullptr)
    {
        SWSS_LOG_WARN("Cannot find MACsec switch at the port %s.", port_name.c_str());
        return task_failed;
    }
    if (ctx.get_port_id() == nullptr)
    {
        SWSS_LOG_WARN("Cannot find the port %s.", port_name.c_str());
        return task_failed;
    }

    if (ctx.get_macsec_port() == nullptr)
    {
        SWSS_LOG_INFO("The MACsec wasn't enabled at the port %s", port_name.c_str());
        return task_success;
    }

    auto result = task_success;
    if (!deleteMACsecPort(
            *ctx.get_macsec_port(),
            port_name,
            *ctx.get_macsec_obj(),
            *ctx.get_port_id(),
            *ctx.get_port(),
            ctx.get_gearbox_phy()))
    {
        result = task_failed;
    }
    m_macsec_ports.erase(port_name);
    ctx.get_macsec_obj()->m_macsec_ports.erase(port_name);
    // All ports on this macsec object have been deleted.
    if (ctx.get_macsec_obj()->m_macsec_ports.empty())
    {
        if (!deinitMACsecObject(*ctx.get_switch_id()))
        {
            SWSS_LOG_WARN("Cannot deinit macsec at the port %s.", port_name.c_str());
            result = task_failed;
        }
    }

    return result;
}

task_process_status MACsecOrch::taskUpdateEgressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return updateMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::taskDeleteEgressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::taskUpdateIngressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return updateMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_INGRESS);
}

task_process_status MACsecOrch::taskDeleteIngressSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_INGRESS);
}

task_process_status MACsecOrch::taskUpdateEgressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    std::string port_name;
    MACsecSCI sci;
    macsec_an_t an = 0;
    if (!extract_variables(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    MACsecOrchContext ctx(this, port_name, SAI_MACSEC_DIRECTION_EGRESS, sci, an);
    if (ctx.get_macsec_sc() == nullptr)
    {
        SWSS_LOG_INFO("The MACsec SC %s hasn't been created at the port %s.", sci.str().c_str(), port_name.c_str());
        return task_need_retry;
    }
    if (ctx.get_macsec_sc()->m_encoding_an == an)
    {
        if (ctx.get_macsec_sa() == nullptr)
        {
            // The MACsec SA hasn't been created
            return createMACsecSA(port_sci_an, sa_attr, SAI_MACSEC_DIRECTION_EGRESS);
        }
        else
        {
            // The MACsec SA has enabled, update SA's attributes
            sai_uint64_t pn;

            if (get_value(sa_attr, "next_pn", pn))
            {
                sai_attribute_t attr;
                attr.id = SAI_MACSEC_SA_ATTR_CONFIGURED_EGRESS_XPN;
                attr.value.u64 = pn;
                if (!this->updateMACsecAttr(SAI_OBJECT_TYPE_MACSEC_SA, *(ctx.get_macsec_sa()), attr))
                {
                    SWSS_LOG_WARN("Fail to update next pn (%" PRIu64 ") of egress MACsec SA %s", pn, port_sci_an.c_str());
                    return task_failed;
                }
            }

            return task_success;
        }
    }
    return task_need_retry;
}

task_process_status MACsecOrch::taskDeleteEgressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSA(port_sci_an, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::taskUpdateIngressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();

    swss::AlphaBoolean alpha_boolean = false;
    bool has_active_field = get_value(sa_attr, "active", alpha_boolean);
    bool active = alpha_boolean.operator bool();
    if (active)
    {
        return createMACsecSA(port_sci_an, sa_attr, SAI_MACSEC_DIRECTION_INGRESS);
    }
    else
    {

        std::string port_name;
        MACsecSCI sci;
        macsec_an_t an = 0;
        if (!extract_variables(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
        {
            SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
            return task_failed;
        }

        MACsecOrchContext ctx(this, port_name, SAI_MACSEC_DIRECTION_INGRESS, sci, an);

        if (ctx.get_macsec_sa() != nullptr)
        {
            if (has_active_field)
            {
                // Delete MACsec SA explicitly by set active to false
                return deleteMACsecSA(port_sci_an, SAI_MACSEC_DIRECTION_INGRESS);
            }
            else
            {
                sai_uint64_t pn;

                if (get_value(sa_attr, "lowest_acceptable_pn", pn))
                {
                    sai_attribute_t attr;
                    attr.id = SAI_MACSEC_SA_ATTR_MINIMUM_INGRESS_XPN;
                    attr.value.u64 = pn;
                    if (!this->updateMACsecAttr(SAI_OBJECT_TYPE_MACSEC_SA, *(ctx.get_macsec_sa()), attr))
                    {
                        SWSS_LOG_WARN("Fail to update lowest acceptable PN (%" PRIu64 ") of ingress MACsec SA %s", pn, port_sci_an.c_str());
                        return task_failed;
                    }
                }

                return task_success;
            }
        }
        else
        {
            // The MACsec SA hasn't been created.
            // Don't need do anything until the active field to be enabled.
            // To use "retry" because this message maybe include other initialized fields
            // To use other return values will make us lose these initialized fields.
            return task_need_retry;
        }
    }

    return task_success;
}

task_process_status MACsecOrch::taskDeleteIngressSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSA(port_sci_an, SAI_MACSEC_DIRECTION_INGRESS);
}

bool MACsecOrch::initMACsecObject(sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    auto macsec_obj = m_macsec_objs.emplace(switch_id, MACsecObject());
    if (!macsec_obj.second)
    {
        SWSS_LOG_INFO("The MACsec has been initialized at the switch 0x%" PRIx64, switch_id);
        return true;
    }
    recover.add_action([&]() { m_macsec_objs.erase(macsec_obj.first); });

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_ATTR_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_EGRESS;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_ATTR_PHYSICAL_BYPASS_ENABLE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    sai_status_t status = sai_macsec_api->create_macsec(
                                &macsec_obj.first->second.m_egress_id,
                                switch_id,
                                static_cast<uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot initialize MACsec egress object at the switch 0x%" PRIx64, switch_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    recover.add_action([&]() { sai_macsec_api->remove_macsec(macsec_obj.first->second.m_egress_id); });

    attrs.clear();
    attr.id = SAI_MACSEC_ATTR_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_INGRESS;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_ATTR_PHYSICAL_BYPASS_ENABLE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    status = sai_macsec_api->create_macsec(
                                &macsec_obj.first->second.m_ingress_id,
                                switch_id,
                                static_cast<uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot initialize MACsec ingress object at the switch 0x%" PRIx64, switch_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    recover.add_action([&]() { sai_macsec_api->remove_macsec(macsec_obj.first->second.m_ingress_id); });

    attrs.clear();
    attr.id = SAI_MACSEC_ATTR_SCI_IN_INGRESS_MACSEC_ACL;
    attrs.push_back(attr);
    status = sai_macsec_api->get_macsec_attribute(
                    macsec_obj.first->second.m_ingress_id,
                    static_cast<uint32_t>(attrs.size()),
                    attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN(
            "Cannot get MACsec attribution SAI_MACSEC_ATTR_SCI_IN_INGRESS_MACSEC_ACL at the switch 0x%" PRIx64,
            switch_id);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_MACSEC, status);
        if (handle_status != task_process_status::task_success)
        {
            return false;
        }
    }
    macsec_obj.first->second.m_sci_in_ingress_macsec_acl = attrs.front().value.booldata;

    attrs.clear();
    attr.id = SAI_MACSEC_ATTR_MAX_SECURE_ASSOCIATIONS_PER_SC;
    attrs.push_back(attr);
    status = sai_macsec_api->get_macsec_attribute(
                    macsec_obj.first->second.m_ingress_id,
                    static_cast<uint32_t>(attrs.size()),
                    attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        // Default to 4 if SAI_MACSEC_ATTR_MAX_SECURE_ASSOCIATION_PER_SC isn't supported
        macsec_obj.first->second.m_max_sa_per_sc = 4;
    } else {
        switch (attrs.front().value.s32)
        {
            case SAI_MACSEC_MAX_SECURE_ASSOCIATIONS_PER_SC_TWO:
                macsec_obj.first->second.m_max_sa_per_sc = 2;
                break;
            case SAI_MACSEC_MAX_SECURE_ASSOCIATIONS_PER_SC_FOUR:
                macsec_obj.first->second.m_max_sa_per_sc = 4;
                break;
            default:
                SWSS_LOG_WARN( "Unsupported value returned from SAI_MACSEC_ATTR_MAX_SECURE_ASSOCIATION_PER_SC" );
                return false;
        }
    }

    recover.clear();
    return true;
}

bool MACsecOrch::deinitMACsecObject(sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    auto macsec_obj = m_macsec_objs.find(switch_id);
    if (macsec_obj == m_macsec_objs.end())
    {
        SWSS_LOG_INFO("The MACsec wasn't initialized at the switch 0x%" PRIx64, switch_id);
        return true;
    }

    bool result = true;

    sai_status_t status = sai_macsec_api->remove_macsec(macsec_obj->second.m_egress_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot deinitialize MACsec egress object at the switch 0x%" PRIx64, macsec_obj->first);
        result &= (handleSaiRemoveStatus(SAI_API_MACSEC, status) == task_success);
    }

    status = sai_macsec_api->remove_macsec(macsec_obj->second.m_ingress_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot deinitialize MACsec ingress object at the switch 0x%" PRIx64, macsec_obj->first);
        result &= (handleSaiRemoveStatus(SAI_API_MACSEC, status) == task_success);
    }

    m_macsec_objs.erase(macsec_obj);
    return result;
}

bool MACsecOrch::createMACsecPort(
    MACsecPort &macsec_port,
    const std::string &port_name,
    const TaskArgs &port_attr,
    const MACsecObject &macsec_obj,
    sai_object_id_t port_id,
    sai_object_id_t switch_id,
    Port &port,
    const gearbox_phy_t* phy)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    if (!createMACsecPort(
            macsec_port.m_egress_port_id,
            port_id,
            switch_id,
            SAI_MACSEC_DIRECTION_EGRESS))
    {
        SWSS_LOG_WARN("Cannot create MACsec egress port at the port %s", port_name.c_str());
        return false;
    }
    recover.add_action([this, &macsec_port]() {
        this->deleteMACsecPort(macsec_port.m_egress_port_id);
        macsec_port.m_egress_port_id = SAI_NULL_OBJECT_ID;
    });

    if (!createMACsecPort(
            macsec_port.m_ingress_port_id,
            port_id,
            switch_id,
            SAI_MACSEC_DIRECTION_INGRESS))
    {
        SWSS_LOG_WARN("Cannot create MACsec ingress port at the port %s", port_name.c_str());
        return false;
    }
    recover.add_action([this, &macsec_port]() {
        this->deleteMACsecPort(macsec_port.m_ingress_port_id);
        macsec_port.m_ingress_port_id = SAI_NULL_OBJECT_ID;
    });

    macsec_port.m_enable_encrypt = DEFAULT_ENABLE_ENCRYPT;
    macsec_port.m_sci_in_sectag = DEFAULT_SCI_IN_SECTAG;
    macsec_port.m_cipher_suite = DEFAULT_CIPHER_SUITE;
    macsec_port.m_enable = false;

    // If hardware matches SCI in ACL, the macsec_flow maps to an IEEE 802.1ae SecY object.
    // Multiple SCs can be associated with such a macsec_flow.
    // Then a specific value of SCI from the SecTAG in the packet is used to identify a specific SC
    // for that macsec_flow.
    // False means one flow can be associated with multiple ACL entries and multiple SC
    if (!macsec_obj.m_sci_in_ingress_macsec_acl)
    {
        if (!createMACsecFlow(
                macsec_port.m_egress_flow_id,
                switch_id,
                SAI_MACSEC_DIRECTION_EGRESS))
        {
            SWSS_LOG_WARN("Cannot create MACsec egress flow at the port %s.", port_name.c_str());
            return false;
        }
        recover.add_action([this, &macsec_port]() {
            this->deleteMACsecFlow(macsec_port.m_egress_flow_id);
            macsec_port.m_egress_flow_id = SAI_NULL_OBJECT_ID;
        });

        if (!createMACsecFlow(
                macsec_port.m_ingress_flow_id,
                switch_id,
                SAI_MACSEC_DIRECTION_INGRESS))
        {
            SWSS_LOG_WARN("Cannot create MACsec ingress flow at the port %s.", port_name.c_str());
            return false;
        }
        recover.add_action([this, &macsec_port]() {
            this->deleteMACsecFlow(macsec_port.m_ingress_flow_id);
            macsec_port.m_ingress_flow_id = SAI_NULL_OBJECT_ID;
        });
    }

    if (!initMACsecACLTable(
            macsec_port.m_egress_acl_table,
            port_id,
            switch_id,
            SAI_MACSEC_DIRECTION_EGRESS,
            macsec_port.m_sci_in_sectag,
            port_name,
            phy))
    {
        SWSS_LOG_WARN("Cannot init the ACL Table at the port %s.", port_name.c_str());
        return false;
    }
    recover.add_action([this, &macsec_port, port_id, phy]() {
        this->deinitMACsecACLTable(
            macsec_port.m_egress_acl_table,
            port_id,
            SAI_MACSEC_DIRECTION_EGRESS,
            phy);
    });

    if (!initMACsecACLTable(
            macsec_port.m_ingress_acl_table,
            port_id,
            switch_id,
            SAI_MACSEC_DIRECTION_INGRESS,
            macsec_port.m_sci_in_sectag,
            port_name,
            phy))
    {
        SWSS_LOG_WARN("Cannot init the ACL Table at the port %s.", port_name.c_str());
        return false;
    }
    recover.add_action([this, &macsec_port, port_id, phy]() {
        this->deinitMACsecACLTable(
            macsec_port.m_ingress_acl_table,
            port_id,
            SAI_MACSEC_DIRECTION_INGRESS,
            phy);
    });

    if (phy)
    {
        if (!setPFCForward(port_id, true))
        {
            SWSS_LOG_WARN("Cannot enable PFC forward at the port %s.", port_name.c_str());
            return false;
        }
        recover.add_action([this, port_id]()
                           { this->setPFCForward(port_id, false); });

        if (phy->macsec_ipg != 0)
        {
            if (!m_port_orch->getPortIPG(port.m_port_id, macsec_port.m_original_ipg))
            {
                SWSS_LOG_WARN("Cannot get Port IPG at the port %s", port_name.c_str());
                return false;
            }
            if (!m_port_orch->setPortIPG(port.m_port_id, phy->macsec_ipg))
            {
                SWSS_LOG_WARN("Cannot set MACsec IPG to %u at the port %s", phy->macsec_ipg, port_name.c_str());
                return false;
            }
        }
    }

    SWSS_LOG_NOTICE("MACsec port %s is created.", port_name.c_str());

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("max_sa_per_sc", std::to_string(macsec_obj.m_max_sa_per_sc));
    fvVector.emplace_back("state", "ok");
    m_state_macsec_port.set(port_name, fvVector);

    recover.clear();
    return true;
}

bool MACsecOrch::createMACsecPort(
    sai_object_id_t &macsec_port_id,
    sai_object_id_t port_id,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_PORT_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = direction;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_PORT_ATTR_PORT_ID;
    attr.value.oid = port_id;
    attrs.push_back(attr);
    sai_status_t status = sai_macsec_api->create_macsec_port(
                                &macsec_port_id,
                                switch_id,
                                static_cast<uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool MACsecOrch::updateMACsecPort(MACsecPort &macsec_port, const TaskArgs &port_attr)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    swss::AlphaBoolean alpha_boolean;

    if (get_value(port_attr, "enable_encrypt", alpha_boolean))
    {
        macsec_port.m_enable_encrypt = alpha_boolean.operator bool();
        if (!updateMACsecSCs(
                macsec_port,
                [&macsec_port, this](MACsecOrch::MACsecSC &macsec_sc)
                {
                    sai_attribute_t attr;
                    attr.id = SAI_MACSEC_SC_ATTR_ENCRYPTION_ENABLE;
                    attr.value.booldata = macsec_port.m_enable_encrypt;
                    return this->updateMACsecAttr(SAI_OBJECT_TYPE_MACSEC_SC, macsec_sc.m_sc_id, attr);
                }))
        {
            return false;
        }
    }
    if (get_value(port_attr, "send_sci", alpha_boolean))
    {
        macsec_port.m_sci_in_sectag = alpha_boolean.operator bool();
    }
    std::string cipher_suite;
    if (get_value(port_attr, "cipher_suite", cipher_suite))
    {
        if (cipher_suite == "GCM-AES-128")
        {
            macsec_port.m_cipher_suite = SAI_MACSEC_CIPHER_SUITE_GCM_AES_128;
        }
        else if (cipher_suite == "GCM-AES-256")
        {
            macsec_port.m_cipher_suite = SAI_MACSEC_CIPHER_SUITE_GCM_AES_256;
        }
        else if (cipher_suite == "GCM-AES-XPN-128")
        {
            macsec_port.m_cipher_suite = SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_128;
        }
        else if (cipher_suite == "GCM-AES-XPN-256")
        {
            macsec_port.m_cipher_suite = SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_256;
        }
        else
        {
            SWSS_LOG_WARN("Unknown Cipher Suite %s", cipher_suite.c_str());
            return false;
        }
        if (!updateMACsecSCs(
                macsec_port,
                [&macsec_port, this](MACsecOrch::MACsecSC &macsec_sc)
                {
                    sai_attribute_t attr;
                    attr.id = SAI_MACSEC_SC_ATTR_MACSEC_CIPHER_SUITE;
                    attr.value.s32 = macsec_port.m_cipher_suite;
                    return this->updateMACsecAttr(SAI_OBJECT_TYPE_MACSEC_SC, macsec_sc.m_sc_id, attr);
                }))
        {
            return false;
        }
    }
    swss::AlphaBoolean enable = false;
    if (get_value(port_attr, "enable", enable) && enable.operator bool() != macsec_port.m_enable)
    {
        macsec_port.m_enable = enable.operator bool();
        if (!updateMACsecSCs(
                macsec_port,
                [&macsec_port, &recover, this](MACsecOrch::MACsecSC &macsec_sc)
                {
                    // Change the ACL entry action from packet action to MACsec flow
                    if (macsec_port.m_enable)
                    {
                        if (!this->setMACsecFlowActive(macsec_sc.m_entry_id, macsec_sc.m_flow_id, true))
                        {
                            SWSS_LOG_WARN("Cannot change the ACL entry action from packet action to MACsec flow");
                            return false;
                        }
                        auto entry_id = macsec_sc.m_entry_id;
                        auto flow_id = macsec_sc.m_flow_id;
                        recover.add_action([this, entry_id, flow_id]()
                                           { this->setMACsecFlowActive(entry_id, flow_id, false); });
                    }
                    else
                    {
                        this->setMACsecFlowActive(macsec_sc.m_entry_id, macsec_sc.m_flow_id, false);
                    }
                    return true;
                }))
        {
            return false;
        }
    }

    recover.clear();
    return true;
}

bool MACsecOrch::updateMACsecSCs(MACsecPort &macsec_port, std::function<bool(MACsecOrch::MACsecSC &)> action)
{
    SWSS_LOG_ENTER();

    auto sc = macsec_port.m_egress_scs.begin();
    while (sc != macsec_port.m_egress_scs.end())
    {
        if (!action((sc++)->second))
        {
            return false;
        }
    }
    sc = macsec_port.m_ingress_scs.begin();
    while (sc != macsec_port.m_ingress_scs.end())
    {
        if (!action((sc++)->second))
        {
            return false;
        }
    }

    return true;
}

bool MACsecOrch::deleteMACsecPort(
    const MACsecPort &macsec_port,
    const std::string &port_name,
    const MACsecObject &macsec_obj,
    sai_object_id_t port_id,
    Port &port,
    const gearbox_phy_t* phy)
{
    SWSS_LOG_ENTER();

    bool result = true;

    auto sc = macsec_port.m_egress_scs.begin();
    while (sc != macsec_port.m_egress_scs.end())
    {
        const std::string port_sci = swss::join(':', port_name, MACsecSCI(sc->first));
        sc ++;
        if (deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_EGRESS) != task_success)
        {
            result &= false;
        }
    }
    sc = macsec_port.m_ingress_scs.begin();
    while (sc != macsec_port.m_ingress_scs.end())
    {
        const std::string port_sci = swss::join(':', port_name, MACsecSCI(sc->first));
        sc ++;
        if (deleteMACsecSC(port_sci, SAI_MACSEC_DIRECTION_INGRESS) != task_success)
        {
            result &= false;
        }
    }

    if (!macsec_obj.m_sci_in_ingress_macsec_acl)
    {
        if (!this->deleteMACsecFlow(macsec_port.m_egress_flow_id))
        {
            SWSS_LOG_WARN("Cannot delete MACsec egress flow at the port %s", port_name.c_str());
            result &= false;
        }

        if (!this->deleteMACsecFlow(macsec_port.m_ingress_flow_id))
        {
            SWSS_LOG_WARN("Cannot delete MACsec ingress flow at the port %s", port_name.c_str());
            result &= false;
        }
    }

    if (!deinitMACsecACLTable(macsec_port.m_ingress_acl_table, port_id, SAI_MACSEC_DIRECTION_INGRESS, phy))
    {
        SWSS_LOG_WARN("Cannot deinit ingress ACL table at the port %s.", port_name.c_str());
        result &= false;
    }

    if (!deinitMACsecACLTable(macsec_port.m_egress_acl_table, port_id, SAI_MACSEC_DIRECTION_EGRESS, phy))
    {
        SWSS_LOG_WARN("Cannot deinit egress ACL table at the port %s.", port_name.c_str());
        result &= false;
    }

    if (!deleteMACsecPort(macsec_port.m_egress_port_id))
    {
        SWSS_LOG_WARN("Cannot delete MACsec egress port at the port %s", port_name.c_str());
        result &= false;
    }

    if (!deleteMACsecPort(macsec_port.m_ingress_port_id))
    {
        SWSS_LOG_WARN("Cannot delete MACsec ingress port at the port %s", port_name.c_str());
        result &= false;
    }

    if (phy)
    {
        if (!setPFCForward(port_id, false))
        {
            SWSS_LOG_WARN("Cannot disable PFC forward at the port %s.", port_name.c_str());
            result &= false;
        }

        if (phy->macsec_ipg != 0)
        {
            if (!m_port_orch->setPortIPG(port.m_port_id, macsec_port.m_original_ipg))
            {
                SWSS_LOG_WARN("Cannot set MACsec IPG to %u at the port %s", macsec_port.m_original_ipg, port_name.c_str());
                result &= false;
            }
        }
    }

    m_state_macsec_port.del(port_name);

    return true;
}

bool MACsecOrch::deleteMACsecPort(sai_object_id_t macsec_port_id)
{
    sai_status_t status = sai_macsec_api->remove_macsec_port(macsec_port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::createMACsecFlow(
    sai_object_id_t &flow_id,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    if (flow_id != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_FLOW_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = direction;
    attrs.push_back(attr);
    sai_status_t status = sai_macsec_api->create_macsec_flow(
                            &flow_id,
                            switch_id,
                            static_cast<uint32_t>(attrs.size()),
                            attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::deleteMACsecFlow(sai_object_id_t flow_id)
{
    sai_status_t status = sai_macsec_api->remove_macsec_flow(flow_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

task_process_status MACsecOrch::updateMACsecSC(
    const std::string &port_sci,
    const TaskArgs &sc_attr,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    MACsecSCI sci;
    if (!extract_variables(port_sci, ':', port_name, sci))
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci.c_str());
        return task_failed;
    }

    MACsecOrchContext ctx(this, port_name, direction, sci);

    if (ctx.get_macsec_port() == nullptr)
    {
        return task_need_retry;
    }

    if (ctx.get_switch_id() == nullptr || ctx.get_macsec_obj() == nullptr)
    {
        SWSS_LOG_ERROR("Cannot find switch id at the port %s.", port_name.c_str());
        return task_failed;
    }

    if (ctx.get_macsec_sc() == nullptr)
    {
        if (!createMACsecSC(
                *ctx.get_macsec_port(),
                port_name,
                sc_attr,
                *ctx.get_macsec_obj(),
                sci,
                *ctx.get_switch_id(),
                direction))
        {
            return task_failed;
        }
    }
    else
    {
        if (!setEncodingAN(*ctx.get_macsec_sc(), sc_attr, direction))
        {
            return task_failed;
        }
    }

    return task_success;
}

bool MACsecOrch::setEncodingAN(
    MACsecSC &sc,
    const TaskArgs &sc_attr,
    sai_macsec_direction_t direction)
{
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        if (!get_value(sc_attr, "encoding_an", sc.m_encoding_an))
        {
            SWSS_LOG_WARN("Wrong parameter, the encoding AN cannot be found");
            return false;
        }
    }
    else
    {
        SWSS_LOG_WARN("Cannot set encoding AN for the ingress SC");
        return false;
    }
    return true;
}

bool MACsecOrch::createMACsecSC(
    MACsecPort &macsec_port,
    const std::string &port_name,
    const TaskArgs &sc_attr,
    const MACsecObject &macsec_obj,
    sai_uint64_t sci,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    const std::string port_sci = swss::join(':', port_name, MACsecSCI(sci));

    auto scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? &macsec_port.m_egress_scs
            : &macsec_port.m_ingress_scs;
    auto sc_itr = scs->emplace(
        std::piecewise_construct,
        std::forward_as_tuple(sci),
        std::forward_as_tuple());
    if (!sc_itr.second)
    {
        SWSS_LOG_ERROR("The SC %s has been created.", port_sci.c_str());
        return false;
    }
    recover.add_action([scs, sc_itr]() {
        scs->erase(sc_itr.first->first);
    });
    auto sc = &sc_itr.first->second;

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        get_value(sc_attr, "encoding_an", sc->m_encoding_an);
    }

    sc->m_flow_id = SAI_NULL_OBJECT_ID;
    // If SCI can only be used as ACL field
    // Which means one flow can be associated with only one ACL entry and one SC.
    if (macsec_obj.m_sci_in_ingress_macsec_acl)
    {
        if (!createMACsecFlow(sc->m_flow_id, switch_id, direction))
        {
            SWSS_LOG_WARN("Cannot create MACsec Flow");
            return false;
        }
        recover.add_action([this, sc]() { this->deleteMACsecFlow(sc->m_flow_id); });
    }
    else
    {
        sc->m_flow_id =
            (direction == SAI_MACSEC_DIRECTION_EGRESS)
                ? macsec_port.m_egress_flow_id
                : macsec_port.m_ingress_flow_id;
    }

    if (!createMACsecSC(
            sc->m_sc_id,
            switch_id,
            direction,
            sc->m_flow_id,
            sci,
            macsec_port.m_enable_encrypt,
            macsec_port.m_sci_in_sectag,
            macsec_port.m_cipher_suite))
    {
        SWSS_LOG_WARN("Create MACsec SC %s fail.", port_sci.c_str());
        return false;
    }
    recover.add_action([this, sc]() { this->deleteMACsecSC(sc->m_sc_id); });

    auto table =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? &macsec_port.m_egress_acl_table
            : &macsec_port.m_ingress_acl_table;
    if (table->m_available_acl_priorities.empty())
    {
        SWSS_LOG_WARN("Available ACL priorities have been exhausted.");
        return false;
    }
    sc->m_acl_priority = *(table->m_available_acl_priorities.begin());
    table->m_available_acl_priorities.erase(table->m_available_acl_priorities.begin());
    if (!createMACsecACLDataEntry(
            sc->m_entry_id,
            table->m_table_id,
            switch_id,
            macsec_port.m_sci_in_sectag,
            sci,
            sc->m_acl_priority))
    {
        SWSS_LOG_WARN("Cannot create ACL Data entry");
        return false;
    }
    recover.add_action([this, sc, table]() {
        deleteMACsecACLEntry(sc->m_entry_id);
        table->m_available_acl_priorities.insert(sc->m_acl_priority);
    });

    SWSS_LOG_NOTICE("MACsec SC %s is created.", port_sci.c_str());

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sc.set(swss::join('|', port_name, MACsecSCI(sci)), fvVector);
    }
    else
    {
        m_state_macsec_ingress_sc.set(swss::join('|', port_name, MACsecSCI(sci)), fvVector);
    }

    recover.clear();
    return true;
}

bool MACsecOrch::createMACsecSC(
    sai_object_id_t &sc_id,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction,
    sai_object_id_t flow_id,
    sai_uint64_t sci,
    bool encryption_enable,
    bool send_sci,
    sai_macsec_cipher_suite_t cipher_suite)
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
    attr.id = SAI_MACSEC_SC_ATTR_ENCRYPTION_ENABLE;
    attr.value.booldata = encryption_enable;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_SC_ATTR_MACSEC_EXPLICIT_SCI_ENABLE;
    attr.value.booldata = send_sci;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_SC_ATTR_MACSEC_CIPHER_SUITE;
    attr.value.s32 = cipher_suite;
    attrs.push_back(attr);

    sai_status_t status = sai_macsec_api->create_macsec_sc(
                                &sc_id,
                                switch_id,
                                static_cast<uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec egress SC %s", MACsecSCI(sci).str().c_str());
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

task_process_status MACsecOrch::deleteMACsecSC(
    const std::string &port_sci,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    MACsecSCI sci;
    if (!extract_variables(port_sci, ':', port_name, sci))
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci.c_str());
        return task_failed;
    }

    MACsecOrchContext ctx(this, port_name, direction, sci);

    if (ctx.get_macsec_sc() == nullptr)
    {
        SWSS_LOG_INFO("The MACsec SC %s wasn't created", port_sci.c_str());
        return task_success;
    }

    auto result = task_success;

    auto sa = ctx.get_macsec_sc()->m_sa_ids.begin();
    while (sa != ctx.get_macsec_sc()->m_sa_ids.end())
    {
        const std::string port_sci_an = swss::join(':', port_sci, sa->first);
        sa ++;
        deleteMACsecSA(port_sci_an, direction);
    }

    deleteMACsecACLEntry(ctx.get_macsec_sc()->m_entry_id);
    ctx.get_acl_table()->m_available_acl_priorities.insert(ctx.get_macsec_sc()->m_acl_priority);

    if (!deleteMACsecSC(ctx.get_macsec_sc()->m_sc_id))
    {
        SWSS_LOG_WARN("The MACsec SC %s cannot be deleted", port_sci.c_str());
        result = task_failed;
    }

    if (ctx.get_macsec_obj()->m_sci_in_ingress_macsec_acl)
    {
        if (!deleteMACsecFlow(ctx.get_macsec_sc()->m_flow_id))
        {
            SWSS_LOG_WARN("Cannot delete MACsec flow");
            result = task_failed;
        }
    }

    auto scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
            ? &ctx.get_macsec_port()->m_egress_scs
            : &ctx.get_macsec_port()->m_ingress_scs;
    scs->erase(sci);

    SWSS_LOG_NOTICE("MACsec SC %s is deleted.", port_sci.c_str());

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sc.del(swss::join('|', port_name, MACsecSCI(sci)));
    }
    else
    {
        m_state_macsec_ingress_sc.del(swss::join('|', port_name, MACsecSCI(sci)));
    }

    return result;
}

bool MACsecOrch::deleteMACsecSC(sai_object_id_t sc_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_macsec_api->remove_macsec_sc(sc_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::updateMACsecAttr(sai_object_type_t object_type, sai_object_id_t object_id, const sai_attribute_t &attr)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    if (object_type == SAI_OBJECT_TYPE_MACSEC_PORT)
    {
        status = sai_macsec_api->set_macsec_port_attribute(object_id, &attr);
    }
    else if (object_type == SAI_OBJECT_TYPE_MACSEC_SC)
    {
        status = sai_macsec_api->set_macsec_sc_attribute(object_id, &attr);
    }
    else if (object_type == SAI_OBJECT_TYPE_MACSEC_SA)
    {
        status = sai_macsec_api->set_macsec_sa_attribute(object_id, &attr);
    }
    else
    {
        SWSS_LOG_ERROR("Wrong type %s", sai_serialize_object_type(object_type).c_str());
        return false;
    }

    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

task_process_status MACsecOrch::createMACsecSA(
    const std::string &port_sci_an,
    const TaskArgs &sa_attr,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    MACsecSCI sci;
    macsec_an_t an = 0;
    if (!extract_variables(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    MACsecOrchContext ctx(this, port_name, direction, sci, an);

    if (ctx.get_macsec_sa() != nullptr)
    {
        SWSS_LOG_NOTICE("The MACsec SA %s has been created.", port_sci_an.c_str());
        return task_success;
    }

    if (ctx.get_macsec_sc() == nullptr)
    {
        SWSS_LOG_INFO("The MACsec SC %s hasn't been created at the port %s.", sci.str().c_str(), port_name.c_str());
        return task_need_retry;
    }
    auto sc = ctx.get_macsec_sc();

    MACsecSAK sak = {{0}, false};
    MACsecSalt salt = {0};
    sai_uint32_t ssci = 0;
    MACsecAuthKey auth_key = {0};
    try
    {
        if (!get_value(sa_attr, "sak", sak))
        {
            SWSS_LOG_WARN("The SAK isn't existed at SA %s", port_sci_an.c_str());
            return task_failed;
        }
        if (sak.m_sak_256_enable)
        {
            if (ctx.get_macsec_port()->m_cipher_suite == SAI_MACSEC_CIPHER_SUITE_GCM_AES_128 
                && ctx.get_macsec_port()->m_cipher_suite == SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_128)
            {
                SWSS_LOG_WARN("Wrong SAK with 256 bit, expect 128 bit");
                return task_failed;
            }
        }
        else
        {
            if (ctx.get_macsec_port()->m_cipher_suite == SAI_MACSEC_CIPHER_SUITE_GCM_AES_256 
                && ctx.get_macsec_port()->m_cipher_suite == SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_256)
            {
                SWSS_LOG_WARN("Wrong SAK with 128 bit, expect 256 bit");
                return task_failed;
            }
        }
        if (ctx.get_macsec_port()->m_cipher_suite == SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_128 
            || ctx.get_macsec_port()->m_cipher_suite == SAI_MACSEC_CIPHER_SUITE_GCM_AES_XPN_256)
        {
            if (!get_value(sa_attr, "salt", salt))
            {
                SWSS_LOG_WARN("The salt isn't existed at SA %s", port_sci_an.c_str());
                return task_failed;
            }
            if (!get_value(sa_attr, "ssci", ssci))
            {
                SWSS_LOG_WARN("The ssci isn't existed at SA %s", port_sci_an.c_str());
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
    sai_uint64_t pn = 1;
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        if (!get_value(sa_attr, "next_pn", pn))
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

    // If this SA is the first SA
    // change the ACL entry action from packet action to MACsec flow
    if (ctx.get_macsec_port()->m_enable && sc->m_sa_ids.empty())
    {
        if (!setMACsecFlowActive(sc->m_entry_id, sc->m_flow_id, true))
        {
            SWSS_LOG_WARN("Cannot change the ACL entry action from packet action to MACsec flow");
            return task_failed;
        }
        recover.add_action([this, sc]() {
            this->setMACsecFlowActive(
                sc->m_entry_id,
                sc->m_flow_id,
                false);
        });
    }

    if (!createMACsecSA(
            sc->m_sa_ids[an],
            *ctx.get_switch_id(),
            direction,
            sc->m_sc_id,
            an,
            sak.m_sak,
            salt.m_salt,
            ssci,
            auth_key.m_auth_key,
            pn))
    {
        SWSS_LOG_WARN("Cannot create the SA %s", port_sci_an.c_str());
        return task_failed;
    }
    recover.add_action([this, sc, an]() {
        this->deleteMACsecSA(sc->m_sa_ids[an]);
        sc->m_sa_ids.erase(an);
    });

    installCounter(ctx, CounterType::MACSEC_SA_ATTR, direction, port_sci_an, sc->m_sa_ids[an], macsec_sa_attrs);
    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        installCounter(ctx, CounterType::MACSEC_SA, direction, port_sci_an, sc->m_sa_ids[an], macsec_sa_egress_stats);
        m_state_macsec_egress_sa.set(swss::join('|', port_name, sci, an), fvVector);
    }
    else
    {
        installCounter(ctx, CounterType::MACSEC_SA, direction, port_sci_an, sc->m_sa_ids[an], macsec_sa_ingress_stats);
        m_state_macsec_ingress_sa.set(swss::join('|', port_name, sci, an), fvVector);
    }

    SWSS_LOG_NOTICE("MACsec SA %s is created.", port_sci_an.c_str());

    recover.clear();
    return task_success;
}

task_process_status MACsecOrch::deleteMACsecSA(
    const std::string &port_sci_an,
    sai_macsec_direction_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name = "";
    MACsecSCI sci;
    macsec_an_t an = 0;
    if (!extract_variables(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    MACsecOrchContext ctx(this, port_name, direction, sci, an);

    if (ctx.get_macsec_sa() == nullptr)
    {
        SWSS_LOG_INFO("MACsec SA %s has been deleted.", port_sci_an.c_str());
        return task_success;
    }

    auto result = task_success;

    uninstallCounter(ctx, CounterType::MACSEC_SA_ATTR, direction, port_sci_an, ctx.get_macsec_sc()->m_sa_ids[an]);
    uninstallCounter(ctx, CounterType::MACSEC_SA, direction, port_sci_an, ctx.get_macsec_sc()->m_sa_ids[an]);
    if (!deleteMACsecSA(ctx.get_macsec_sc()->m_sa_ids[an]))
    {
        SWSS_LOG_WARN("Cannot delete the MACsec SA %s.", port_sci_an.c_str());
        result = task_failed;
    }
    ctx.get_macsec_sc()->m_sa_ids.erase(an);

    // If this SA is the last SA
    // change the ACL entry action from MACsec flow to packet action
    if (ctx.get_macsec_sc()->m_sa_ids.empty())
    {
        if (!setMACsecFlowActive(ctx.get_macsec_sc()->m_entry_id, ctx.get_macsec_sc()->m_flow_id, false))
        {
            SWSS_LOG_WARN("Cannot change the ACL entry action from MACsec flow to packet action");
            result = task_failed;
        }
    }


    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sa.del(swss::join('|', port_name, sci, an));
    }
    else
    {
        m_state_macsec_ingress_sa.del(swss::join('|', port_name, sci, an));
    }

    SWSS_LOG_NOTICE("MACsec SA %s is deleted.", port_sci_an.c_str());
    return result;
}

bool MACsecOrch::createMACsecSA(
    sai_object_id_t &sa_id,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction,
    sai_object_id_t sc_id,
    macsec_an_t an,
    sai_macsec_sak_t sak,
    sai_macsec_salt_t salt,
    sai_uint32_t ssci,
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

    attr.id = SAI_MACSEC_SA_ATTR_SAK;
    std::copy(sak, sak + sizeof(attr.value.macsecsak), attr.value.macsecsak);
    attrs.push_back(attr);

    // Valid when SAI_MACSEC_SC_ATTR_MACSEC_XPN64_ENABLE == true.
    attr.id = SAI_MACSEC_SA_ATTR_SALT;
    std::copy(salt, salt + sizeof(attr.value.macsecsalt), attr.value.macsecsalt);
    attrs.push_back(attr);

    // Valid when SAI_MACSEC_SC_ATTR_MACSEC_XPN64_ENABLE == true.
    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_SSCI;
    attr.value.u32 = ssci;
    attrs.push_back(attr);

    attr.id = SAI_MACSEC_SA_ATTR_AUTH_KEY;
    std::copy(auth_key, auth_key + sizeof(attr.value.macsecauthkey), attr.value.macsecauthkey);
    attrs.push_back(attr);

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        attr.id = SAI_MACSEC_SA_ATTR_CONFIGURED_EGRESS_XPN;
        attr.value.u64 = pn;
        attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_MACSEC_SA_ATTR_MINIMUM_INGRESS_XPN;
        attr.value.u64 = pn;
        attrs.push_back(attr);
    }

    sai_status_t status = sai_macsec_api->create_macsec_sa(
                                &sa_id,
                                switch_id,
                                static_cast<uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::deleteMACsecSA(sai_object_id_t sa_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_macsec_api->remove_macsec_sa(sa_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_MACSEC, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

FlexCounterManager& MACsecOrch::MACsecSaStatManager(MACsecOrchContext &ctx)
{
    if (ctx.get_gearbox_phy() != nullptr)
        return m_gb_macsec_sa_stat_manager;
    return m_macsec_sa_stat_manager;
}

FlexCounterManager& MACsecOrch::MACsecSaAttrStatManager(MACsecOrchContext &ctx)
{
    if (ctx.get_gearbox_phy() != nullptr)
        return m_gb_macsec_sa_attr_manager;
    return m_macsec_sa_attr_manager;
}

FlexCounterManager& MACsecOrch::MACsecFlowStatManager(MACsecOrchContext &ctx)
{
    if (ctx.get_gearbox_phy() != nullptr)
        return m_gb_macsec_flow_stat_manager;
    return m_macsec_flow_stat_manager;
}

Table& MACsecOrch::MACsecCountersMap(MACsecOrchContext &ctx)
{
    if (ctx.get_gearbox_phy() != nullptr)
        return m_gb_macsec_counters_map;
    return m_macsec_counters_map;
}

void MACsecOrch::installCounter(
    MACsecOrchContext &ctx,
    CounterType counter_type,
    sai_macsec_direction_t direction,
    const std::string &obj_name,
    sai_object_id_t obj_id,
    const std::vector<std::string> &stats)
{
    FieldValueTuple tuple(obj_name, sai_serialize_object_id(obj_id));
    vector<FieldValueTuple> fields;
    fields.push_back(tuple);

    std::unordered_set<std::string> counter_stats;
    for (const auto &stat : stats)
    {
        counter_stats.emplace(stat);
    }
    switch(counter_type)
    {
        case CounterType::MACSEC_SA_ATTR:
            MACsecSaAttrStatManager(ctx).setCounterIdList(obj_id, counter_type, counter_stats);
            MACsecCountersMap(ctx).set("", fields);
            break;

        case CounterType::MACSEC_SA:
            MACsecSaStatManager(ctx).setCounterIdList(obj_id, counter_type, counter_stats);
            MACsecCountersMap(ctx).set("", fields);
            break;

        case CounterType::MACSEC_FLOW:
            MACsecFlowStatManager(ctx).setCounterIdList(obj_id, counter_type, counter_stats);
            break;

        default:
            SWSS_LOG_ERROR("Failed to install unknown counter type %u.\n",
                           static_cast<uint32_t>(counter_type));
            break;
    }
}

void MACsecOrch::uninstallCounter(
    MACsecOrchContext &ctx,
    CounterType counter_type,
    sai_macsec_direction_t direction,
    const std::string &obj_name,
    sai_object_id_t obj_id)
{
    switch(counter_type)
    {
        case CounterType::MACSEC_SA_ATTR:
            MACsecSaAttrStatManager(ctx).clearCounterIdList(obj_id);
            m_counter_db.hdel(COUNTERS_MACSEC_NAME_MAP, obj_name);
            break;

        case CounterType::MACSEC_SA:
            MACsecSaStatManager(ctx).clearCounterIdList(obj_id);
            if (direction == SAI_MACSEC_DIRECTION_EGRESS)
            {
                m_counter_db.hdel(COUNTERS_MACSEC_SA_TX_NAME_MAP, obj_name);
            }
            else
            {
                m_counter_db.hdel(COUNTERS_MACSEC_SA_RX_NAME_MAP, obj_name);
            }
            break;

        case CounterType::MACSEC_FLOW:
            MACsecFlowStatManager(ctx).clearCounterIdList(obj_id);
            break;

        default:
            SWSS_LOG_ERROR("Failed to uninstall unknown counter type %u.\n",
                           static_cast<uint32_t>(counter_type));
            break;
    }

}

bool MACsecOrch::initMACsecACLTable(
    MACsecACLTable &acl_table,
    sai_object_id_t port_id,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction,
    bool sci_in_sectag,
    const std::string &port_name,
    const gearbox_phy_t* phy)
{
    SWSS_LOG_ENTER();

    RecoverStack recover;

    if (!createMACsecACLTable(acl_table.m_table_id, switch_id, direction, sci_in_sectag))
    {
        SWSS_LOG_WARN("Cannot create ACL table");
        return false;
    }
    recover.add_action([this, &acl_table]() {
        this->deleteMACsecACLTable(acl_table.m_table_id);
        acl_table.m_table_id = SAI_NULL_OBJECT_ID;
    });

    if (!createMACsecACLEAPOLEntry(
            acl_table.m_eapol_packet_forward_entry_id,
            acl_table.m_table_id,
            switch_id))
    {
        SWSS_LOG_WARN("Cannot create ACL EAPOL entry");
        return false;
    }
    recover.add_action([this, &acl_table]() {
        this->deleteMACsecACLEntry(acl_table.m_eapol_packet_forward_entry_id);
        acl_table.m_eapol_packet_forward_entry_id = SAI_NULL_OBJECT_ID;
    });

    if (!bindMACsecACLTabletoPort(acl_table.m_table_id, port_id, direction))
    {
        SWSS_LOG_WARN("Cannot bind ACL table");
        return false;
    }
    recover.add_action([this, port_id, direction]() { this->unbindMACsecACLTable(port_id, direction); });

    sai_uint32_t minimum_priority = 0;
    if (!getAclPriority(switch_id, SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY, minimum_priority))
    {
        return false;
    }
    sai_uint32_t maximum_priority = 0;
    if (!getAclPriority(switch_id, SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY, maximum_priority))
    {
        return false;
    }
    sai_uint32_t priority = minimum_priority + 1;
    while (priority < maximum_priority)
    {
        if (acl_table.m_available_acl_priorities.size() >= AVAILABLE_ACL_PRIORITIES_LIMITATION)
        {
            break;
        }
        acl_table.m_available_acl_priorities.insert(priority);
        priority += 1;
    }
    recover.add_action([&acl_table]() { acl_table.m_available_acl_priorities.clear(); });

    if (phy)
    {
        if (acl_table.m_available_acl_priorities.empty())
        {
            SWSS_LOG_WARN("Available ACL priorities have been exhausted.");
            return false;
        }
        priority = *(acl_table.m_available_acl_priorities.rbegin());
        acl_table.m_available_acl_priorities.erase(std::prev(acl_table.m_available_acl_priorities.end()));

        TaskArgs values;
        if (!m_applPortTable.get(port_name, values))
        {
            SWSS_LOG_ERROR("Port %s isn't existing", port_name.c_str());
            return false;
        }
        std::string pfc_mode = PFC_MODE_DEFAULT;
        get_value(values, "pfc_encryption_mode", pfc_mode);

        if (!createPFCEntry(acl_table.m_pfc_entry_id, acl_table.m_table_id, switch_id, direction, priority, pfc_mode))
        {
            return false;
        }
        recover.add_action([this, &acl_table, priority]() {
            this->deleteMACsecACLEntry(acl_table.m_pfc_entry_id);
            acl_table.m_pfc_entry_id = SAI_NULL_OBJECT_ID;
            acl_table.m_available_acl_priorities.insert(priority);
        });
    }

    recover.clear();
    return true;
}

bool MACsecOrch::deinitMACsecACLTable(
    const MACsecACLTable &acl_table,
    sai_object_id_t port_id,
    sai_macsec_direction_t direction,
    const gearbox_phy_t* phy)
{
    bool result = true;

    if (!unbindMACsecACLTable(port_id, direction))
    {
        SWSS_LOG_WARN("Cannot unbind ACL table");
        result &= false;
    }
    if (!deleteMACsecACLEntry(acl_table.m_eapol_packet_forward_entry_id))
    {
        SWSS_LOG_WARN("Cannot delete EAPOL ACL entry");
        result &= false;
    }
    if (phy)
    {
        if (!deleteMACsecACLEntry(acl_table.m_pfc_entry_id))
        {
            SWSS_LOG_WARN("Cannot delete PFC ACL entry");
            result &= false;
        }
    }
    if (!deleteMACsecACLTable(acl_table.m_table_id))
    {
        SWSS_LOG_WARN("Cannot delete ACL table");
        result &= false;
    }

    return result;
}

bool MACsecOrch::createMACsecACLTable(
    sai_object_id_t &table_id,
    sai_object_id_t switch_id,
    sai_macsec_direction_t direction,
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

    sai_status_t status = sai_acl_api->create_acl_table(
                                &table_id,
                                switch_id,
                                static_cast<std::uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::deleteMACsecACLTable(sai_object_id_t table_id)
{
    sai_status_t status = sai_acl_api->remove_acl_table(table_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::bindMACsecACLTabletoPort(
    sai_object_id_t table_id,
    sai_object_id_t port_id,
    sai_macsec_direction_t direction)
{
    sai_attribute_t attr;

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        attr.id = SAI_PORT_ATTR_EGRESS_MACSEC_ACL;
    }
    else
    {
        attr.id = SAI_PORT_ATTR_INGRESS_MACSEC_ACL;
    }
    attr.value.oid = table_id;

    sai_status_t status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::unbindMACsecACLTable(
    sai_object_id_t port_id,
    sai_macsec_direction_t direction)
{
    sai_attribute_t attr;

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        attr.id = SAI_PORT_ATTR_EGRESS_MACSEC_ACL;
    }
    else
    {
        attr.id = SAI_PORT_ATTR_INGRESS_MACSEC_ACL;
    }
    attr.value.oid = SAI_NULL_OBJECT_ID;

    sai_status_t status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::createMACsecACLEAPOLEntry(
    sai_object_id_t &entry_id,
    sai_object_id_t table_id,
    sai_object_id_t switch_id)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    static const MacAddress nearest_non_tpmr_bridge("01:80:c2:00:00:03");
    static const MacAddress mac_address_mask("ff:ff:ff:ff:ff:ff");

    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = table_id;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    if (!getAclPriority(switch_id, SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY, attr.value.u32))
    {
        return false;
    }
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC;
    nearest_non_tpmr_bridge.getMac(attr.value.aclfield.data.mac);
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
    sai_status_t status = sai_acl_api->create_acl_entry(
                                &entry_id,
                                switch_id,
                                static_cast<std::uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::createMACsecACLDataEntry(
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
    if (sci_in_sectag)
    {
        attr.id = SAI_ACL_ENTRY_ATTR_FIELD_MACSEC_SCI;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.mask.u64 = 0xFFFFFFFFFFFFFFFF;
        attr.value.aclfield.data.u64 = sci;
        attrs.push_back(attr);
    }

    sai_status_t status = sai_acl_api->create_acl_entry(
                                &entry_id,
                                switch_id,
                                static_cast<std::uint32_t>(attrs.size()),
                                attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::setMACsecFlowActive(sai_object_id_t entry_id, sai_object_id_t flow_id, bool active)
{
    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_MACSEC_FLOW;
    attr.value.aclaction.parameter.oid = flow_id;
    attr.value.aclaction.enable = active;

    sai_status_t status = sai_acl_api->set_acl_entry_attribute(entry_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DROP;
    attr.value.aclaction.enable = !active;
    status = sai_acl_api->set_acl_entry_attribute(entry_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool MACsecOrch::deleteMACsecACLEntry(sai_object_id_t entry_id)
{
    if (entry_id == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    sai_status_t status = sai_acl_api->remove_acl_entry(entry_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool MACsecOrch::getAclPriority(sai_object_id_t switch_id, sai_attr_id_t priority_id, sai_uint32_t &priority) const
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = priority_id;
    attrs.push_back(attr);
    if (sai_switch_api->get_switch_attribute(
            switch_id,
            static_cast<std::uint32_t>(attrs.size()),
            attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Cannot fetch ACL Priority from switch");
        return false;
    }
    priority = attrs.front().value.u32;

    return true;
}

bool MACsecOrch::setPFCForward(sai_object_id_t port_id, bool enable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t status;

    // Enable/Disable Forward pause frame
    attr.id = SAI_PORT_ATTR_GLOBAL_FLOW_CONTROL_FORWARD;
    attr.value.booldata = enable;
    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    // Enable/Disable Forward PFC frame
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_FORWARD;
    attr.value.booldata = enable;
    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool MACsecOrch::createPFCEntry(
        sai_object_id_t &entry_id,
        sai_object_id_t table_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        sai_uint32_t priority,
        const std::string &pfc_mode)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    if (pfc_mode == PFC_MODE_BYPASS)
    {
        attrs.push_back(identifyPFC());
        attrs.push_back(bypassPFC());
    }
    else if (pfc_mode == PFC_MODE_ENCRYPT)
    {
        if (direction == SAI_MACSEC_DIRECTION_EGRESS)
        {
            entry_id = SAI_NULL_OBJECT_ID;
            return true;
        }
        else
        {
            attrs.push_back(identifyPFC());
            attrs.push_back(bypassPFC());
        }
    }
    else if (pfc_mode == PFC_MODE_STRICT_ENCRYPT)
    {
        if (direction == SAI_MACSEC_DIRECTION_EGRESS)
        {
            entry_id = SAI_NULL_OBJECT_ID;
            return true;
        }
        else
        {
            attrs.push_back(identifyPFC());
            attrs.push_back(dropPFC());
        }
    }

    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = table_id;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = priority;
    attrs.push_back(attr);
    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    sai_status_t status = sai_acl_api->create_acl_entry(
                                    &entry_id,
                                    switch_id,
                                    static_cast<std::uint32_t>(attrs.size()),
                                    attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

sai_attribute_t MACsecOrch::identifyPFC() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE;
    attr.value.aclfield.data.u16 = PAUSE_ETHER_TYPE;
    attr.value.aclfield.mask.u16 = 0xFFFF;
    attr.value.aclfield.enable = true;

    return attr;
}

sai_attribute_t MACsecOrch::bypassPFC() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.parameter.s32 = SAI_PACKET_ACTION_FORWARD;
    attr.value.aclaction.enable = true;

    return attr;
}

sai_attribute_t MACsecOrch::dropPFC() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
    attr.value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DROP;
    attr.value.aclaction.enable = true;

    return attr;
}
