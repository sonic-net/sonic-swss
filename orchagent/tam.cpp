/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <utility>

#include "logger.h"
#include "tokenize.h"
#include "orch.h"
#include "table.h"
#include "tam.h"
#include "portsorch.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "observer.h"

extern sai_tam_api_t* sai_tam_api;
extern sai_object_id_t  gSwitchId;

Tam::Tam() 
{
    SWSS_LOG_ENTER();
}

/* Create a TAM report object.
 *
 */
bool Tam::tamReportCreate(sai_tam_report_type_t report_type, sai_object_id_t *tam_report_obj)
{
   sai_attribute_t tam_report_attr;
   sai_status_t status;

   if (tam_report_obj == NULL)
   {
       return false;
   }

   /* Create a TAM report object. */
   tam_report_attr.id = SAI_TAM_REPORT_ATTR_TYPE;
   tam_report_attr.value.s32 = report_type;
   status =  sai_tam_api->create_tam_report(tam_report_obj, gSwitchId, 1, &tam_report_attr);
   if (status != SAI_STATUS_SUCCESS)
   {
       SWSS_LOG_ERROR("Unable to create telemetry report type object.");
       return false;
   }
   return true;
}

/* Delete a TAM report object.
 *
 */
bool Tam::tamReportDelete(sai_object_id_t tam_report_obj)
{
    sai_status_t status;
    /* Delete the TAM report object. */
    status = sai_tam_api->remove_tam_report(tam_report_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove TAM report type object.");
        return false;
    }
    return true;
}

/*
 * Create TAM event action object.
 */
bool Tam::tamEventActionCreate(sai_object_id_t tam_report_obj, sai_object_id_t *tam_event_action_obj)
{
    sai_attribute_t attr;
    sai_status_t status;

    if (tam_event_action_obj == NULL)
    {
        return false;
    }

    attr.id = SAI_TAM_EVENT_ACTION_ATTR_REPORT_TYPE;
    attr.value.oid = tam_report_obj;

    status = sai_tam_api->create_tam_event_action(tam_event_action_obj, gSwitchId,
                                                   1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM event action object.");
        return false;
    }
    return true;
}

/*
 * Delete TAM event action object.
 */
bool Tam::tamEventActionDelete(sai_object_id_t tam_event_action_obj)
{
    sai_status_t status;

    status = sai_tam_api->remove_tam_event_action(tam_event_action_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM event action object.");
        return false;
    }
    return true;
}

/*
 * Create TAM event object.
 */
bool Tam::tamEventCreate(sai_tam_event_type_t type, sai_object_id_t tam_event_action_obj,
                         sai_object_id_t tam_collector_obj, sai_object_id_t tam_thd_obj, 
                         sai_object_id_t *tam_event_obj)
{
    sai_attribute_t attr;
    sai_status_t status;
    vector<sai_attribute_t> tam_event_attrs;

    if (tam_event_obj == NULL)
    {
        return false;
    }

    attr.id = SAI_TAM_EVENT_ATTR_TYPE;
    attr.value.s32 = type;
    tam_event_attrs.push_back(attr);

    vector<sai_object_id_t> tam_event_list;
    tam_event_list.push_back(tam_event_action_obj);
    attr.id = SAI_TAM_EVENT_ATTR_ACTION_LIST;
    attr.value.objlist.count = (uint32_t)tam_event_list.size();
    attr.value.objlist.list = tam_event_list.data();
    tam_event_attrs.push_back(attr);

    vector<sai_object_id_t> tam_collector_list;
    tam_collector_list.push_back(tam_collector_obj);
    attr.id = SAI_TAM_EVENT_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = (uint32_t)tam_collector_list.size();;
    attr.value.objlist.list = tam_collector_list.data();
    tam_event_attrs.push_back(attr);
    
    attr.id = SAI_TAM_EVENT_ATTR_THRESHOLD;
    attr.value.oid = tam_thd_obj;
    tam_event_attrs.push_back(attr);

    status = sai_tam_api->create_tam_event(tam_event_obj, gSwitchId,
                                                   (uint32_t)tam_event_attrs.size(), 
                                                    tam_event_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM event object.");
        return false;
    }
    return true;
}

/*
 * Delete TAM event object.
 */
bool Tam::tamEventDelete(sai_object_id_t tam_event_obj)
{
    sai_status_t status;

    status = sai_tam_api->remove_tam_event(tam_event_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM event object.");
        return false;
    }
    return true;
}


/* Create TAM telemetry type object.
 *
 */
bool Tam::tamTelemetryTypeCreate(sai_object_id_t tam_report_obj,
                                    sai_object_id_t *tam_telemetry_type_obj)
{
    sai_attribute_t attr;
    sai_status_t status;
    vector<sai_attribute_t> tam_telemetry_type_attrs;

    if (tam_telemetry_type_obj == NULL)
    {
        return false;
    }

    /* Create TAM telemetry type object. */
    /* Setup attributes. */
    attr.id = SAI_TAM_TEL_TYPE_ATTR_TAM_TELEMETRY_TYPE;
    attr.value.s32 = SAI_TAM_TELEMETRY_TYPE_SWITCH;
    tam_telemetry_type_attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_REPORT_ID;
    attr.value.oid = tam_report_obj;
    tam_telemetry_type_attrs.push_back(attr);

    attr.id = SAI_TAM_TEL_TYPE_ATTR_SWITCH_ENABLE_MMU_STATS;
    attr.value.booldata = true;
    tam_telemetry_type_attrs.push_back(attr);

    status = sai_tam_api->create_tam_tel_type(tam_telemetry_type_obj, gSwitchId,
                                                  (uint32_t)tam_telemetry_type_attrs.size(),
                                                        tam_telemetry_type_attrs.data() );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add telemetry type.");
        return false;
    }
    return true;
}

/* Delete TAM telemetry type object.
 *
 */
bool Tam::tamTelemetryTypeDelete(sai_object_id_t tam_telemetry_type_obj)
{
    sai_status_t status;

    /* Delete the TAM telemetry type object. */
    status = sai_tam_api->remove_tam_tel_type(tam_telemetry_type_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove telemetry type object.");
        return false;
    }
    return true;
}

/* 
 * Create  a TAM transport object.
 */
bool Tam::tamTransportCreate(int dstPort, int srcPort, sai_object_id_t *tam_transport_obj)
{
    sai_attribute_t attr;
    sai_status_t status;
    vector<sai_attribute_t> tam_transport_attrs;

    if (tam_transport_obj == NULL)
    {
        return false;
    }

    /* Create TAM transport object */
    /* Setup attributes. */
    attr.id = SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE;
    attr.value.s32 = SAI_TAM_TRANSPORT_TYPE_UDP;
    tam_transport_attrs.push_back(attr);

    attr.id = SAI_TAM_TRANSPORT_ATTR_SRC_PORT;
    attr.value.s32 = srcPort;
    tam_transport_attrs.push_back(attr);

    attr.id = SAI_TAM_TRANSPORT_ATTR_DST_PORT;
    attr.value.s32 = dstPort;
    tam_transport_attrs.push_back(attr);

    status = sai_tam_api->create_tam_transport(tam_transport_obj, gSwitchId,
                                                   (uint32_t)tam_transport_attrs.size(),
                                                     tam_transport_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM transport");
        return false;
    }

    return true;
}

/* Delete TAM transport object.
 *
 */
bool Tam::tamTransportDelete(sai_object_id_t tam_transport_obj)
{
    sai_status_t status;

    /* Delete the TAM transport object. */
    status = sai_tam_api->remove_tam_transport(tam_transport_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove transport object.");
        return false;
    }
    return true;
}

/*
 * Create TAM collector object. 
 */
bool Tam::tamCollectorCreate(sai_ip_address_t src_ip_addr, sai_ip_address_t dst_ip_addr,
                         sai_object_id_t tam_transport_obj, sai_object_id_t *tam_collector_obj)
{
    sai_attribute_t attr;
    sai_status_t status;
    vector<sai_attribute_t> tam_collector_attrs;

    if (tam_collector_obj == NULL)
    {
        return false;
    }

    attr.id = SAI_TAM_COLLECTOR_ATTR_SRC_IP;
    attr.value.ipaddr = src_ip_addr;
    tam_collector_attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_DST_IP;
    attr.value.ipaddr = dst_ip_addr;
    tam_collector_attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_TRANSPORT;
    attr.value.oid = tam_transport_obj;
    tam_collector_attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_DSCP_VALUE;
    attr.value.u8 = 0;
    tam_collector_attrs.push_back(attr);

    status = sai_tam_api->create_tam_collector(tam_collector_obj, gSwitchId, 
                                           (uint32_t)tam_collector_attrs.size(),
                                                     tam_collector_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM collector object.");
        return false;
    }

    return true;
}

/* Delete TAM collector object.
 *
 */
bool Tam::tamCollectorDelete(sai_object_id_t tam_collector_obj)
{
    sai_status_t status;

    /* Delete the TAM collector object. */
    status = sai_tam_api->remove_tam_collector(tam_collector_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove TAM collector object.");
        return false;
    }
    return true;
}

/*
 * Create TAM telemetry object.
 */
bool Tam::tamTelemetryCreate(int interval, sai_object_id_t tam_telemetry_type_obj,
                                sai_object_id_t tam_collector_obj, sai_object_id_t *tam_telemetry_obj)
{
    sai_attribute_t attr;
    vector<sai_attribute_t> tam_telemetry_attrs;
    sai_status_t status;

    if (tam_telemetry_obj == NULL)
    {
        return false;
    }

    /* TAM periodic report to be configured
     * with interval = 0 i.e send a report 
     * right away. Create a TAM object.
     */
    vector<sai_object_id_t> tam_list;
    tam_list.push_back(tam_telemetry_type_obj);
    attr.id = SAI_TAM_TELEMETRY_ATTR_TAM_TYPE_LIST;
    attr.value.objlist.count = (uint32_t)tam_list.size();
    attr.value.objlist.list = tam_list.data();
    tam_telemetry_attrs.push_back(attr);
    
    vector<sai_object_id_t> collector_list;
    collector_list.push_back(tam_collector_obj);
    attr.id = SAI_TAM_TELEMETRY_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = (uint32_t)collector_list.size();
    attr.value.objlist.list = collector_list.data();
    tam_telemetry_attrs.push_back(attr);

    attr.id = SAI_TAM_TELEMETRY_ATTR_TAM_REPORTING_UNIT;
    attr.value.s32 = SAI_TAM_REPORTING_UNIT_SEC;
    tam_telemetry_attrs.push_back(attr);
   
    attr.id = SAI_TAM_TELEMETRY_ATTR_REPORTING_INTERVAL;
    /* Hardcode interval to 0. */
    attr.value.s32 = 0;
    tam_telemetry_attrs.push_back(attr);
    status = sai_tam_api->create_tam_telemetry(tam_telemetry_obj, gSwitchId, 
                                                    (uint32_t)tam_telemetry_attrs.size(),
                                                     tam_telemetry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM telemetry object.");
        return false;
    }

    return true;
}

/* Delete TAM telemetry object.
 *
 */
bool Tam::tamTelemetryDelete(sai_object_id_t tam_telemetry_obj)
{
    sai_status_t status;

    /* Delete the TAM telemetry object. */
    status = sai_tam_api->remove_tam_telemetry(tam_telemetry_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove TAM telemetry object.");
        return false;
    }
    return true;
}

/*
 * Create TAM threshold event object.
 */
bool Tam::tamEventThresholdCreate(int threshold, sai_object_id_t *tam_event_threshold_obj)
{
    sai_status_t status;
    sai_attribute_t attr;
    vector<sai_attribute_t> tam_event_thd_attrs;

    attr.id = SAI_TAM_EVENT_THRESHOLD_ATTR_ABS_VALUE;
    attr.value.u32 = threshold;
    tam_event_thd_attrs.push_back(attr);

    attr.id = SAI_TAM_EVENT_THRESHOLD_ATTR_UNIT;
    attr.value.u32 = SAI_TAM_EVENT_THRESHOLD_UNIT_PERCENT;
    tam_event_thd_attrs.push_back(attr);

    status = sai_tam_api->create_tam_event_threshold(tam_event_threshold_obj, gSwitchId,
                                                      (uint32_t)tam_event_thd_attrs.size(), 
                                                       tam_event_thd_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM event threshold object.");
        return false;
    }

    return true;
}

/*
 * Delete TAM threshold event object.
 */
bool Tam::tamEventThresholdDelete(sai_object_id_t tam_event_threshold_obj)
{
    sai_status_t status;

    /* Delete the TAM threshold evnet object. */
    status = sai_tam_api->remove_tam_event_threshold(tam_event_threshold_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove TAM event threshold object.");
        return false;
    }
    return true;
}

/*
 *  Create TAM object.
 */
bool Tam::tamCreate(sai_tam_bind_point_type_t bind_point, sai_tam_attr_t attr_type,
                       sai_object_id_t tam_telemetry_event_obj, sai_object_id_t *tam_obj)
{
    sai_attribute_t attr;
    vector<sai_attribute_t> tam_attrs;
    sai_status_t status;

    if (tam_obj == NULL)
    {
        return false;
    }

    /* Create a TAM object. */
    vector<sai_object_id_t> tam_list;
    tam_list.push_back(tam_telemetry_event_obj);
    attr.id = attr_type;
    attr.value.objlist.count = (uint32_t)tam_list.size();
    attr.value.objlist.list = tam_list.data();
    tam_attrs.push_back(attr);
    
    vector<sai_int32_t> bind_list;
    bind_list.push_back(bind_point);
    attr.id = SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST;
    attr.value.s32list.count = (uint32_t)bind_list.size();
    attr.value.s32list.list = bind_list.data();
    tam_attrs.push_back(attr);

    status = sai_tam_api->create_tam(tam_obj, gSwitchId, 
                                           (uint32_t)tam_attrs.size(),
                                           tam_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM object.");
        return false;
    }

    return true;
}

/* Delete TAM object.
 *
 */
bool Tam::tamDelete(sai_object_id_t tam_obj)
{
    sai_status_t status;

    /* Delete the TAM object. */
    status = sai_tam_api->remove_tam(tam_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove TAM telemetry object.");
        return false;
    }
    return true;
}

/*
 *  Create TAM INT object.
 */
bool Tam::tamIntCreate(uint32_t device_id, sai_object_id_t tam_report_obj,
                       bool sampling_enable, sai_object_id_t tam_sampling_obj,
                       bool collector_enable, sai_object_id_t tam_collector_obj,
                       sai_object_id_t *tam_int_obj)
{
    sai_attribute_t attr;
    sai_status_t status;
    vector<sai_attribute_t> tam_int_attrs;

    if (tam_int_obj == NULL)
    {
        return false;
    }

    /* Configure TAM INT attributes. */
    attr.id             = SAI_TAM_INT_ATTR_TYPE;
    attr.value.s32      = SAI_TAM_INT_TYPE_IFA1;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_DEVICE_ID;
    attr.value.u32      = device_id;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_INT_PRESENCE_TYPE;
    attr.value.u32      = SAI_TAM_INT_PRESENCE_TYPE_PB;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_INT_PRESENCE_PB1;
    attr.value.u32      = 0xAAAAAAAA;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_INT_PRESENCE_PB2;
    attr.value.u32      = 0xBBBBBBBB;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_INLINE;
    attr.value.booldata = false;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_MAX_HOP_COUNT;
    attr.value.u8       = 32;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_MAX_LENGTH;
    attr.value.u8       = 255;
    tam_int_attrs.push_back(attr);

    attr.id             = SAI_TAM_INT_ATTR_REPORT_ID;
    attr.value.oid      = tam_report_obj;
    tam_int_attrs.push_back(attr);

    if (true == sampling_enable)
    {
        attr.id             = SAI_TAM_INT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid      = tam_sampling_obj;
        tam_int_attrs.push_back(attr);
    }

    if (true == collector_enable)
    {
        sai_attribute_t tam_attr_list[1];
        memset(tam_attr_list, 0, sizeof(sai_attribute_t));
        tam_attr_list[0].value.objlist.list = (sai_object_id_t *)calloc(1,sizeof(sai_object_id_t));
        tam_attr_list[0].id = SAI_TAM_INT_ATTR_COLLECTOR_LIST;
        tam_attr_list[0].value.objlist.count = 1;
        tam_attr_list[0].value.objlist.list[0] = tam_collector_obj;

        tam_int_attrs.push_back(tam_attr_list[0]);
    }

    /* Create a TAM INT object. */
    status = sai_tam_api->create_tam_int(tam_int_obj, gSwitchId, 
                                         (uint32_t)tam_int_attrs.size(),
                                         tam_int_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to add TAM INT object.");
        return false;
    }

    SWSS_LOG_DEBUG("Created TAM INT object OID:0x%lx", *tam_int_obj);
    return true;
}

/* Delete TAM INT object.
 *
 */
bool Tam::tamIntDelete(sai_object_id_t tam_int_obj)
{
    sai_status_t status;

    /* Delete the TAM INT object. */
    status = sai_tam_api->remove_tam_int(tam_int_obj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to remove TAM INT telemetry object.");
        return false;
    }

    SWSS_LOG_DEBUG("Deleted TAM INT object OID:0x%lx", tam_int_obj);
    return true;
}
