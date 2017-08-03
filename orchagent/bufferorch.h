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

const string pgs = "3-4";
const vector<uint32_t> supported_speed = { 10000, 25000, 40000, 50000, 100000 };
const vector<uint32_t> supported_cable = { 5, 40, 300 };


class BufferOrch : public Orch, public Observer, public Subject
{
public:
    BufferOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch);
    static type_map m_buffer_type_maps;
private:
    typedef task_process_status (BufferOrch::*buffer_table_handler)(Consumer& consumer);
    typedef map<string, buffer_table_handler> buffer_table_handler_map;
    typedef pair<string, buffer_table_handler> buffer_handler_pair;
    typedef map<string, string> cable_length_map;
    typedef map<string, string> port_config_profile_map;

    virtual void doTask(Consumer& consumer);
    virtual void update(SubjectType type, void *cntx);
    void updatePortProfile(const PortSpeedUpdate& update);
    void initTableHandlers();
    bool getTableValue(string table_name, string table_key, string item_key, string &item_value);
    bool getTableValue(string table_name, string table_key, string item_key, uint32_t &item_value);
    bool setTableValue(string table_name, string table_key, string item_key, string &item_value);
    bool setTableValue(string table_name, string table_key, string item_key, uint32_t &item_value);
    string cutTableKey(string fullTableName);
    uint32_t roundUp(uint32_t value, vector<uint32_t> round_values);

    task_process_status processBufferPool(Consumer &consumer);
    task_process_status processBufferProfile(Consumer &consumer);
    task_process_status processQueue(Consumer &consumer);
    task_process_status processPriorityGroup(Consumer &consumer);
    task_process_status processIngressBufferProfileList(Consumer &consumer);
    task_process_status processEgressBufferProfileList(Consumer &consumer);
    task_process_status processPortConfigToPgProfile(Consumer &consumer);
    task_process_status processPortCableLenth(Consumer &consumer);

    swss::DBConnector m_db;
    PortsOrch *m_portsOrch;
    cable_length_map m_cableLengthMap;
    buffer_table_handler_map m_bufferHandlerMap;
    port_config_profile_map m_portConfigProfileMap;
};
#endif /* SWSS_BUFFORCH_H */

