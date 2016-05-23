#ifndef SWSS_BUFFORCH_H
#define SWSS_BUFFORCH_H

#include <map>
#include "orch.h"
#include "portsorch.h"

const std::string buffer_size_field_name         = "size";
const std::string buffer_pool_type_field_name    = "type";
const std::string buffer_pool_mode_field_name    = "mode";
const std::string buffer_pool_field_name         = "pool";
const std::string buffer_xon_field_name          = "xon";
const std::string buffer_xoff_field_name         = "xoff";
const std::string buffer_dynamic_th_field_name   = "dynamic_th";
const std::string buffer_static_th_field_name    = "static_th";
const std::string buffer_profile_field_name      = "profile";
const std::string buffer_value_ingress           = "ingress";
const std::string buffer_value_egress            = "egress";
const std::string buffer_pool_mode_dynamic_value = "dynamic";
const std::string buffer_pool_mode_static_value  = "static";
const std::string range_specifier                = "-";
const std::string buffer_profile_list_field_name = "profile_list";
const std::string comma                          = ",";

class BufferOrch : public Orch
{
public:
    BufferOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch);
    static type_map m_buffer_type_maps;
private:
    typedef task_process_status (BufferOrch::*buffer_table_handler)(Consumer& consumer);
    typedef std::map<std::string, buffer_table_handler> buffer_table_handler_map;
    typedef std::pair<string, buffer_table_handler> buffer_handler_pair;

    virtual void doTask(Consumer& consumer);
    void initTableHandlers();
    task_process_status processBufferPool(Consumer &consumer);
    task_process_status processBufferProfile(Consumer &consumer);
    task_process_status processQueue(Consumer &consumer);
    task_process_status processPriorityGroup(Consumer &consumer);
    task_process_status processIngressBufferProfileList(Consumer &consumer);
    task_process_status processEgressBufferProfileList(Consumer &consumer);
    bool parseIndexRange(const string &input, sai_uint32_t &range_low, sai_uint32_t &range_high);
    bool parseNameArray(const string &input, vector<string> &port_names);
private:
    PortsOrch *m_portsOrch;
    buffer_table_handler_map m_bufferHandlerMap;
};
#endif /* SWSS_BUFFORCH_H */

