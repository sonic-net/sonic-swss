#include <cassert>
#include <fstream>
#include <sstream>
#include <map>
#include <net/if.h>
#include <inttypes.h>

#include "sai_serialize.h"
#include "logger.h"
#include "tokenize.h"
#include "subscriberstatetable.h"
#include "vrrporch.h"

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;

extern sai_router_interface_api_t*  sai_router_intfs_api;

VrrpOrch::VrrpOrch(DBConnector *appdb, string tableName, VRFOrch *vrf_orch, PortsOrch *port_orch):
        Orch(appdb, tableName), m_vrfOrch(vrf_orch), m_portsOrch(port_orch)
{

}

void VrrpOrch::update(SubjectType, void *)
{

}

void VrrpOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        vector<string> keys = tokenize(kfvKey(t), delimiter);

        string alias(keys[0]);
        string vrid_str(keys[1]);
        string ip_type(keys[2]);

        VrrpIntf vrrp(alias, vrid_str, ip_type);
        SWSS_LOG_INFO("key %s, alias %s, vrid %s, ip_type %s", kfvKey(t).c_str(), alias.c_str(), vrid_str.c_str(), ip_type.c_str());
        SWSS_LOG_INFO("vrrp, alias %s, vrid %d, ip_type %d", vrrp.getParentName().c_str(), vrrp.getVrid(), vrrp.isIpv4());

        const vector<FieldValueTuple>& data = kfvFieldsValues(t);

        MacAddress vmac;
        bool state = false;

        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "vmac")
            {
                try
                {
                    vmac = MacAddress(value);
                }
                catch (const std::invalid_argument &e)
                {
                    SWSS_LOG_ERROR("Invalid vmac argument %s to %s()", value.c_str(), e.what());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }
            else if (field == "state")
            {
                state = value == "up" ? true : false;
            }
        }

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            Port port;
            if (!m_portsOrch->getPort(alias, port))
            {
                it++;
                continue;
            }

            VrrpAppl vrrp_appl{vrrp, vmac, state};
            m_syncdVrrps[vrrp.getParentName()][vrrp.getVrrpName()] = vrrp_appl;

            if (!setVrrpIntf(port, vrrp, vmac, state))
            {
                it++;
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            Port port;
            /* Cannot locate interface */
            if (!m_portsOrch->getPort(alias, port))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (m_syncdVrrps.find(vrrp.getParentName()) != m_syncdVrrps.end())
            {
                m_syncdVrrps[vrrp.getParentName()].erase(vrrp.getVrrpName());
            }

            if (!removeVrrpIntf(port, vrrp))
            {
                it++;
                continue;
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}

bool VrrpOrch::setVrrpIntf(const Port &port, const VrrpIntf &vrrp, const MacAddress &vmac, const bool state)
{
    SWSS_LOG_ENTER();

    sai_object_id_t vrif_id = 0;
    if (m_vrifs.find(vrrp.getVrrpName()) == m_vrifs.end())
    {
        /* add vrif */
        if (port.m_rif_id <= 0 || port.m_vr_id <= 0)
        {
            SWSS_LOG_WARN("Create Virtual Router interface must on Router interface: %s", port.m_alias.c_str());
            return false;
        }

        if (state)
        {
            if (!addVirtualRouterIntf(port, vmac, vrif_id))
            {
                return false;
            }
            m_vrfOrch->increaseVrfRefCount(port.m_vr_id);
        }
        m_vrifs[vrrp.getVrrpName()] = vrif_id;
        SWSS_LOG_NOTICE("Create Virtual Router interface on %s, vrid: %d, state: %d, is ipv4: %d",
                        port.m_alias.c_str(), vrrp.getVrid(), state, vrrp.isIpv4());
    }
    else
    {
        vrif_id = m_vrifs[vrrp.getVrrpName()];
        if (state && vrif_id == 0)
        {
            if (!addVirtualRouterIntf(port, vmac, vrif_id))
            {
                return false;
            }
            m_vrfOrch->increaseVrfRefCount(port.m_vr_id);
        }
        else if (!state && vrif_id != 0)
        {
            if (!removeVirtualRouterIntf(port, vrif_id))
            {
                return false;
            }
            vrif_id = 0;
            m_vrfOrch->decreaseVrfRefCount(port.m_vr_id);
        }
        m_vrifs[vrrp.getVrrpName()] = vrif_id;
        SWSS_LOG_NOTICE("Set Virtual Router interface on %s, vrid: %d, state: %d, is ipv4: %d",
                        port.m_alias.c_str(), vrrp.getVrid(), state, vrrp.isIpv4());
    }

    return true;
}

bool VrrpOrch::removeVrrpIntf(const Port &port, const VrrpIntf &vrrp)
{
    SWSS_LOG_ENTER();
    if (m_vrifs.find(vrrp.getVrrpName()) == m_vrifs.end())
    {
        SWSS_LOG_INFO("Not found vrrp on interface: %s with vrid %d ipv4 %d", port.m_alias.c_str(), vrrp.getVrid(), vrrp.isIpv4());
        return true;
    }

    if (m_vrifs[vrrp.getVrrpName()] != 0)
    {
        if (!removeVirtualRouterIntf(port, m_vrifs[vrrp.getVrrpName()]))
        {
            return false;
        }
        m_vrfOrch->decreaseVrfRefCount(port.m_vr_id);
    }
    m_vrifs.erase(vrrp.getVrrpName());
    SWSS_LOG_NOTICE("Delete vrrp on interface: %s with vrid %d ipv4 %d", port.m_alias.c_str(), vrrp.getVrid(), vrrp.isIpv4());
    return true;
}

bool VrrpOrch::addVirtualRouterIntf(const Port &port, const MacAddress &vmac, sai_object_id_t &vrif_id)
{
    SWSS_LOG_ENTER();

    /* Create router interface if the router interface doesn't exist */
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = port.m_vr_id;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, vmac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    switch (port.m_type)
    {
    case Port::PHY:
    case Port::SYSTEM:
        attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
        attrs.push_back(attr);

        attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        attr.value.oid = port.m_port_id;
        attrs.push_back(attr);
        break;
    case Port::LAG:
        attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
        attrs.push_back(attr);

        attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        attr.value.oid = port.m_lag_id;
        attrs.push_back(attr);
        break;
    case Port::VLAN:
        attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
        attrs.push_back(attr);

        attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
        attr.value.oid = port.m_vlan_info.vlan_oid;
        attrs.push_back(attr);
        break;
    default:
        SWSS_LOG_ERROR("Create Virtual Router interface unsupported port type: %d", port.m_type);
        break;
    }

    attr.id = SAI_ROUTER_INTERFACE_ATTR_IS_VIRTUAL;
    attr.value.booldata = true;
    attrs.push_back(attr);

    sai_status_t status = sai_router_intfs_api->create_router_interface(&vrif_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create virtual router interface on %s, rv:%d", port.m_alias.c_str(), status);
        if (handleSaiCreateStatus(SAI_API_ROUTER_INTERFACE, status) != task_success)
        {
            throw runtime_error("Failed to create virtual router interface.");
        }
    }

    SWSS_LOG_NOTICE("Create virtual router interface on %s, mac: %s", port.m_alias.c_str(), vmac.to_string().c_str());
    return true;
}

bool VrrpOrch::removeVirtualRouterIntf(const Port &port, const sai_object_id_t vrif_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_router_intfs_api->remove_router_interface(vrif_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove virtual router interface on %s, rv:%d", port.m_alias.c_str(), status);
        if (handleSaiRemoveStatus(SAI_API_ROUTER_INTERFACE, status) != task_success)
        {
            throw runtime_error("Failed to remove router interface.");
        }
    }

    SWSS_LOG_NOTICE("Remove virtual router interface on %s", port.m_alias.c_str());
    return true;
}
