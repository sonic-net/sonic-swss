/*
 * Runtime emulation of the INGRESS/EGRESS sample port attributes for mirrororch_ut.
 * See mirrororch_sample_port_sai_wrap.h for the rationale.
 */
#include "mirrororch_sample_port_sai_wrap.h"

#include "saiport.h"
#include "saistatus.h"
#include "saitypes.h"

#include <map>

extern sai_port_api_t *sai_port_api;

namespace
{
    sai_port_api_t *g_real_port_api = nullptr;
    sai_port_api_t g_wrap_port_api;

    // Per-port emulated value of SAI_PORT_ATTR_{INGRESS,EGRESS}_SAMPLEPACKET_ENABLE.
    std::map<sai_object_id_t, sai_object_id_t> g_ingress_samplepacket;
    std::map<sai_object_id_t, sai_object_id_t> g_egress_samplepacket;

    bool isIntercepted(sai_attr_id_t id)
    {
        return id == SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE
            || id == SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION
            || id == SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE
            || id == SAI_PORT_ATTR_EGRESS_SAMPLE_MIRROR_SESSION;
    }

    extern "C" sai_status_t wrap_set_port_attribute(
        sai_object_id_t port_id,
        const sai_attribute_t *attr)
    {
        if (attr == nullptr)
        {
            return SAI_STATUS_INVALID_PARAMETER;
        }

        if (attr->id == SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE
            || attr->id == SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE)
        {
            auto &store = (attr->id == SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE)
                ? g_ingress_samplepacket : g_egress_samplepacket;
            if (attr->value.oid == SAI_NULL_OBJECT_ID)
            {
                store.erase(port_id);
            }
            else
            {
                store[port_id] = attr->value.oid;
            }
            return SAI_STATUS_SUCCESS;
        }

        if (attr->id == SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION
            || attr->id == SAI_PORT_ATTR_EGRESS_SAMPLE_MIRROR_SESSION)
        {
            // Validate the call framing the production code uses (bind: a
            // single non-null session; unbind: an empty list) so a malformed
            // attribute is still caught, without depending on libsaivs.
            if (attr->value.objlist.count == 0)
            {
                return SAI_STATUS_SUCCESS;
            }
            if (attr->value.objlist.count == 1
                && attr->value.objlist.list != nullptr
                && attr->value.objlist.list[0] != SAI_NULL_OBJECT_ID)
            {
                return SAI_STATUS_SUCCESS;
            }
            return SAI_STATUS_INVALID_PARAMETER;
        }

        return g_real_port_api->set_port_attribute(port_id, attr);
    }

    extern "C" sai_status_t wrap_get_port_attribute(
        sai_object_id_t port_id,
        uint32_t attr_count,
        sai_attribute_t *attr_list)
    {
        if (attr_list != nullptr && attr_count == 1 && isIntercepted(attr_list[0].id))
        {
            if (attr_list[0].id == SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE
                || attr_list[0].id == SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE)
            {
                auto &store =
                    (attr_list[0].id == SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE)
                    ? g_ingress_samplepacket : g_egress_samplepacket;
                auto it = store.find(port_id);
                attr_list[0].value.oid =
                    (it != store.end()) ? it->second : SAI_NULL_OBJECT_ID;
                return SAI_STATUS_SUCCESS;
            }

            // {INGRESS,EGRESS}_SAMPLE_MIRROR_SESSION: report unbound.
            attr_list[0].value.objlist.count = 0;
            return SAI_STATUS_SUCCESS;
        }

        return g_real_port_api->get_port_attribute(port_id, attr_count, attr_list);
    }
}

namespace mirror_sample_port_wrap_ut
{
    void install()
    {
        if (sai_port_api == &g_wrap_port_api)
        {
            // Already wrapped (defensive against accidental nesting): keep the
            // existing emulated state instead of wiping it.
            return;
        }

        g_real_port_api = sai_port_api;
        g_wrap_port_api = *sai_port_api;
        g_wrap_port_api.set_port_attribute = wrap_set_port_attribute;
        g_wrap_port_api.get_port_attribute = wrap_get_port_attribute;

        g_ingress_samplepacket.clear();
        g_egress_samplepacket.clear();
        sai_port_api = &g_wrap_port_api;
    }

    void uninstall()
    {
        if (sai_port_api == &g_wrap_port_api && g_real_port_api != nullptr)
        {
            sai_port_api = g_real_port_api;
        }
        g_ingress_samplepacket.clear();
        g_egress_samplepacket.clear();
    }
}
