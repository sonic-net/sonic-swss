#include "poeorch.h"
#include "table.h"
#include "converter.h"
#include "schema.h"
#include "timer.h"

#include <map>

using namespace swss;

extern sai_object_id_t gPoeSwitchId;
extern sai_poe_api_t* sai_poe_api;

#define POE_PORT_UPD_INTERVAL 2

static std::map<std::string, sai_poe_device_limit_mode_t> poe_device_limit_mode = {
    {"port", SAI_POE_DEVICE_LIMIT_MODE_PORT},
    {"class", SAI_POE_DEVICE_LIMIT_MODE_CLASS},
};

static std::map<sai_poe_device_limit_mode_t, std::string> poe_device_limit_mode_to_str = {
    {SAI_POE_DEVICE_LIMIT_MODE_PORT, "port"},
    {SAI_POE_DEVICE_LIMIT_MODE_CLASS, "class"},
};

static std::map<sai_attr_id_t, std::string> poe_device_attr_to_field = {
    {SAI_POE_DEVICE_ATTR_HARDWARE_INFO, "hw_info"},
    {SAI_POE_DEVICE_ATTR_POE_PSE_LIST, "total_pse"},
    {SAI_POE_DEVICE_ATTR_POE_PORT_LIST, "total_ports"},
    {SAI_POE_DEVICE_ATTR_TOTAL_POWER, "total_pwr"},
    {SAI_POE_DEVICE_ATTR_POWER_CONSUMPTION, "pwr_consump"},
    {SAI_POE_DEVICE_ATTR_VERSION, "version"},
    {SAI_POE_DEVICE_ATTR_POWER_LIMIT_MODE, "pwr_limit_mode"},
};

static std::map<sai_poe_pse_status_t, std::string> poe_pse_status_to_str = {
    {SAI_POE_PSE_STATUS_TYPE_ACTIVE, "active"},
    {SAI_POE_PSE_STATUS_TYPE_FAIL, "fail"},
    {SAI_POE_PSE_STATUS_TYPE_NOT_PRESENT, "not present"},
};

static std::map<sai_attr_id_t, std::string> poe_pse_attr_to_field = {
    {SAI_POE_PSE_ATTR_SOFTWARE_VERSION, "sw_ver"},
    {SAI_POE_PSE_ATTR_HARDWARE_VERSION, "hw_ver"},
    {SAI_POE_PSE_ATTR_TEMPERATURE, "temperature"},
    {SAI_POE_PSE_ATTR_STATUS, "status"},
};

static std::map<std::string, sai_poe_port_power_priority_t> poe_port_power_priority = {
    {"low", SAI_POE_PORT_POWER_PRIORITY_TYPE_LOW},
    {"high", SAI_POE_PORT_POWER_PRIORITY_TYPE_HIGH},
    {"crit", SAI_POE_PORT_POWER_PRIORITY_TYPE_CRITICAL},
};

static std::map<sai_poe_port_power_priority_t, std::string> power_priority_to_str = {
    {SAI_POE_PORT_POWER_PRIORITY_TYPE_LOW, "low"},
    {SAI_POE_PORT_POWER_PRIORITY_TYPE_HIGH, "high"},
    {SAI_POE_PORT_POWER_PRIORITY_TYPE_CRITICAL, "crit"},
};

static std::map<std::string, bool> port_admin_state = {
    {"disable", false},
    {"enable", true},
};

static std::map<bool, std::string> port_admin_state_to_str = {
    {false, "disable"},
    {true, "enable"},
};

static std::map<sai_poe_port_standard_t, std::string> poe_standard_to_str = {
    {SAI_POE_PORT_STANDARD_TYPE_AF, "802.3af"},
    {SAI_POE_PORT_STANDARD_TYPE_AT, "802.3at"},
    {SAI_POE_PORT_STANDARD_TYPE_60W, "60w"},
    {SAI_POE_PORT_STANDARD_TYPE_BT_TYPE3, "802.3bt Type 3"},
    {SAI_POE_PORT_STANDARD_TYPE_BT_TYPE4, "802.3bt Type 4"},
};

static std::map<sai_poe_port_status_t, std::string> poe_port_status_to_str = {
    {SAI_POE_PORT_STATUS_TYPE_OFF, "off"},
    {SAI_POE_PORT_STATUS_TYPE_SEARCHING, "searching"},
    {SAI_POE_PORT_STATUS_TYPE_DELIVERING_POWER, "delivering"},
    {SAI_POE_PORT_STATUS_TYPE_FAULT, "fail"},
};

static std::map<sai_attr_id_t, std::string> poe_port_attr_to_field = {
    {SAI_POE_PORT_ATTR_FRONT_PANEL_ID, "fp_port"},
    {SAI_POE_PORT_ATTR_STANDARD, "protocol"},
    {SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE, "enabled"},
    {SAI_POE_PORT_ATTR_POWER_LIMIT, "pwr_limit"},
    {SAI_POE_PORT_ATTR_POWER_PRIORITY, "priority"},
    {SAI_POE_PORT_ATTR_CONSUMPTION, "pwr_consump"},
    {SAI_POE_PORT_ATTR_STATUS, "status"},
};

PoeOrch::PoeOrch(DBConnector *applDb, DBConnector *cfgDb, DBConnector *stateDb, const std::vector<std::string> &poeTables) :
        Orch(applDb, poeTables),
        m_cfgPoeTable(cfgDb, CFG_POE_TABLE_NAME),
        m_appPoeTable(applDb, "_POE_TABLE"),
        m_deviceStateTable(stateDb, STATE_POE_DEVICE_TABLE_NAME),
        m_pseStateTable(stateDb, STATE_POE_PSE_TABLE_NAME),
        m_portStateTable(stateDb, STATE_POE_PORT_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    if (!initPoe())
    {
        SWSS_LOG_NOTICE("Failed to init PoE");
        return;
    }

    auto timer = new SelectableTimer(timespec {.tv_sec = POE_PORT_UPD_INTERVAL, .tv_nsec = 0});
    Orch::addExecutor(new ExecutableTimer(timer, this, "POE_PORT_POLL"));
    timer->start();
    initDone = true;
}

bool PoeOrch::initPoe()
{
    PoeConfig poe(m_appPoeTable);
    if (!poe.isPoeEnabled())
    {
        SWSS_LOG_WARN("poe not enabled");
        return false;
    }

    poe.loadConfig();
    m_poeDeviceMap = poe.getDeviceMap();
    m_poePortMap = poe.getPortMap();
    m_poePseMap = poe.getPseMap();

    for (auto &device : m_poeDeviceMap)
    {
        if (!initPoeDevice(gPoeSwitchId, device.second))
        {
            SWSS_LOG_ERROR("Failed to create poe device");
            return false;
        }
    }
    for (auto &pse : m_poePseMap)
    {
        sai_object_id_t devOid;
        sai_deserialize_object_id(m_poeDeviceMap[pse.second.deviceId].oid, devOid);
        if (!initPoePse(gPoeSwitchId, devOid, pse.second))
        {
            SWSS_LOG_ERROR("Failed to create poe pse");
            return false;
        }
    }
    for (auto &port : m_poePortMap)
    {
        sai_object_id_t devOid;
        sai_deserialize_object_id(m_poeDeviceMap[port.second.deviceId].oid, devOid);
        if (!initPoePort(gPoeSwitchId, devOid, port.second))
        {
            SWSS_LOG_ERROR("Failed to create poe port");
            return false;
        }
    }
    // Set state values that are defined at init and won't change later
    initStateTables();
    return true;
}

bool PoeOrch::initPoeDevice(const sai_object_id_t &switchOid, poe_device_t &dev)
{
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr = {};
    sai_object_id_t devOid;
    sai_status_t status;

    SWSS_LOG_ENTER();

    attr.id = SAI_POE_DEVICE_ATTR_HARDWARE_INFO;
    strncpy(attr.value.chardata, dev.hwInfo.c_str(), sizeof(attr.value.chardata) - 1);
    attrs.push_back(attr);

    if (!dev.powerLimitMode.empty())
    {
        attr.id = SAI_POE_DEVICE_ATTR_POWER_LIMIT_MODE;
        attr.value.u32 = poe_device_limit_mode.at(dev.powerLimitMode);
        attrs.push_back(attr);
    }

    status = sai_poe_api->create_poe_device(&devOid, switchOid, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("poe: Failed to create device %u", dev.id);
        return false;
    }

    dev.oid = sai_serialize_object_id(devOid);
    return true;
}

bool PoeOrch::initPoePse(const sai_object_id_t &switchOid, const sai_object_id_t &devOid, poe_pse_t &pse)
{
    std::vector<sai_attribute_t> attrs;
    sai_object_id_t pseOid;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    attr.id = SAI_POE_PSE_ATTR_ID;
    attr.value.u32 = pse.pseIndex;
    attrs.push_back(attr);

    attr.id = SAI_POE_PSE_ATTR_DEVICE_ID;
    attr.value.oid = devOid;
    attrs.push_back(attr);

    status = sai_poe_api->create_poe_pse(&pseOid, switchOid, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("poe: Failed to create pse %u", pse.pseIndex);
        return false;
    }

    pse.oid = sai_serialize_object_id(pseOid);
    return true;
}

bool PoeOrch::initPoePort(const sai_object_id_t &switchOid, const sai_object_id_t &devOid, poe_port_t &port)
{
    std::vector<sai_attribute_t> attrs;
    sai_object_id_t portOid;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    attr.id = SAI_POE_PORT_ATTR_FRONT_PANEL_ID;
    attr.value.u32 = port.frontPanelIndex;
    attrs.push_back(attr);

    attr.id = SAI_POE_PORT_ATTR_DEVICE_ID;
    attr.value.oid = devOid;
    attrs.push_back(attr);

    if (port.powerLimit)
    {
        attr.id = SAI_POE_PORT_ATTR_POWER_LIMIT;
        attr.value.u32 = port.powerLimit;
        attrs.push_back(attr);
    }

    if (!port.powerPriority.empty())
    {
        attr.id = SAI_POE_PORT_ATTR_POWER_PRIORITY;
        attr.value.u32 = poe_port_power_priority.at(port.powerPriority);
        attrs.push_back(attr);
    }

    if (port.enable)
    {
        attr.id = SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE;
        attr.value.booldata = port.enable;
        attrs.push_back(attr);
    }

    status = sai_poe_api->create_poe_port(&portOid, switchOid, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("poe: Failed to create port %u", port.frontPanelIndex);
        return false;
    }

    port.oid = sai_serialize_object_id(portOid);
    return true;
}

void PoeOrch::initStateTables()
{
    sai_attribute_t attr;
    sai_object_id_t oid;
    sai_status_t status;

    for (auto &dev : m_poeDeviceMap)
    {
        std::vector<sai_object_id_t> port_list(48);  /* typical port count */

        attr.id = SAI_POE_DEVICE_ATTR_POE_PORT_LIST;
        attr.value.objlist.count = (uint32_t)port_list.size();
        attr.value.objlist.list = port_list.data();
        sai_deserialize_object_id(dev.second.oid, oid);
        status = sai_poe_api->get_poe_device_attribute(oid, 1, &attr);
        if (SAI_STATUS_BUFFER_OVERFLOW == status)
        {
            port_list.resize(attr.value.objlist.count);
            attr.value.objlist.count = (uint32_t)port_list.size();
            attr.value.objlist.list = port_list.data();
            status = sai_poe_api->get_poe_device_attribute(oid, 1, &attr);
        }
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE device port count");
        }
        else
        {
            m_deviceStateTable.hset(std::to_string(dev.second.id), poe_device_attr_to_field[attr.id],
                                    std::to_string(attr.value.objlist.count));
        }

        attr.id = SAI_POE_DEVICE_ATTR_VERSION;
        attr.value = {};
        status = sai_poe_api->get_poe_device_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE device version");
        }
        else
        {
            m_deviceStateTable.hset(std::to_string(dev.second.id), poe_device_attr_to_field[attr.id],
                                    std::string(attr.value.chardata));
        }

        attr.id = SAI_POE_DEVICE_ATTR_HARDWARE_INFO;
        attr.value = {};
        status = sai_poe_api->get_poe_device_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE device hardware info");
        }
        else
        {
            m_deviceStateTable.hset(std::to_string(dev.second.id), poe_device_attr_to_field[attr.id],
                                    std::string(attr.value.chardata));
        }
    }
    for (auto &pse : m_poePseMap)
    {
        attr.id = SAI_POE_PSE_ATTR_SOFTWARE_VERSION;
        attr.value = {};
        sai_deserialize_object_id(pse.second.oid, oid);
        status = sai_poe_api->get_poe_pse_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE pse version");
        }
        else
        {
            m_pseStateTable.hset(std::to_string(pse.second.pseIndex), poe_pse_attr_to_field[attr.id],
                                 std::string(attr.value.chardata));
        }

        attr.id = SAI_POE_PSE_ATTR_HARDWARE_VERSION;
        attr.value = {};
        status = sai_poe_api->get_poe_pse_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE pse version");
        }
        else
        {
            m_pseStateTable.hset(std::to_string(pse.second.pseIndex), poe_pse_attr_to_field[attr.id],
                                 std::string(attr.value.chardata));
        }
    }
    for (auto &port : m_poePortMap)
    {
        attr.id = SAI_POE_PORT_ATTR_STANDARD;
        attr.value = {};
        sai_deserialize_object_id(port.second.oid, oid);
        status = sai_poe_api->get_poe_port_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE port standard");
        }
        else
        {
            std::string val = poe_standard_to_str[static_cast<sai_poe_port_standard_t>(attr.value.u32)];
            m_portStateTable.hset(port.second.interface, poe_port_attr_to_field[attr.id], val);
        }
        m_portStateTable.hset(port.second.interface, poe_port_attr_to_field[SAI_POE_PORT_ATTR_FRONT_PANEL_ID],
                              std::to_string(port.second.frontPanelIndex));

        /* write current configuration to cfgdb */
        attr.id = SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE;
        attr.value = {};
        status = sai_poe_api->get_poe_port_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE port state");
        }
        else
        {
            std::string val = port_admin_state_to_str[attr.value.booldata];
            m_cfgPoeTable.hset(port.second.interface, poe_port_attr_to_field[attr.id], val);
        }

        attr.id = SAI_POE_PORT_ATTR_POWER_LIMIT;
        attr.value = {};
        status = sai_poe_api->get_poe_port_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE port power limit");
        }
        else
        {
            std::string val = std::to_string(attr.value.u32);
            m_cfgPoeTable.hset(port.second.interface, poe_port_attr_to_field[attr.id], val);
        }

        attr.id = SAI_POE_PORT_ATTR_POWER_PRIORITY;
        attr.value = {};
        status = sai_poe_api->get_poe_port_attribute(oid, 1, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE port power priority");
        }
        else
        {
            std::string val = power_priority_to_str[static_cast<sai_poe_port_power_priority_t>(attr.value.u32)];
            m_cfgPoeTable.hset(port.second.interface, poe_port_attr_to_field[attr.id], val);
        }
    }
}

void PoeOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    if (!initDone)
    {
        SWSS_LOG_NOTICE("Consumer waiting for init to finish");
        return;
    }

    std::string table_name = consumer.getTableName();

    if (table_name != APP_POE_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        std::string key = kfvKey(t);
        std::string op = kfvOp(t);

        if (key.rfind("Ethernet", 0) != 0)
        {
            it = consumer.m_toSync.erase(it);
            SWSS_LOG_DEBUG("not ethernet OP: %s, key: %s", op.c_str(), key.c_str());
            continue;
        }
        SWSS_LOG_DEBUG("OP: %s, key: %s", op.c_str(), key.c_str());
        bool found = false;
        sai_object_id_t portOid;
        for (auto &port : m_poePortMap)
        {
            if (port.second.interface != key)
                continue;
            sai_deserialize_object_id(port.second.oid, portOid);
            found = true;
        }
        if (!found)
        {
            SWSS_LOG_ERROR("unknown interface %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            sai_attribute_t attr;
            // Scan all attributes
            for (auto itp : kfvFieldsValues(t))
            {
                std::string attr_name = fvField(itp);
                std::string attr_value = fvValue(itp);

                SWSS_LOG_DEBUG("TABLE ATTRIBUTE: %s : %s", attr_name.c_str(), attr_value.c_str());
                if (attr_name == poe_port_attr_to_field[SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE])
                {
                    attr.id = SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE;
                    attr.value.booldata = port_admin_state[attr_value];
                }
                else if (attr_name == poe_port_attr_to_field[SAI_POE_PORT_ATTR_POWER_LIMIT])
                {
                    attr.id = SAI_POE_PORT_ATTR_POWER_LIMIT;
                    attr.value.u32 = std::stoi(attr_value);
                }
                else if (attr_name == poe_port_attr_to_field[SAI_POE_PORT_ATTR_POWER_PRIORITY])
                {
                    attr.id = SAI_POE_PORT_ATTR_POWER_PRIORITY;
                    attr.value.u32 = poe_port_power_priority[attr_value];
                }
                else
                {
                    SWSS_LOG_ERROR("unknown field %s", attr_name.c_str());
                    continue;
                }
                sai_status_t status = sai_poe_api->set_poe_port_attribute(portOid, &attr);
                if (SAI_STATUS_SUCCESS != status)
                {
                    SWSS_LOG_ERROR("Failed to set PoE port attribute");
                }
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

static std::string milli_to_str(uint32_t val)
{
    std::string s(16, '\0');
    snprintf(&s[0], s.size(), "%.3f", ((double)val) / 1000);
    return s;
}

void PoeOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();
    if (!initDone)
    {
        SWSS_LOG_NOTICE("SelectableTimer waiting for init to finish");
        return;
    }

    for (auto &device : m_poeDeviceMap)
    {
        // fetch all attrs for device
        std::vector<sai_attribute_t> attrs = {
            {.id=SAI_POE_DEVICE_ATTR_TOTAL_POWER, .value={}},
            {.id=SAI_POE_DEVICE_ATTR_POWER_CONSUMPTION, .value={}},
            {.id=SAI_POE_DEVICE_ATTR_POWER_LIMIT_MODE, .value={}},
        };
        uint32_t idx = device.second.id;
        sai_object_id_t deviceOid;
        sai_deserialize_object_id(device.second.oid, deviceOid);
        sai_status_t status = sai_poe_api->get_poe_device_attribute(deviceOid, static_cast<uint32_t>(attrs.size()), attrs.data());
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE device status [%u]", idx);
            continue;
        }
        // convert results to strings
        for (auto &attr : attrs)
        {
            std::string val;
            switch (attr.id)
            {
            case SAI_POE_DEVICE_ATTR_TOTAL_POWER:
                val = std::to_string(attr.value.u16);
                break;
            case SAI_POE_DEVICE_ATTR_POWER_CONSUMPTION:
                val = milli_to_str(attr.value.u16);
                break;
            case SAI_POE_DEVICE_ATTR_POWER_LIMIT_MODE:
                val = poe_device_limit_mode_to_str[static_cast<sai_poe_device_limit_mode_t>(attr.value.u32)];
                break;
            default:
                SWSS_LOG_ERROR("Unknown attr id %u", attr.id);
                continue;  // go to next attr
            }
            // update state table
            m_deviceStateTable.hset(std::to_string(idx), poe_device_attr_to_field[attr.id], val);
        }
    }

    for (auto &pse : m_poePseMap)
    {
        // fetch all attrs for pse
        std::vector<sai_attribute_t> attrs = {
            {.id=SAI_POE_PSE_ATTR_TEMPERATURE, .value={}},
            {.id=SAI_POE_PSE_ATTR_STATUS, .value={}},
        };
        uint32_t idx = pse.second.pseIndex;
        sai_object_id_t pseOid;
        sai_deserialize_object_id(pse.second.oid, pseOid);
        sai_status_t status = sai_poe_api->get_poe_pse_attribute(pseOid, static_cast<uint32_t>(attrs.size()), attrs.data());
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE PSE status [%u]", idx);
            continue;
        }
        // convert results to strings
        for (auto &attr : attrs)
        {
            std::string val;
            switch (attr.id)
            {
            case SAI_POE_PSE_ATTR_TEMPERATURE:
                val = std::to_string(attr.value.u16);
                break;
            case SAI_POE_PSE_ATTR_STATUS:
                val = poe_pse_status_to_str[static_cast<sai_poe_pse_status_t>(attr.value.u32)];
                break;
            default:
                SWSS_LOG_ERROR("Unknown attr id %u", attr.id);
                continue;  // go to next attr
            }
            // update state table
            m_pseStateTable.hset(std::to_string(idx), poe_pse_attr_to_field[attr.id], val);
        }
    }

    for (auto &port : m_poePortMap)
    {
        // fetch all attrs for port
        std::vector<sai_attribute_t> attrs = {
            {.id=SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE, .value={}},
            {.id=SAI_POE_PORT_ATTR_POWER_LIMIT, .value={}},
            {.id=SAI_POE_PORT_ATTR_POWER_PRIORITY, .value={}},
            {.id=SAI_POE_PORT_ATTR_CONSUMPTION, .value={}},
            {.id=SAI_POE_PORT_ATTR_STATUS, .value={}},
        };
        sai_object_id_t portOid;
        sai_deserialize_object_id(port.second.oid, portOid);
        sai_status_t status = sai_poe_api->get_poe_port_attribute(portOid, static_cast<uint32_t>(attrs.size()), attrs.data());
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Failed to get PoE port status [%s]", port.second.interface.c_str());
            continue;
        }
        // convert results to strings
        for (auto &attr : attrs)
        {
            std::string val;
            switch (attr.id)
            {
            case SAI_POE_PORT_ATTR_POWER_PRIORITY:
                val = power_priority_to_str[static_cast<sai_poe_port_power_priority_t>(attr.value.u32)];
                break;
            case SAI_POE_PORT_ATTR_STATUS:
                val = poe_port_status_to_str[static_cast<sai_poe_port_status_t>(attr.value.u32)];
                break;
            case SAI_POE_PORT_ATTR_ADMIN_ENABLED_STATE:
                val = port_admin_state_to_str[attr.value.booldata];
                break;
            case SAI_POE_PORT_ATTR_POWER_LIMIT:
                val = std::to_string(attr.value.u32);
                break;
            case SAI_POE_PORT_ATTR_CONSUMPTION:
                val = std::to_string(attr.value.portpowerconsumption.assigned_class_a);
                m_portStateTable.hset(port.second.interface, "class_a", val);
                val = std::to_string(attr.value.portpowerconsumption.assigned_class_b);
                m_portStateTable.hset(port.second.interface, "class_b", val);
                val = milli_to_str(attr.value.portpowerconsumption.consumption);
                m_portStateTable.hset(port.second.interface, "pwr_consump", val);
                val = milli_to_str(attr.value.portpowerconsumption.voltage);
                m_portStateTable.hset(port.second.interface, "voltage", val);
                val = milli_to_str(attr.value.portpowerconsumption.current);
                m_portStateTable.hset(port.second.interface, "current", val);
                continue;  // go to next attr
            default:
                SWSS_LOG_ERROR("Unknown attr id %u", attr.id);
                continue;  // go to next attr
            }
            // update state table
            m_portStateTable.hset(port.second.interface, poe_port_attr_to_field[attr.id], val);
        }
    }
}
