#ifndef SWSS_BUFFORCH_H
#define SWSS_BUFFORCH_H

#include <map>
#include "orch.h"
#include "portsorch.h"

const string buffer_size_field_name         = "size";
const string buffer_pool_type_field_name    = "type";
const string buffer_pool_mode_field_name    = "mode";
const string buffer_pool_field_name         = "pool";
const string buffer_xon_field_name          = "xon";
const string buffer_xoff_field_name         = "xoff";
const string buffer_dynamic_th_field_name   = "dynamic_th";
const string buffer_static_th_field_name    = "static_th";
const string buffer_profile_field_name      = "profile";
const string buffer_value_ingress           = "ingress";
const string buffer_value_egress            = "egress";
const string buffer_pool_mode_dynamic_value = "dynamic";
const string buffer_pool_mode_static_value  = "static";
const string buffer_profile_list_field_name = "profile_list";

class BufferOrch : public Orch
{
public:
    BufferOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch);
    static type_map m_buffer_type_maps;
private:
    typedef task_process_status (BufferOrch::*buffer_table_handler)(Consumer& consumer);
    typedef map<string, buffer_table_handler> buffer_table_handler_map;
    typedef pair<string, buffer_table_handler> buffer_handler_pair;

    virtual void doTask(Consumer& consumer);
    void initTableHandlers();
    task_process_status processBufferPool(Consumer &consumer);
    task_process_status processBufferProfile(Consumer &consumer);
    task_process_status processQueue(Consumer &consumer);
    task_process_status processPriorityGroup(Consumer &consumer);
    task_process_status processIngressBufferProfileList(Consumer &consumer);
    task_process_status processEgressBufferProfileList(Consumer &consumer);

    PortsOrch *m_portsOrch;
    buffer_table_handler_map m_bufferHandlerMap;
};
#endif /* SWSS_BUFFORCH_H */

