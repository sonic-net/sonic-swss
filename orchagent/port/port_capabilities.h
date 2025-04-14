#pragma once

extern "C" {
#include <saiobject.h>
#include <saitypes.h>
#include <saiport.h>
#include <saiqueue.h>
}

class PortCapabilities final
{
public:
    PortCapabilities();
    ~PortCapabilities() = default;

    bool isPortPfcAsymSupported() const;

private:
    sai_status_t queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const;

    template<typename T>
    void queryPortAttrCapabilities(T &obj, sai_port_attr_t attrId);

    // Port SAI capabilities
    struct {
        struct {
            sai_attr_capability_t attrCap = { false, false, false };
        } pfcRx; // SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX

        struct {
            sai_attr_capability_t attrCap = { false, false, false };
        } pfcTx; // SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX

        struct {
            sai_attr_capability_t attrCap = { false, false, false };
        } pfc; // SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL

        struct {
            sai_attr_capability_t attrCap = { false, false, false };
        } pfcMode; // SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE
    } portCapabilities;
};
