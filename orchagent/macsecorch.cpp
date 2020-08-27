#include "macsecorch.h"
#include "gearboxutils.h"
#include "sai_serialize.h"

#include <vector>
#include <sstream>
#include <algorithm>

extern sai_macsec_api_t* sai_macsec_api;
extern sai_acl_api_t* sai_acl_api;

template<typename T>
static bool split(const std::string & input, char delimiter, T & output)
{
    if (input.find(delimiter) != std::string::npos)
    {
        return false;
    }
    std::istringstream istream(input);
    istream >> output;
    return true;
}

template<typename T, typename ... Args>
static bool split(const std::string & input, char delimiter, T & output, Args & ... args)
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

template<typename T>
static std::string join(const T & input)
{
    std::ostringstream ostream;
    ostream << input;
    return ostream.str();
}

template<typename T>
static std::string join(char delimiter, const T & input)
{
    return join(input);
}

template<typename T, typename ... Args>
static std::string join(char delimiter, const T & input, const Args & ... args)
{
    std::ostringstream ostream;
    ostream << input << delimiter << join(delimiter, args...);
    return ostream.str();
}

template<class T>
static bool get_value(
    const MACsecOrch::TaskArgs & ta,
    const std::string & field,
    T & value)
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
        [&](const MACsecOrch::TaskArgs::value_type & entry)
        {
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

static std::istringstream& operator>>(
    std::istringstream &istream,
    bool & b)
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
    const std::string & hex_str,
    std::uint8_t * buffer,
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
    bool             m_sak_256_enable;
};

static std::istringstream& operator>>(
    std::istringstream &istream,
    MACsecSAK & sak)
{
    SWSS_LOG_ENTER();
    const std::string & buffer = istream.str();
    bool convert_done = false;
    if (buffer.length() == sizeof(sak.m_sak))
    {
        sak.m_sak_256_enable = false;
        convert_done = hex_to_binary(buffer, &sak.m_sak[8], sizeof(sak.m_sak) / 2);
    }
    else if (buffer.length() == sizeof(sak.m_sak) * 2)
    {
        sak.m_sak_256_enable = true;
        convert_done = hex_to_binary(buffer, sak.m_sak, sizeof(sak.m_sak));
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

static std::istringstream& operator>>(
    std::istringstream &istream,
    MACsecSalt & salt)
{
    SWSS_LOG_ENTER();
    const std::string & buffer = istream.str();

    if (
        (buffer.length() != sizeof(salt.m_salt) * 2)
        || (!hex_to_binary(buffer, salt.m_salt, sizeof(salt.m_salt))))
    {
        throw std::invalid_argument("Invalid SALT : " + buffer);
    }
    return istream;
}

struct MACsecAuthKey
{
    sai_macsec_auth_key_t m_auth_key;
};

static std::istringstream& operator>>(
    std::istringstream &istream,
    MACsecAuthKey & auth_key)
{
    SWSS_LOG_ENTER();
    const std::string & buffer = istream.str();

    if (
        (buffer.length() != sizeof(auth_key.m_auth_key) * 2)
        || (!hex_to_binary(buffer, auth_key.m_auth_key, sizeof(auth_key.m_auth_key))))
    {
        throw std::invalid_argument("Invalid Auth Key : " + buffer);
    }
    return istream;
}

MACsecOrch::MACsecOrch(
    DBConnector *appDb,
    DBConnector *state_db,
    const std::vector<std::string> &tables,
    PortsOrch * port_orch) :
    Orch(appDb, tables),
    m_port_orch(port_orch),
    m_state_macsec_port(state_db, STATE_MACSEC_PORT_TABLE_NAME),
    m_state_macsec_egress_sc(state_db, STATE_MACSEC_EGRESS_SC_TABLE_NAME),
    m_state_macsec_ingress_sc(state_db, STATE_MACSEC_INGRESS_SC_TABLE_NAME),
    m_state_macsec_egress_sa(state_db, STATE_MACSEC_EGRESS_SA_TABLE_NAME),
    m_state_macsec_ingress_sa(state_db, STATE_MACSEC_INGRESS_SA_TABLE_NAME),
    m_gearbox_table(appDb, "_GEARBOX_TABLE"),
    m_gearbox_enabled(false)
{
}

MACsecOrch::~MACsecOrch()
{
    while (!m_macsec_ports.empty())
    {
        auto port = m_macsec_ports.begin();
        const TaskArgs temp;
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

    using TaskType = std::tuple<const std::string,const std::string>;
    using TaskFunc = task_process_status (MACsecOrch::*)(
        const std::string &,
        const TaskArgs &);
    const static std::map<TaskType, TaskFunc > TaskMap = {
        { { APP_MACSEC_PORT_TABLE_NAME, SET_COMMAND },
            &MACsecOrch::enableMACsecPort },
        { { APP_MACSEC_PORT_TABLE_NAME, DEL_COMMAND },
            &MACsecOrch::disableMACsecPort },
        { { APP_MACSEC_EGRESS_SC_TABLE_NAME, SET_COMMAND },
            &MACsecOrch::updateEgressSC },
        { { APP_MACSEC_EGRESS_SC_TABLE_NAME, DEL_COMMAND },
            &MACsecOrch::deleteEgressSC },
        { { APP_MACSEC_INGRESS_SC_TABLE_NAME, SET_COMMAND },
            &MACsecOrch::updateIngressSC },
        { { APP_MACSEC_INGRESS_SC_TABLE_NAME, DEL_COMMAND },
            &MACsecOrch::deleteIngressSC },
        { { APP_MACSEC_EGRESS_SA_TABLE_NAME, SET_COMMAND },
            &MACsecOrch::updateEgressSA },
        { { APP_MACSEC_EGRESS_SA_TABLE_NAME, DEL_COMMAND },
            &MACsecOrch::deleteEgressSA },
        { { APP_MACSEC_INGRESS_SA_TABLE_NAME, SET_COMMAND },
            &MACsecOrch::updateIngressSA },
        { { APP_MACSEC_INGRESS_SA_TABLE_NAME, DEL_COMMAND },
            &MACsecOrch::deleteIngressSA },
    };

    const std::string & table_name = consumer.getTableName();
    auto itr = consumer.m_toSync.begin();
    while (itr != consumer.m_toSync.end())
    {
        task_process_status task_done = task_failed;
        auto & message = itr->second;
        const std::string & op = kfvOp(message);

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

task_process_status MACsecOrch::enableMACsecPort(
    const std::string & port_name,
    const TaskArgs & port_attr)
{
    SWSS_LOG_ENTER();

    Port port;
    if (!m_port_orch->getPort(port_name, port))
    {
        SWSS_LOG_WARN("Port %s cannot be found.", port_name.c_str());
        return task_failed;
    }
    sai_object_id_t switch_id = 0;
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
    auto macsec_port = createMACsecPort(port, switch_id);
    if (macsec_port == nullptr)
    {
        SWSS_LOG_WARN("Cannot init MACsec port at the port %s.", port_name.c_str());
        return task_failed;
    }

    // Set flex counter

    macsec_port->m_enable_encrypt = true;
    get_value(port_attr, "enable_encrypt", macsec_port->m_enable_encrypt);

    macsec_obj->second.m_ports[port_name] = macsec_port;

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    m_state_macsec_port.set(port_name, fvVector);

    return task_success;
}

task_process_status MACsecOrch::disableMACsecPort(
    const std::string & port_name,
    const TaskArgs & port_attr)
{
    SWSS_LOG_ENTER();

    sai_object_id_t switch_id = 0;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    task_process_status result = task_success;
    if (!deleteMACsecPort(port_name))
    {
        SWSS_LOG_WARN("Cannot delete macsec port at the port %s.", port_name.c_str());
        result = task_failed;
    }

    // Delete flex counter

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
    const std::string & port_sci,
    const TaskArgs & sc_attr)
{
    SWSS_LOG_ENTER();
    return updateMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::deleteEgressSC(
    const std::string & port_sci,
    const TaskArgs & sc_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_EGRESS);
}

task_process_status MACsecOrch::updateIngressSC(
    const std::string & port_sci,
    const TaskArgs & sc_attr)
{
    SWSS_LOG_ENTER();
    return updateMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_INGRESS);
}

task_process_status MACsecOrch::deleteIngressSC(
    const std::string & port_sci,
    const TaskArgs & sc_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSC(port_sci, sc_attr, SAI_MACSEC_DIRECTION_INGRESS);
}

task_process_status MACsecOrch::updateEgressSA(
    const std::string & port_sci_an,
    const TaskArgs & sa_attr)
{
    SWSS_LOG_ENTER();
    std::string port_name;
    sai_uint64_t sci;
    sai_uint8_t an;
    if (!split(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }
    auto port = m_macsec_ports.find(port_name);
    if (port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_need_retry;
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
    const std::string & port_sci_an,
    const TaskArgs & sa_attr)
{
    SWSS_LOG_ENTER();
    return deleteMACsecSA(port_sci_an, sa_attr, SAI_MACSEC_DIRECTION_EGRESS);;
}

task_process_status MACsecOrch::updateIngressSA(
    const std::string & port_sci_an,
    const TaskArgs & sa_attr)
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
        return deleteMACsecSA(port_sci_an, sa_attr, SAI_MACSEC_DIRECTION_INGRESS);
    }
    
    return task_success;
}

task_process_status MACsecOrch::deleteIngressSA(
    const std::string & port_sci_an,
    const TaskArgs & sa_attr)
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

bool MACsecOrch::getGearboxSwitchId(const std::string & port_name, sai_object_id_t & switch_id) const
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

bool MACsecOrch::getGearboxSwitchId(const Port & port, sai_object_id_t & switch_id) const
{
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

    auto macsec_obj = m_macsec_objs.emplace(switch_id, MACsecObject());
    if (!macsec_obj.second)
    {
        SWSS_LOG_INFO("The MACsec has been initialized at the switch %lu", switch_id);
        return macsec_obj.first;
    }

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_EGRESS;
    attrs.push_back(attr);
    if(sai_macsec_api->create_macsec(
        &macsec_obj.first->second.m_egress_id,
        switch_id,
        static_cast<uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot initialize MACsec egress object at the switch %lu", switch_id);
        m_macsec_objs.erase(macsec_obj.first);
        return m_macsec_objs.end();
    }

    attrs.clear();
    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_INGRESS;
    attrs.push_back(attr);
    if(sai_macsec_api->create_macsec(
        &macsec_obj.first->second.m_ingress_id,
        switch_id,
        static_cast<uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot initialize MACsec ingress object at the switch %lu", switch_id);
        sai_macsec_api->remove_macsec_port(macsec_obj.first->second.m_egress_id);
        m_macsec_objs.erase(macsec_obj.first);
        return m_macsec_objs.end();
    }

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

    if(sai_macsec_api->remove_macsec(
        macsec_obj->second.m_egress_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot deinitialize MACsec egress object at the switch %lu", macsec_obj->first);
        result &= false;
    }

    if(sai_macsec_api->remove_macsec(
        macsec_obj->second.m_ingress_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot deinitialize MACsec ingress object at the switch %lu", macsec_obj->first);
        result &= false;
    }

    m_macsec_objs.erase(macsec_obj);
    return result;
}

std::shared_ptr<MACsecOrch::MACsecPort> MACsecOrch::createMACsecPort(
    const Port & port,
    sai_object_id_t switch_id)
{
    SWSS_LOG_ENTER();

    auto macsec_port = m_macsec_ports.emplace(port.m_alias, std::make_shared<MACsecPort>());
    if (!macsec_port.second)
    {
        SWSS_LOG_INFO("The MACsec port has been initialized at the port %s", port.m_alias.c_str());
        return macsec_port.first->second;
    }

    sai_attribute_t attr;
    std::vector<sai_attribute_t> attrs;

    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_EGRESS;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_PORT_ATTR_PORT_ID;
    attr.value.oid = port.m_line_port_id;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_port(
        &macsec_port.first->second->m_egress_port_id,
        switch_id,
        static_cast<uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec egress port at the port %s", port.m_alias.c_str());
        m_macsec_ports.erase(macsec_port.first);
        return nullptr;
    }

    attrs.clear();
    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_INGRESS;
    attrs.push_back(attr);
    attr.id = SAI_MACSEC_PORT_ATTR_PORT_ID;
    attr.value.oid = port.m_line_port_id;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_port(
        &macsec_port.first->second->m_ingress_port_id,
        switch_id,
        static_cast<uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec ingress port at the port %s", port.m_alias.c_str());
        sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_egress_port_id);
        m_macsec_ports.erase(macsec_port.first);
        return nullptr;
    }

    attrs.clear();
    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_EGRESS;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_flow(
        &macsec_port.first->second->m_egress_flow_id,
        switch_id,
        static_cast<uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec egress flow at the port %s", port.m_alias.c_str());
        sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_egress_port_id);
        sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_ingress_port_id);
        m_macsec_ports.erase(macsec_port.first);
        return nullptr;
    }

    attrs.clear();
    attr.id = SAI_MACSEC_SA_ATTR_MACSEC_DIRECTION;
    attr.value.s32 = SAI_MACSEC_DIRECTION_INGRESS;
    attrs.push_back(attr);
    if (sai_macsec_api->create_macsec_flow(
        &macsec_port.first->second->m_ingress_flow_id,
        switch_id,
        static_cast<uint32_t>(attrs.size()),
        attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot create MACsec ingress flow at the port %s", port.m_alias.c_str());
        sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_egress_port_id);
        sai_macsec_api->remove_macsec_port(macsec_port.first->second->m_ingress_port_id);
        sai_macsec_api->remove_macsec_flow(macsec_port.first->second->m_ingress_flow_id);
        m_macsec_ports.erase(macsec_port.first);
        return nullptr;
    }

    return macsec_port.first->second;
}

bool MACsecOrch::deleteMACsecPort(const std::string & port_name)
{
    SWSS_LOG_ENTER();

    auto macsec_port = m_macsec_ports.find(port_name);
    if (macsec_port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port wasn't initialized at the port %s", port_name.c_str());
        return true;
    }

    bool result = true;

    if(sai_macsec_api->remove_macsec_port(
        macsec_port->second->m_egress_port_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec egress port at the port %s", port_name.c_str());
        result &= false;
    }

    if(sai_macsec_api->remove_macsec_port(
        macsec_port->second->m_ingress_port_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec ingress port at the port %s", port_name.c_str());
        result &= false;
    }

    if(sai_macsec_api->remove_macsec_flow(
        macsec_port->second->m_egress_flow_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec egress flow at the port %s", port_name.c_str());
        result &= false;
    }

    if(sai_macsec_api->remove_macsec_flow(
        macsec_port->second->m_ingress_flow_id) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Cannot delete MACsec ingress flow at the port %s", port_name.c_str());
        result &= false;
    }

    m_macsec_ports.erase(macsec_port);
    return true;
}

task_process_status MACsecOrch::updateMACsecSC(
    const std::string & port_sci,
    const TaskArgs & sc_attr,
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

    auto port = m_macsec_ports.find(port_name);
    if (port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_need_retry;
    }
    sai_object_id_t switch_id = 0;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto & scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS) 
        ?
        port->second->m_egress_scs
        :
        port->second->m_ingress_scs;
    auto sc = scs.emplace(sci, MACsecSC());
    // If SC has been created
    if (!sc.second)
    {
        if (direction == SAI_MACSEC_DIRECTION_EGRESS)
        {
            if (!get_value(sc_attr, "encoding_an", sc.first->second.m_encoding_an))
            {
                return task_failed;
            }
        }
        return task_success;
    }
    sai_uint32_t ssci;
    sc.first->second.m_xpn64_enable = false;
    if (get_value(sc_attr, "ssci", ssci))
    {
        sc.first->second.m_xpn64_enable = true;
    }
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        get_value(sc_attr, "encoding_an", sc.first->second.m_encoding_an);
    }
    sai_object_id_t flow_id=
        (direction == SAI_MACSEC_DIRECTION_EGRESS) 
        ?
        port->second->m_egress_flow_id
        :
        port->second->m_ingress_flow_id;
    if (!createMACsecSC(
        sc.first->second.m_sc_id,
        switch_id,
        direction,
        flow_id,
        sci,
        ssci,
        sc.first->second.m_xpn64_enable))
    {
        SWSS_LOG_WARN("Create MACsec SC %s fail.", port_sci.c_str());
        return task_failed;
    }

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
    const std::string & port_sci,
    const TaskArgs & sc_attr,
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

    auto port = m_macsec_ports.find(port_name);
    if (port == m_macsec_ports.end())
    {
        SWSS_LOG_WARN("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto & scs =
        (direction == SAI_MACSEC_DIRECTION_EGRESS) 
        ?
        port->second->m_egress_scs
        :
        port->second->m_ingress_scs;
    auto sc = scs.find(sci);
    if (sc == scs.end())
    {
        SWSS_LOG_WARN("The MACsec SC %s wasn't created", port_sci.c_str());
        return task_failed;
    }
    auto ret = task_success;
    if (!deleteMACsecSC(sc->second.m_sc_id))
    {
        SWSS_LOG_WARN("The MACsec SC %s cannot be deleted", port_sci.c_str());
        ret = task_failed;
    }
    port->second->m_egress_scs.erase(sc);
    SWSS_LOG_NOTICE("MACsec SC %s is deleted.", port_sci.c_str());

    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sc.del(join('|', port_name, sci));
    }
    else
    {
        m_state_macsec_ingress_sc.del(join('|', port_name, sci));
    }

    return ret;
}

bool MACsecOrch::createMACsecSC(
    sai_object_id_t & sc_id,
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
        attrs.data()) != SAI_STATUS_SUCCESS) {
        SWSS_LOG_WARN("Cannot create MACsec egress SC %lu", sci);
        return false;
    }
    return true;
}

bool MACsecOrch::deleteMACsecSC(sai_object_id_t sc_id)
{
    SWSS_LOG_ENTER();

    if (sai_macsec_api->remove_macsec_sc(
        sc_id) != SAI_STATUS_SUCCESS) {
        return false;
    }
    return true;
}

task_process_status MACsecOrch::createMACsecSA(
    const std::string & port_sci_an,
    const TaskArgs & sa_attr,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    sai_uint64_t sci;
    sai_uint8_t an;
    if (!split(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    auto port = m_macsec_ports.find(port_name);
    if (port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_need_retry;
    }
    sai_object_id_t switch_id = 0;
    if (!getGearboxSwitchId(port_name, switch_id))
    {
        SWSS_LOG_WARN("Cannot find gearbox at the port %s.", port_name.c_str());
        return task_failed;
    }

    auto & scs = 
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
        ?
        port->second->m_egress_scs
        :
        port->second->m_ingress_scs;
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
    catch(std::invalid_argument & e)
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

    if (!createMACsecSA(
        sc->second.m_sa_ids[an],
        switch_id,
        direction,
        sc->second.m_sc_id,
        an,
        port->second->m_enable_encrypt,
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

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", "ok");
    if (direction == SAI_MACSEC_DIRECTION_EGRESS)
    {
        m_state_macsec_egress_sa.set(join('|', port_name, sci, an), fvVector);
    }
    else
    {
        m_state_macsec_ingress_sa.set(join('|', port_name, sci, an), fvVector);
    }

    SWSS_LOG_NOTICE("MACsec SA %s is created.", port_sci_an.c_str());
    return task_success;
}

task_process_status MACsecOrch::deleteMACsecSA(
    const std::string & port_sci_an,
    const TaskArgs & sa_attr,
    sai_int32_t direction)
{
    SWSS_LOG_ENTER();

    std::string port_name;
    sai_uint64_t sci;
    sai_uint8_t an;
    if (!split(port_sci_an, ':', port_name, sci, an) || an > MAX_SA_NUMBER)
    {
        SWSS_LOG_WARN("The key %s isn't correct.", port_sci_an.c_str());
        return task_failed;
    }

    auto port = m_macsec_ports.find(port_name);
    if (port == m_macsec_ports.end())
    {
        SWSS_LOG_INFO("The MACsec port hasn't been created at the port %s.", port_name.c_str());
        return task_need_retry;
    }

    auto & scs = 
        (direction == SAI_MACSEC_DIRECTION_EGRESS)
        ?
        port->second->m_egress_scs
        :
        port->second->m_ingress_scs;
    auto sc = scs.find(sci);
    if (sc == scs.end())
    {
        SWSS_LOG_INFO("The MACsec SC %lu hasn't been created at the port %s.", sci, port_name.c_str());
        return task_need_retry;
    }

    if (sc->second.m_sa_ids[an] == 0)
    {
        SWSS_LOG_WARN("The MACsec SA %s hasn't been created.", port_sci_an.c_str());
        return task_failed;
    }
    if (!deleteMACsecSA(sc->second.m_sa_ids[an]))
    {
        SWSS_LOG_WARN("Cannot delete the MACsec SA %s.", port_sci_an.c_str());
    }

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
        sai_object_id_t & sa_id,
        sai_object_id_t switch_id,
        sai_int32_t direction,
        sai_object_id_t sc_id,
        sai_uint8_t an,
        bool encryption_enable,
        bool sak_256_bit,
        sai_macsec_sak_t sak,
        bool xpn64_enable,
        sai_macsec_salt_t salt,
        sai_macsec_auth_key_t auth_key,
        sai_uint64_t pn
        )
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
    attr.value.u8 = an;
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
