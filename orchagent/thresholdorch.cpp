#include <assert.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <utility>

#include "logger.h"
#include "tokenize.h"
#include "orch.h"
#include "table.h"
#include "thresholdorch.h"
#include "portsorch.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "observer.h"
#include "swssnet.h"

extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;
extern sai_tam_api_t* sai_tam_api;
extern sai_port_api_t* sai_port_api;
extern sai_buffer_api_t* sai_buffer_api;
extern sai_queue_api_t* sai_queue_api;
extern sai_switch_api_t* sai_switch_api;

ThresholdOrch::ThresholdOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *port) :
    Orch(db, tableNames), 
    m_portsOrch(port)
{
    
    SWSS_LOG_ENTER();
    m_portsOrch->attach(this);

    /* Initialize SAI objects to NULL object ID. */
    m_tamReport = SAI_NULL_OBJECT_ID;
    m_tamEventAction = SAI_NULL_OBJECT_ID;
    m_tamTransport = SAI_NULL_OBJECT_ID;
    m_tamCollector = SAI_NULL_OBJECT_ID;

    /* Create TAM report type object. */
    if (tamReportCreate(SAI_TAM_REPORT_TYPE_PROTO, &m_tamReport) != true)
    {
        SWSS_LOG_ERROR("Unable to create TAM report object.");
    }

    /* Create TAM event object */
    if (tamEventActionCreate(m_tamReport, &m_tamEventAction) != true)
    {
        SWSS_LOG_ERROR("Unable to create TAM event action object.");
    }

    /* Create TAM transport object */
    if (tamTransportCreate(THRESHOLD_ORCH_SAI_TAM_THRESHOLD_DST_PORT, THRESHOLD_ORCH_SAI_TAM_TRANSPORT_SRC_PORT,
                              &m_tamTransport ) != true)
    {
        SWSS_LOG_ERROR("Unable to create TAM transport object.");
    }

    /* Create TAM collector object */
    /* Create TAM collector object */  
    /* Using a random source ip */
//    sai_ip_address_t src_ip_addr = {SAI_IP_ADDR_FAMILY_IPV4, .addr.ip4 = 0x0a0a0a0a }; 
//    sai_ip_address_t dst_ip_addr = {SAI_IP_ADDR_FAMILY_IPV4, .addr.ip4 = 0x7f000001 };
    sai_ip_address_t src_ip_addr; 
    sai_ip_address_t dst_ip_addr;
    IpAddress src_ip(0x0a0a0a0a);
    IpAddress dst_ip(0x7f000001);

    copy (dst_ip_addr, dst_ip);
    copy (src_ip_addr, src_ip);

    if (tamCollectorCreate(src_ip_addr, dst_ip_addr, m_tamTransport, &m_tamCollector) != true)
    {
        SWSS_LOG_ERROR("Unable to create TAM collector object.");
    }
}

ThresholdOrch::~ThresholdOrch()
{
    m_portsOrch->detach(this);
 
    /* Delete TAM objects. */
    (void) tamReportDelete(m_tamReport); 
    (void) tamEventActionDelete(m_tamEventAction);
    (void) tamTransportDelete(m_tamTransport);
    (void) tamCollectorDelete(m_tamCollector);
}

void ThresholdOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type) {
        default:
            break;
    }

    return;
}

/*
 * Add TAM threshold table entry.
 */
bool ThresholdOrch::addTamThdObjEntry(uint32_t threshold)
{
    sai_object_id_t tamThdObj;
    thresTamThdObjData tamThdData;

    /* New threshold TAM object. */
    if (tamEventThresholdCreate(threshold, &tamThdObj) != true)
    {
        SWSS_LOG_ERROR("Unable to create event threshold object.");
        return false;
    }

    /* Add an entry to threshold table. */
    tamThdData.m_thresTamThd = tamThdObj;
    tamThdData.ref_cnt = 0;
    m_thresTamThdTable[threshold] = tamThdData;
    return true;
}

/*
 * Delete TAM threshold table entry.
 */
bool ThresholdOrch::deleteTamThdObjEntry(uint32_t threshold)
{
    if (tamEventThresholdDelete(m_thresTamThdTable[threshold].m_thresTamThd) != true) 
    {
        SWSS_LOG_ERROR("Unable to delete event threshold object.");
        return false;
    }

    /* Update the threshold table. */
    m_thresTamThdTable.erase(threshold);
   
    return true;
}

/*
 * Add TAM object table entry. 
 */
bool ThresholdOrch::addTamObjEntry(thresTamObjEntry tamObjEntry)
{
    sai_object_id_t tamThdObj;
    uint32_t threshold = tamObjEntry.threshold;
    sai_tam_event_type_t type = tamObjEntry.type;
    thresTamObjData tamEntryData;
    sai_tam_bind_point_type_t bindPoint = bindPointType.at(type);
    bool thdCreated = false;

    /* Check if a TAM threshold object exists. */
    if (m_thresTamThdTable.find(threshold) == m_thresTamThdTable.end())
    {
        /* New threshold object table entry. */ 
        if (addTamThdObjEntry(threshold) != true)
        {
            SWSS_LOG_ERROR("Unable to add threshold table entry.");
            return false;
        } 
        thdCreated = true;
    }

    tamThdObj = m_thresTamThdTable[threshold].m_thresTamThd;

    /* Create a new TAM object entry */

    /* Create TAM event object. */
    if (tamEventCreate(type, m_tamEventAction, 
                            m_tamCollector, tamThdObj, &tamEntryData.m_thresTamEvent) != true)
    {
        SWSS_LOG_ERROR("Unable to create tam event object.");
        /* Destroy the threshold event object created earlier. */
        if (thdCreated == true)
            (void) deleteTamThdObjEntry(threshold);
        return false;
    }

    /* Create TAM object. */
    if (tamCreate(bindPoint, SAI_TAM_ATTR_EVENT_OBJECTS_LIST,
                                tamEntryData.m_thresTamEvent, &tamEntryData.m_thresTam) != true)
    {
        SWSS_LOG_ERROR("Unable to create tam object.");
        /* Destroy TAM event object and threshold object. */
        (void) tamEventDelete(tamEntryData.m_thresTamEvent);
        if (thdCreated == true)
            (void) deleteTamThdObjEntry(threshold);
        return false;
    }

    /* Create TAM table entry. */
    m_thresTamTable[tamObjEntry] = tamEntryData;

    /* Increase ref count of threshold object. */
    m_thresTamThdTable[threshold].ref_cnt++;

    return true;
}

/*
 * Delete TAM object table entry. 
 */
bool ThresholdOrch::deleteTamObjEntry(thresTamObjEntry tamObjEntry)
{
    thresTamObjData tamEntryData = m_thresTamTable[tamObjEntry];

    /* Destroy TAM object */
    if (tamDelete(tamEntryData.m_thresTam) != true)
    {
        SWSS_LOG_ERROR("Unable to delete TAM object.");
        return false;
    }
                
    /* Destroy TAM event object */
    if (tamEventDelete(tamEntryData.m_thresTamEvent) != true)
    {
        SWSS_LOG_ERROR("Unable to delete TAM event object.");
        return false;
    }

    /* Delete entry from TAM object table. */
    m_thresTamTable.erase(tamObjEntry);

    /* Decrement threshold ref cnt */
    m_thresTamThdTable[tamObjEntry.threshold].ref_cnt--;
    if (m_thresTamThdTable[tamObjEntry.threshold].ref_cnt == 0)
    {
        /* Delete threshold table entry */
        if (deleteTamThdObjEntry(tamObjEntry.threshold) != true)
        {
            SWSS_LOG_ERROR("Unable to delete TAM threshold object entry.");
            return false;
        }
    }

    return true;
}

/* 
 * Bind TAM object to bind points. 
 */
bool ThresholdOrch::thresTamObjBind(thresEventType_t thresType, string alias, uint32_t index,
                          sai_object_id_t tamObj)
{
    sai_attribute_t attr;
    vector<sai_object_id_t>list;
    uint32_t qid;
    sai_status_t status;
    Port port;

    SWSS_LOG_ENTER();
    
    if (!gPortsOrch->getPort(alias, port))
    {
        SWSS_LOG_ERROR("Unable to get port data for port %s", alias.c_str());
        return false;
    }

    list.clear();
    if (tamObj != SAI_NULL_OBJECT_ID)
    {
        list.push_back(tamObj);
    }
    attr.value.objlist.count = (uint32_t)list.size();
    attr.value.objlist.list = list.data();
    
    /* Port class maintains per port pgs, queues etc. 
     * Use the data to get appropriate sai_object_id_t
     */
    if ((thresType == THRES_EVENT_TYPE_IPG_SHARED) || 
                (thresType == THRES_EVENT_TYPE_IPG_HEADROOM))
    {
        attr.id = SAI_INGRESS_PRIORITY_GROUP_ATTR_TAM;
        /* Bind a TAM object to port, pg pair. */
        /* Port has been validated in doTask(), go ahead and use the data. */
        status = sai_buffer_api->set_ingress_priority_group_attribute(port.m_priority_group_ids[index], 
                                                                              &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Unable to set ingress port priority group attribute TAM object.");
            return false;
        }
    }
    else if ((thresType == THRES_EVENT_TYPE_QUEUE_UNICAST) 
                || (thresType == THRES_EVENT_TYPE_QUEUE_MULTICAST))
    {
        attr.id = SAI_QUEUE_ATTR_TAM_OBJECT;

        /* Bind a TAM object to port, queue pair. */
        if ((thresType == THRES_EVENT_TYPE_QUEUE_UNICAST))
        {
            qid = index;
        }
        else if ((thresType == THRES_EVENT_TYPE_QUEUE_MULTICAST))
        {
            /* MCQ - multicast queues start after unicast queues. 
             * Hence, multicast queue index = queue index + num_uc_queues.
             */
            int num_uc_queues = 0;
            sai_attribute_t queue_attr;
            
            queue_attr.id = SAI_QUEUE_ATTR_TYPE;
            queue_attr.value.s32 = 0;

            for (qid = 0; qid < port.m_queue_ids.size(); qid++)
            {
                /* Get queue type. */
                status = sai_queue_api->get_queue_attribute(port.m_queue_ids[qid], 1, &queue_attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to get queue type and index for queue %u rv:%d", qid, status);
                    return false;
                }

                if (queue_attr.value.s32 == SAI_QUEUE_TYPE_UNICAST)
                {
                    num_uc_queues++;
                }
            }
 
            /* Multicast queues start after unicast queues. */
            qid = num_uc_queues + index;
        }

        /* UCQ - first 8/10 queues of a port are unicast. 
         * CPUQ - 8 queues, map to queue index logically.
         */
        status = sai_queue_api->set_queue_attribute(port.m_queue_ids[qid], &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Unable to set port queue attribute.");
            return false;
        }
    }
   
    return true;
} 

/* 
 * Unbind TAM object to bind points. 
 */
bool ThresholdOrch::thresTamObjUnbind(thresEventType_t type, string alias, uint32_t index)
{
    return thresTamObjBind(type, alias, index, SAI_NULL_OBJECT_ID);
}

sai_tam_event_type_t ThresholdOrch::getTamEventType(thresEventType_t type)
{
    sai_tam_event_type_t tamEventType;

    if (type == THRES_EVENT_TYPE_IPG_SHARED) 
    {
        tamEventType = SAI_TAM_EVENT_TYPE_IPG_SHARED;
    }
    else if (type == THRES_EVENT_TYPE_IPG_HEADROOM)
    {
        tamEventType = SAI_TAM_EVENT_TYPE_IPG_XOFF_ROOM;
    }
    else 
    {
        tamEventType = SAI_TAM_EVENT_TYPE_QUEUE_THRESHOLD;
    }

    return tamEventType;
}

/*
 * Add and bind TAM entry.
 * If a TAM entry exists, the bind point is updated.
 */
bool ThresholdOrch::addBindTamEntry(thresEventType_t eventType, string alias, uint32_t index, 
                                     uint32_t threshold)
{
    sai_tam_event_type_t type = getTamEventType(eventType);
    thresTamObjEntry tamEntry = {threshold, type};
    thresTamObjData tamEntryData;
    bool tamCreated = false;

    /* Check if there is an existing TAM object for config. */
    /* Key is <threshold, stat_type> */
    if (m_thresTamTable.find(tamEntry) == m_thresTamTable.end())
    {
        /* No existing TAM object, need to create a new one. */
        if (addTamObjEntry(tamEntry) != true)
        {
            SWSS_LOG_ERROR("Unable to create TAM object table entry.");
            return false;
         }
         tamCreated = true;
     }   

     tamEntryData = m_thresTamTable[tamEntry];

     /* Bind TAM object to bind points */
     if (thresTamObjBind(eventType, alias, index, 
                                tamEntryData.m_thresTam) != true)
     {
         SWSS_LOG_ERROR("Unable to bind TAM object");
         /* Delete TAM object */
         if (tamCreated == true)
             (void) deleteTamObjEntry(tamEntry);
         return false;
     }
 
     bind_list_t bind = {eventType, alias, index};

     /* Update bind list of TAM entry */
     m_thresTamTable[tamEntry].bind_list.push_back(bind);
     return true;
}

/*  
 *  Update existing port realm entry.
 */
bool ThresholdOrch::updateCfgThreshold(thresEventType_t eventType, string alias, uint32_t index,
                                        uint32_t cfgThreshold, uint32_t threshold)
{
    sai_tam_event_type_t type = getTamEventType(eventType);
    thresTamObjEntry tamEntry = {cfgThreshold, type};
    thresTamObjData *tamEntryData;

    SWSS_LOG_ENTER();

    /* Find the TAM object. */
    if (m_thresTamTable.find(tamEntry) == m_thresTamTable.end())
    {
        SWSS_LOG_ERROR("Realm entry exists but TAM object entry does not exist");
        return false;
    }
            
    tamEntryData = &m_thresTamTable[tamEntry];

    /* Unbind and proceed. */
    if (thresTamObjUnbind(eventType, alias, index) != true)
    {
        SWSS_LOG_ERROR("Unable to unbind TAM object."); 
        return false;
    }

    /* Check if there are other bind points for this TAM object. */
    if (tamEntryData->bind_list.size() > 1)
    {
        /* Find the entry in vector */
        for (vector<bind_list_t>::iterator it = tamEntryData->bind_list.begin(); 
                                          it != tamEntryData->bind_list.end(); ++it)
        {
            if (it->type == eventType && 
                      it->alias == alias && it->index == index)
            {
                tamEntryData->bind_list.erase(it);
                break;
            }
        }

        /* Find and elete entry from bind list */
       // tamEntryData->bind_list.erase(bindList);
    }
    else
    {
        /* Delete the TAM object itself */
        if (deleteTamObjEntry(tamEntry) != true)
        {
            SWSS_LOG_ERROR("Unable to delete TAM object entry.");  
        }
    }

    SWSS_LOG_DEBUG("Applying new threshold %d to entry", threshold);

    /* Apply the new threshold */
    tamEntry.threshold  = threshold;
    tamEntry.type = type;

    /* Check if there is an existing TAM object for config. */
    /* Key is <threshold, stat_type> */
    if (m_thresTamTable.find(tamEntry) == m_thresTamTable.end())
    {
        SWSS_LOG_DEBUG("New Tam entry being added thres %d, type %d.", threshold, type);

        /* No existing TAM object, need to create a new one. */
        if (addTamObjEntry(tamEntry) != true)
        {
            SWSS_LOG_ERROR("Unable to create TAM object table entry.");
            return false;
        }
    }

    tamEntryData = &m_thresTamTable[tamEntry];

    /* Bind TAM object to bind points */
    if (thresTamObjBind(eventType, alias, index, tamEntryData->m_thresTam) != true)
    {
        SWSS_LOG_ERROR("Unable to bind TAM object.");
        /* Delete TAM object */
        (void) deleteTamObjEntry(tamEntry);
        return false;
    }

    bind_list_t bind = {eventType, alias, index};
    /* Update bind list of TAM entry */
    m_thresTamTable[tamEntry].bind_list.push_back(bind);

    return true;
}

/*  
 *  Add/update threshold configuration.
 */
bool ThresholdOrch::addUpdateThresEntry(thresEventType_t eventType, string alias, uint32_t index, 
                                        uint32_t threshold)
{
    /* Get realm type */
    thresEntry entry = {eventType, alias, index};

    SWSS_LOG_ENTER();

    if (m_thresTable.find(entry) == m_thresTable.end())
    {
        /* New thres entry. */
        if (addBindTamEntry(eventType, alias, index,
                                 threshold) != true)
        {
            SWSS_LOG_ERROR("Unable to add/bind TAM  entry.");
            return false;
        }

        /* Create the new realm entry. */
        m_thresTable[entry] = threshold;

        return true;
    }    

    SWSS_LOG_DEBUG("Received an update for existing entry, Old threshold %d, new %d", m_thresTable[entry], threshold);

    /* Update existing realm entry. 
     * Remove existing config and apply new configuration.
     */
    if (threshold == m_thresTable[entry])
    {
        /* Configuration already exists. Return success */
        SWSS_LOG_DEBUG("Received threshold update for same threshold value %d - %d.", m_thresTable[entry], threshold);
        return true;
    }

    if (updateCfgThreshold(entry.type, entry.alias, entry.index,
                             m_thresTable[entry], threshold) != true)
    {
        SWSS_LOG_ERROR("Unable to update threshold table entry.");
        return false;
    }

    /* Update thres entry. */
    m_thresTable[entry] = threshold;

    return true;
}

/*  
 *  Remove threshold configuration.
 */
bool ThresholdOrch::removeThresEntry(thresEventType_t eventType, string alias, uint32_t index) 
{
    sai_tam_event_type_t type = getTamEventType(eventType);
    thresEntry entry = {eventType, alias, index};
    uint32_t threshold;

    /* Sequence of removal is:
     * Find TAM object, check bind_list.
     * Unbind from TAM object and if bind_list is 0 after
     * remove TAM object and threshold if ref_cnt is 0.
     * Finally, remove the entry from port/global realm table. 
     */
    /* Find the realm entry. */
    if (m_thresTable.find(entry) == m_thresTable.end())
    {
        SWSS_LOG_ERROR("Unable to find the threshold entry.");
        /* Return true here so that the event gets erased. */
        return true;
    }

    threshold = m_thresTable[entry];

    thresTamObjEntry tamEntry = {threshold, type};
    thresTamObjData *tamEntryData;

    /* Find the TAM object. */
    if (m_thresTamTable.find(tamEntry) == m_thresTamTable.end())
    {
        SWSS_LOG_ERROR("Realm entry exists but TAM object entry does not exist");
        return false;
    }

    /* Unbind and proceed. */
    if (thresTamObjUnbind(eventType, alias, index) != true)
    {
        SWSS_LOG_ERROR("Unable to unbind TAM object."); 
        return false;
    }

    tamEntryData = &m_thresTamTable[tamEntry];

    /* Check if there are other bind points for this TAM object. */
    if (tamEntryData->bind_list.size() > 1)
    {
        /* Find the entry in vector */
        for (vector<bind_list_t>::iterator it = tamEntryData->bind_list.begin(); 
                                          it != tamEntryData->bind_list.end(); ++it)
        {
            if (it->type == eventType && 
                      it->alias == alias && it->index == index)
            {
                tamEntryData->bind_list.erase(it);
                break;
            }
        }

        /* Find and elete entry from bind list */
       // tamEntryData.bind_list.erase(bindList);
    }
    else
    {
        /* Delete the TAM object itself */
        if (deleteTamObjEntry(tamEntry) != true)
        {
            SWSS_LOG_ERROR("Unable to delete TAM object entry.");  
        }
    }

    /* Delete the realm entry. */
    m_thresTable.erase(entry);

    return true;
}
 
thresEventType_t getThresEventType(string group, string statType)
{
    thresEventType_t type; 

    if (group == "priority-group" && statType == "shared")
    {
        type = THRES_EVENT_TYPE_IPG_SHARED;
    }
    else if (group == "priority-group" && statType == "headroom")
    {
        type = THRES_EVENT_TYPE_IPG_HEADROOM;
    }
    else if (group == "queue" && statType == "unicast")
    {
        type = THRES_EVENT_TYPE_QUEUE_UNICAST;
    }  
    else
    {
        type = THRES_EVENT_TYPE_QUEUE_MULTICAST;
    }

    return type;
}

bool thresholdValidate (uint32_t threshold)
{
    /* Validate the threshold value. 
     * As of now, threshold is configured as a
     * percentage of buffer utilization.
     */
    if ((threshold < THRESHOLD_MIN) || (threshold > THRESHOLD_MAX))
    {
        SWSS_LOG_ERROR("Invalid threshold %d received.", threshold);
        return false;
    }
    return true;
}

bool thresholdIndexValidate (uint32_t index, thresEventType_t eventType, Port port)
{
    uint32_t len = 0;
    
    if ((eventType == THRES_EVENT_TYPE_IPG_SHARED) || 
               (eventType == THRES_EVENT_TYPE_IPG_HEADROOM))
    {
        len = (uint32_t)port.m_priority_group_ids.size();
    }
    else if ((eventType == THRES_EVENT_TYPE_QUEUE_UNICAST) ||
                           (THRES_EVENT_TYPE_QUEUE_MULTICAST))
    {
        len = (uint32_t)port.m_queue_ids.size();
    }
    else
    {
        SWSS_LOG_ERROR("Invalid eventType received.");     
        return false;
    }

    if (index > (len - 1))
    {
        SWSS_LOG_ERROR("Invalid threshold index received %d.", index);     
        return false;
    }
            
    return true;
}

/* Threshold table updates. */
void ThresholdOrch::doThresholdTask(Consumer& consumer)
{
    /* Retrieve attributes from table. */
    thresEventType_t eventType;
    string alias, group, type;
    uint32_t index;
    Port port;

    SWSS_LOG_ENTER();

    /* Read the key value tuples from Consumer */
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
       /* For every entry read from the queue,
        * extract data and call handler to handle data.
        * If required to wait for an event, keep event in queue
        * and continue.
        */
        auto &t = it->second;

        /* Extract key */
        /* key can be of format priority-group|shared|Ethernet1|3 
         * or queue|unicast|Ethernet5|3
         */
        vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter); 

        if (keys.size() != 4)
        {
            /* Invalid table entry */
            SWSS_LOG_ERROR("Wrong format of table CFG_THRESHOLD_TABLE_NAME." );
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if ((keys[0] != "priority-group") && (keys[0] != "queue"))
        {
            /* Log and erase event. */
            SWSS_LOG_ERROR("Invalid buffer %s in key on THRESHOLD_TABLE ", keys[0].c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Validate keys */
        if (keys[0] == "priority-group" &&
             ((keys[1] != "shared") && (keys[1] != "headroom")))
        {
            SWSS_LOG_ERROR("Invalid buffer/buffer_type %s/%s in key on THRESHOLD_TABLE ", 
                                                           keys[0].c_str(), keys[1].c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        else if (keys[0] == "queue" &&
                 ((keys[1] != "unicast") && (keys[1] != "multicast")))
        {
            SWSS_LOG_ERROR("Invalid buffer/buffer_type %s/%s in key on THRESHOLD_TABLE ", 
                                                           keys[0].c_str(), keys[1].c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        eventType = getThresEventType(keys[0], keys[1]);

        alias = keys[2];

        /* Verify the port information. */
        if  (!gPortsOrch->isPortReady())
        { 
             /* Ports not ready yet. Skip over. */
             it++;
             continue;
        }

        if (!gPortsOrch->getPort(alias, port))
        {
            /* Port not available. Skip over. */
            it++;
            continue;  
        }
 
        index = (uint32_t)stoul(keys[3]);

        if (thresholdIndexValidate(index, eventType, port) != true)
        {
            /* Invalid index, delete event and proceed. */
            it = consumer.m_toSync.erase(it);
            SWSS_LOG_ERROR("Invalid index %d received in key %s.", index, kfvKey(t).c_str());
            continue;
        }

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            uint32_t threshold = 0;
            SWSS_LOG_DEBUG("Received SET command for THRESHOLD_TABLE, key %s", kfvKey(t).c_str());

            for (auto i : kfvFieldsValues(t))
            {

                if (fvField(i) == "threshold")
                {
                    threshold = stoi(fvValue(i));
                }
            }
   
            if (thresholdValidate(threshold) != true)
            {
                /* Invalid threshold, delete event and proceed. */
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_ERROR("Invalid threshold %d received in key %s.", threshold, kfvKey(t).c_str());
                continue;
            }
                 
            if (addUpdateThresEntry(eventType, alias, index, threshold) != true)
            {
                /* Retry configuration next time. */
                it++;
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_DEBUG("Received DEL command for THRESHOLD_TABLE, key %s", kfvKey(t).c_str());
            if (removeThresEntry(eventType, alias, index) != true)
            {
                /* Leave event in queue and 
                 * retry next time. 
                 */
                it++;
                continue;
            }
        }
        else 
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        /* Erase event. */
        it = consumer.m_toSync.erase(it);
    }
}

void ThresholdOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    /* Hand-off processing for each table to appropriate handler. */
    if (table_name == CFG_THRESHOLD_TABLE_NAME)
    {
        doThresholdTask(consumer); 
    }
}

