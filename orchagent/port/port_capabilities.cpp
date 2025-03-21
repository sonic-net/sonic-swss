// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saistatus.h>
}

#include <string>

#include <sai_serialize.h>
#include <logger.h>

#include "orchagent/port/port_capabilities.h"

using namespace swss;

// variables ----------------------------------------------------------------------------------------------------------

extern sai_object_id_t gSwitchId;

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(sai_object_type_t objType, sai_attr_id_t attrId) noexcept
{
    const auto *meta = sai_metadata_get_attr_metadata(objType, attrId);

    return meta != nullptr ? meta->attridname : "UNKNOWN";
}

// Port capabilities --------------------------------------------------------------------------------------------------

PortCapabilities::PortCapabilities()
{
    queryPortAttrCapabilities(portCapabilities.pfc, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL);
    queryPortAttrCapabilities(portCapabilities.pfcTx, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX);
    queryPortAttrCapabilities(portCapabilities.pfcRx, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX);
    queryPortAttrCapabilities(portCapabilities.pfcMode, SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE);
}

bool PortCapabilities::isPortPfcAsymSupported() const
{
    auto supported = portCapabilities.pfcMode.attrCap.set_implemented;
    supported = supported && portCapabilities.pfc.attrCap.set_implemented;
    supported = supported && portCapabilities.pfcTx.attrCap.set_implemented;
    supported = supported && portCapabilities.pfcRx.attrCap.set_implemented;

    return supported;
}

sai_status_t PortCapabilities::queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const
{
    return sai_query_attribute_capability(gSwitchId, objType, attrId, &attrCap);
}

template<typename T>
void PortCapabilities::queryPortAttrCapabilities(T &obj, sai_port_attr_t attrId)
{
    SWSS_LOG_ENTER();

    auto status = queryAttrCapabilitiesSai(
        obj.attrCap, SAI_OBJECT_TYPE_PORT, attrId
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN(
            "Failed to get attribute(%s) capabilities",
            toStr(SAI_OBJECT_TYPE_PORT, attrId).c_str()
        );
        return;
    }

    if (!obj.attrCap.set_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) SET is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_PORT, attrId).c_str()
        );
    }
}
