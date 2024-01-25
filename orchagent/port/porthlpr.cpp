// includes -----------------------------------------------------------------------------------------------------------

#include <cstdbool>
#include <cstdint>

#include <unordered_map>
#include <string>
#include <exception>

#include <boost/algorithm/string.hpp>

#include "portschema.h"
#include "converter.h"
#include "tokenize.h"
#include "logger.h"

#include "porthlpr.h"

using namespace swss;

// types --------------------------------------------------------------------------------------------------------------

typedef decltype(PortConfig::serdes) PortSerdes_t;

// constants ----------------------------------------------------------------------------------------------------------

static const std::uint32_t minPortSpeed = 1;
static const std::uint32_t maxPortSpeed = 800000;

static const std::uint32_t minPortMtu = 68;
static const std::uint32_t maxPortMtu = 9216;

static const std::unordered_map<std::string, bool> portModeMap =
{
    { PORT_MODE_ON,  true  },
    { PORT_MODE_OFF, false }
};

static const std::unordered_map<std::string, bool> portStatusMap =
{
    { PORT_STATUS_UP,   true  },
    { PORT_STATUS_DOWN, false }
};

static const std::unordered_map<std::string, sai_port_interface_type_t> portInterfaceTypeMap =
{
    { PORT_INTERFACE_TYPE_NONE,  SAI_PORT_INTERFACE_TYPE_NONE  },
    { PORT_INTERFACE_TYPE_CR,    SAI_PORT_INTERFACE_TYPE_CR    },
    { PORT_INTERFACE_TYPE_CR2,   SAI_PORT_INTERFACE_TYPE_CR2   },
    { PORT_INTERFACE_TYPE_CR4,   SAI_PORT_INTERFACE_TYPE_CR4   },
    { PORT_INTERFACE_TYPE_CR8,   SAI_PORT_INTERFACE_TYPE_CR8   },
    { PORT_INTERFACE_TYPE_SR,    SAI_PORT_INTERFACE_TYPE_SR    },
    { PORT_INTERFACE_TYPE_SR2,   SAI_PORT_INTERFACE_TYPE_SR2   },
    { PORT_INTERFACE_TYPE_SR4,   SAI_PORT_INTERFACE_TYPE_SR4   },
    { PORT_INTERFACE_TYPE_SR8,   SAI_PORT_INTERFACE_TYPE_SR8   },
    { PORT_INTERFACE_TYPE_LR,    SAI_PORT_INTERFACE_TYPE_LR    },
    { PORT_INTERFACE_TYPE_LR4,   SAI_PORT_INTERFACE_TYPE_LR4   },
    { PORT_INTERFACE_TYPE_LR8,   SAI_PORT_INTERFACE_TYPE_LR8   },
    { PORT_INTERFACE_TYPE_KR,    SAI_PORT_INTERFACE_TYPE_KR    },
    { PORT_INTERFACE_TYPE_KR4,   SAI_PORT_INTERFACE_TYPE_KR4   },
    { PORT_INTERFACE_TYPE_KR8,   SAI_PORT_INTERFACE_TYPE_KR8   },
    { PORT_INTERFACE_TYPE_CAUI,  SAI_PORT_INTERFACE_TYPE_CAUI  },
    { PORT_INTERFACE_TYPE_GMII,  SAI_PORT_INTERFACE_TYPE_GMII  },
    { PORT_INTERFACE_TYPE_SFI,   SAI_PORT_INTERFACE_TYPE_SFI   },
    { PORT_INTERFACE_TYPE_XLAUI, SAI_PORT_INTERFACE_TYPE_XLAUI },
    { PORT_INTERFACE_TYPE_KR2,   SAI_PORT_INTERFACE_TYPE_KR2   },
    { PORT_INTERFACE_TYPE_CAUI4, SAI_PORT_INTERFACE_TYPE_CAUI4 },
    { PORT_INTERFACE_TYPE_XAUI,  SAI_PORT_INTERFACE_TYPE_XAUI  },
    { PORT_INTERFACE_TYPE_XFI,   SAI_PORT_INTERFACE_TYPE_XFI   },
    { PORT_INTERFACE_TYPE_XGMII, SAI_PORT_INTERFACE_TYPE_XGMII }
};

static const std::unordered_map<std::string, sai_port_fec_mode_t> portFecMap =
{
    { PORT_FEC_NONE, SAI_PORT_FEC_MODE_NONE },
    { PORT_FEC_RS,   SAI_PORT_FEC_MODE_RS   },
    { PORT_FEC_FC,   SAI_PORT_FEC_MODE_FC   },
    { PORT_FEC_AUTO, SAI_PORT_FEC_MODE_NONE }
};

static const std::unordered_map<sai_port_fec_mode_t, std::string> portFecRevMap =
{
    { SAI_PORT_FEC_MODE_NONE, PORT_FEC_NONE },
    { SAI_PORT_FEC_MODE_RS,   PORT_FEC_RS   },
    { SAI_PORT_FEC_MODE_FC,   PORT_FEC_FC   }
};

static const std::unordered_map<std::string, bool> portFecOverrideMap =
{
    { PORT_FEC_NONE, true  },
    { PORT_FEC_RS,   true  },
    { PORT_FEC_FC,   true  },
    { PORT_FEC_AUTO, false }
};

static const std::unordered_map<std::string, sai_port_priority_flow_control_mode_t> portPfcAsymMap =
{
    { PORT_MODE_ON,  SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE },
    { PORT_MODE_OFF, SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED }
};

static const std::unordered_map<std::string, sai_bridge_port_fdb_learning_mode_t> portLearnModeMap =
{
    { PORT_LEARN_MODE_DROP,         SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DROP             },
    { PORT_LEARN_MODE_DISABLE,      SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE          },
    { PORT_LEARN_MODE_HARDWARE,     SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW               },
    { PORT_LEARN_MODE_CPU_TRAP,     SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_TRAP         },
    { PORT_LEARN_MODE_CPU_LOG,      SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_LOG          },
    { PORT_LEARN_MODE_NOTIFICATION, SAI_BRIDGE_PORT_FDB_LEARNING_MODE_FDB_NOTIFICATION }
};

static const std::unordered_map<std::string, Port::Role> portRoleMap =
{
    { PORT_ROLE_EXT, Port::Role::Ext },
    { PORT_ROLE_INT, Port::Role::Int },
    { PORT_ROLE_INB, Port::Role::Inb },
    { PORT_ROLE_REC, Port::Role::Rec }
};

// functions ----------------------------------------------------------------------------------------------------------

template<typename T>
static inline T toUInt(const std::string &hexStr)
{
    if (hexStr.substr(0, 2) != "0x")
    {
        throw std::invalid_argument("Invalid argument: '" + hexStr + "'");
    }

    return to_uint<T>(hexStr);
}

static inline std::uint16_t toUInt16(const std::string &hexStr)
{
    return toUInt<std::uint16_t>(hexStr);
}

static inline std::uint32_t toUInt32(const std::string &hexStr)
{
    return toUInt<std::uint32_t>(hexStr);
}

// Port helper --------------------------------------------------------------------------------------------------------

bool PortHelper::fecToStr(std::string &str, sai_port_fec_mode_t value) const
{
    const auto &cit = portFecRevMap.find(value);
    if (cit == portFecRevMap.cend())
    {
        return false;
    }

    str = cit->second;

    return true;
}

bool PortHelper::fecToSaiFecMode(const std::string &str, sai_port_fec_mode_t &value) const
{
    const auto &cit = portFecMap.find(str);
    if (cit == portFecMap.cend())
    {
        return false;
    }

    value = cit->second;

    return true;
}

bool PortHelper::fecIsOverrideRequired(const std::string &str) const
{
    const auto &cit = portFecMap.find(str);
    if (cit == portFecMap.cend())
    {
        return false;
    }

    return cit->second;

}
std::string PortHelper::getFieldValueStr(const PortConfig &port, const std::string &field) const
{
    static std::string str;

    const auto &cit = port.fieldValueMap.find(field);
    if (cit != port.fieldValueMap.cend())
    {
        return cit->second;
    }

    return str;
}

std::string PortHelper::getAutonegStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_AUTONEG);
}

std::string PortHelper::getPortInterfaceTypeStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_INTERFACE_TYPE);
}

std::string PortHelper::getAdvInterfaceTypesStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_ADV_INTERFACE_TYPES);
}

std::string PortHelper::getFecStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_FEC);
}

std::string PortHelper::getPfcAsymStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_PFC_ASYM);
}

std::string PortHelper::getLearnModeStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_LEARN_MODE);
}

std::string PortHelper::getLinkTrainingStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_LINK_TRAINING);
}

std::string PortHelper::getAdminStatusStr(const PortConfig &port) const
{
    return this->getFieldValueStr(port, PORT_ADMIN_STATUS);
}

bool PortHelper::parsePortAlias(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty string is prohibited", field.c_str());
        return false;
    }

    port.alias.value = value;
    port.alias.is_set = true;

    return true;
}

bool PortHelper::parsePortIndex(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        port.index.value = to_uint<std::uint16_t>(value);
        port.index.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PortHelper::parsePortLanes(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty string is prohibited", field.c_str());
        return false;
    }

    const auto &laneList = tokenize(value, ',');

    try
    {
        for (const auto &cit : laneList)
        {
            port.lanes.value.insert(to_uint<std::uint32_t>(cit));
        }

        port.lanes.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    if (port.lanes.value.size() != laneList.size())
    {
        SWSS_LOG_WARN("Duplicate lanes in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
    }

    return true;
}

bool PortHelper::parsePortSpeed(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        port.speed.value = to_uint<std::uint32_t>(value);
        port.speed.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    if (!((minPortSpeed <= port.speed.value) && (port.speed.value <= maxPortSpeed)))
    {
        SWSS_LOG_ERROR(
            "Failed to parse field(%s): value(%s) is out of range: %u <= speed <= %u",
            field.c_str(), value.c_str(), minPortSpeed, maxPortSpeed
        );
        return false;
    }

    return true;
}

bool PortHelper::parsePortAutoneg(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portModeMap.find(value);
    if (cit == portModeMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.autoneg.value = cit->second;
    port.autoneg.is_set = true;

    return true;
}

bool PortHelper::parsePortAdvSpeeds(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    auto nValue = boost::algorithm::to_lower_copy(value);

    if (nValue == PORT_ADV_ALL)
    {
        port.adv_speeds.is_set = true;
        return true;
    }

    const auto &speedList = tokenize(nValue, ',');

    try
    {
        for (const auto &cit : speedList)
        {
            auto speed = to_uint<std::uint32_t>(cit);

            if (!((minPortSpeed <= speed) && (speed <= maxPortSpeed)))
            {
                SWSS_LOG_ERROR(
                    "Failed to parse field(%s): value(%s) is out of range: %u <= speed <= %u",
                    field.c_str(), value.c_str(), minPortSpeed, maxPortSpeed
                );
                return false;
            }

            port.adv_speeds.value.insert(speed);
        }

        port.adv_speeds.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    if (port.adv_speeds.value.size() != speedList.size())
    {
        SWSS_LOG_WARN("Duplicate speeds in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
    }

    return true;
}

bool PortHelper::parsePortInterfaceType(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    auto nValue = boost::algorithm::to_lower_copy(value);

    const auto &cit = portInterfaceTypeMap.find(nValue);
    if (cit == portInterfaceTypeMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.interface_type.value = cit->second;
    port.interface_type.is_set = true;

    return true;
}

bool PortHelper::parsePortAdvInterfaceTypes(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    auto nValue = boost::algorithm::to_lower_copy(value);

    if (nValue == PORT_ADV_ALL)
    {
        port.adv_interface_types.is_set = true;
        return true;
    }

    const auto &intfTypeList = tokenize(nValue, ',');

    for (const auto &cit1 : intfTypeList)
    {
        const auto &cit2 = portInterfaceTypeMap.find(cit1);
        if (cit2 == portInterfaceTypeMap.cend())
        {
            SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
            return false;
        }

        port.adv_interface_types.value.insert(cit2->second);
    }

    port.adv_interface_types.is_set = true;

    if (port.adv_interface_types.value.size() != intfTypeList.size())
    {
        SWSS_LOG_WARN("Duplicate interface types in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
    }

    return true;
}

bool PortHelper::parsePortFec(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portFecMap.find(value);
    if (cit == portFecMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    const auto &override_cit = portFecOverrideMap.find(value);
    if (override_cit == portFecOverrideMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s) in override map", field.c_str(), value.c_str());
        return false;
    }

    port.fec.value = cit->second;
    port.fec.is_set = true;
    port.fec.override_fec =override_cit->second;

    return true;
}

bool PortHelper::parsePortMtu(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        port.mtu.value = to_uint<std::uint32_t>(value);
        port.mtu.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    if (!((minPortMtu <= port.mtu.value) && (port.mtu.value <= maxPortMtu)))
    {
        SWSS_LOG_ERROR(
            "Failed to parse field(%s): value(%s) is out of range: %u <= mtu <= %u",
            field.c_str(), value.c_str(), minPortMtu, maxPortMtu
        );
        return false;
    }

    return true;
}

bool PortHelper::parsePortTpid(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        port.tpid.value = toUInt16(value);
        port.tpid.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PortHelper::parsePortPfcAsym(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portPfcAsymMap.find(value);
    if (cit == portPfcAsymMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.pfc_asym.value = cit->second;
    port.pfc_asym.is_set = true;

    return true;
}

bool PortHelper::parsePortLearnMode(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portLearnModeMap.find(value);
    if (cit == portLearnModeMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.learn_mode.value = cit->second;
    port.learn_mode.is_set = true;

    return true;
}

bool PortHelper::parsePortLinkTraining(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portModeMap.find(value);
    if (cit == portModeMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.link_training.value = cit->second;
    port.link_training.is_set = true;

    return true;
}

template<typename T>
bool PortHelper::parsePortSerdes(T &serdes, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty string is prohibited", field.c_str());
        return false;
    }

    const auto &serdesList = tokenize(value, ',');

    try
    {
        for (const auto &cit : serdesList)
        {
            serdes.value.push_back(toUInt32(cit));
        }
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    serdes.is_set = true;

    return true;
}

template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::preemphasis) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::idriver) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::ipredriver) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::pre1) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::pre2) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::pre3) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::main) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::post1) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::post2) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::post3) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::attn) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::ob_m2lp) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::ob_alev_out) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::obplev) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::obnlev) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::regn_bfm1p) &serdes, const std::string &field, const std::string &value) const;
template bool PortHelper::parsePortSerdes(decltype(PortSerdes_t::regn_bfm1n) &serdes, const std::string &field, const std::string &value) const;



bool PortHelper::parsePortRole(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portRoleMap.find(value);
    if (cit == portRoleMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.role.value = cit->second;
    port.role.is_set = true;

    return true;
}

bool PortHelper::parsePortAdminStatus(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    const auto &cit = portStatusMap.find(value);
    if (cit == portStatusMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    port.admin_status.value = cit->second;
    port.admin_status.is_set = true;

    return true;
}

bool PortHelper::parsePortDescription(PortConfig &port, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    port.description.value = value;
    port.description.is_set = true;

    return true;
}

bool PortHelper::parsePortConfig(PortConfig &port) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : port.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == PORT_ALIAS)
        {
            if (!this->parsePortAlias(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_INDEX)
        {
            if (!this->parsePortIndex(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_LANES)
        {
            if (!this->parsePortLanes(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_SPEED)
        {
            if (!this->parsePortSpeed(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_AUTONEG)
        {
            if (!this->parsePortAutoneg(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_ADV_SPEEDS)
        {
            if (!this->parsePortAdvSpeeds(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_INTERFACE_TYPE)
        {
            if (!this->parsePortInterfaceType(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_ADV_INTERFACE_TYPES)
        {
            if (!this->parsePortAdvInterfaceTypes(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_FEC)
        {
            if (!this->parsePortFec(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_MTU)
        {
            if (!this->parsePortMtu(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_TPID)
        {
            if (!this->parsePortTpid(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_PFC_ASYM)
        {
            if (!this->parsePortPfcAsym(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_LEARN_MODE)
        {
            if (!this->parsePortLearnMode(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_LINK_TRAINING)
        {
            if (!this->parsePortLinkTraining(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_PREEMPHASIS)
        {
            if (!this->parsePortSerdes(port.serdes.preemphasis, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_IDRIVER)
        {
            if (!this->parsePortSerdes(port.serdes.idriver, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_IPREDRIVER)
        {
            if (!this->parsePortSerdes(port.serdes.ipredriver, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_PRE1)
        {
            if (!this->parsePortSerdes(port.serdes.pre1, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_PRE2)
        {
            if (!this->parsePortSerdes(port.serdes.pre2, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_PRE3)
        {
            if (!this->parsePortSerdes(port.serdes.pre3, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_MAIN)
        {
            if (!this->parsePortSerdes(port.serdes.main, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_POST1)
        {
            if (!this->parsePortSerdes(port.serdes.post1, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_POST2)
        {
            if (!this->parsePortSerdes(port.serdes.post2, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_POST3)
        {
            if (!this->parsePortSerdes(port.serdes.post3, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_ATTN)
        {
            if (!this->parsePortSerdes(port.serdes.attn, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_OB_M2LP)
        {
            if (!this->parsePortSerdes(port.serdes.ob_m2lp, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_OB_ALEV_OUT)
        {
            if (!this->parsePortSerdes(port.serdes.ob_alev_out, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_OBPLEV)
        {
            if (!this->parsePortSerdes(port.serdes.obplev, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_OBNLEV)
        {
            if (!this->parsePortSerdes(port.serdes.obnlev, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_REGN_BFM1P)
        {
            if (!this->parsePortSerdes(port.serdes.regn_bfm1p, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_REGN_BFM1N)
        {
            if (!this->parsePortSerdes(port.serdes.regn_bfm1n, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_ROLE)
        {
            if (!this->parsePortRole(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_ADMIN_STATUS)
        {
            if (!this->parsePortAdminStatus(port, field, value))
            {
                return false;
            }
        }
        else if (field == PORT_DESCRIPTION)
        {
            if (!this->parsePortDescription(port, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return this->validatePortConfig(port);
}

bool PortHelper::validatePortConfig(PortConfig &port) const
{
    SWSS_LOG_ENTER();

    if (!port.lanes.is_set)
    {
        SWSS_LOG_WARN("Validation error: missing mandatory field(%s)", PORT_LANES);
        return false;
    }

    if (!port.speed.is_set)
    {
        SWSS_LOG_WARN("Validation error: missing mandatory field(%s)", PORT_SPEED);
        return false;
    }

    if (!port.admin_status.is_set)
    {
        SWSS_LOG_INFO(
            "Missing non mandatory field(%s): setting default value(%s)",
            PORT_ADMIN_STATUS,
            PORT_STATUS_DOWN
        );

        port.admin_status.value = false;
        port.admin_status.is_set = true;

        port.fieldValueMap[PORT_ADMIN_STATUS] = PORT_STATUS_DOWN;
    }

    return true;
}
