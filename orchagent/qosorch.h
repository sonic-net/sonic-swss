#ifndef SWSS_QOSORCH_H
#define SWSS_QOSORCH_H

#include <map>
#include "orch.h"
#include "portsorch.h"

const string dscp_to_tc_field_name             = "dscp_to_tc_map";
const string pfc_to_pg_map_name                = "pfc_to_pg_map";
const string pfc_to_queue_map_name             = "pfc_to_queue_map";
const string pfc_enable_name                   = "pfc_enable";
const string tc_to_pg_map_field_name           = "tc_to_pg_map";
const string tc_to_queue_field_name            = "tc_to_queue_map";
const string scheduler_field_name              = "scheduler";
const string red_max_threshold_field_name      = "red_max_threshold";
const string yellow_max_threshold_field_name   = "yellow_max_threshold";
const string green_max_threshold_field_name    = "green_max_threshold";

const string wred_profile_field_name           = "wred_profile";
const string wred_red_enable_field_name        = "wred_red_enable";
const string wred_yellow_enable_field_name     = "wred_yellow_enable";
const string wred_green_enable_field_name      = "wred_green_enable";

const string scheduler_algo_type_field_name    = "type";
const string scheduler_algo_DWRR               = "DWRR";
const string scheduler_algo_WRR                = "WRR";
const string scheduler_algo_STRICT             = "STRICT";
const string scheduler_weight_field_name       = "weight";
const string scheduler_priority_field_name     = "priority";

const string ecn_field_name                    = "ecn";
const string ecn_none                          = "ecn_none";
const string ecn_red                           = "ecn_red";
const string ecn_yellow                        = "ecn_yellow";
const string ecn_yellow_red                    = "ecn_yellow_red";
const string ecn_green                         = "ecn_green";
const string ecn_green_red                     = "ecn_green_red";
const string ecn_green_yellow                  = "ecn_green_yellow";
const string ecn_all                           = "ecn_all";

class QosMapHandler
{
public:
    task_process_status processWorkItem(Consumer& consumer);
    virtual bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes) = 0;
    virtual void freeAttribResources(vector<sai_attribute_t> &attributes);
    virtual bool modifyQosItem(sai_object_id_t, vector<sai_attribute_t> &attributes);
    virtual sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes) = 0;//different for sub-classes
    virtual bool removeQosItem(sai_object_id_t sai_object);
};

class DscpToTcMapHandler : public QosMapHandler
{
public:
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes);
    sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes);
};

class TcToQueueMapHandler : public QosMapHandler
{
public:
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes);
    sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes);
};

class WredMapHandler : public QosMapHandler
{
public:
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes);
    void freeAttribResources(vector<sai_attribute_t> &attributes);
    sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes);
    bool modifyQosItem(sai_object_id_t sai_object, vector<sai_attribute_t> &attribs);
    bool removeQosItem(sai_object_id_t sai_object);
protected:
    bool convertEcnMode(string str, sai_ecn_mark_mode_t &ecn_val);
    bool convertBool(string str, bool &val);
};


class TcToPgHandler : public QosMapHandler
{
public:
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes);
    sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes);
};

class PfcPrioToPgHandler : public QosMapHandler
{
public:
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes);
    sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes);
};

class PfcToQueueHandler : public QosMapHandler
{
public:
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, vector<sai_attribute_t> &attributes);
    sai_object_id_t addQosItem(const vector<sai_attribute_t> &attributes);
};

class QosOrch : public Orch
{
public:
    QosOrch(DBConnector *db, vector<string> &tableNames);

    static type_map& getTypeMap();
    static type_map m_qos_maps;
private:
    virtual void doTask(Consumer& consumer);

    typedef task_process_status (QosOrch::*qos_table_handler)(Consumer& consumer);
    typedef map<string, qos_table_handler> qos_table_handler_map;
    typedef pair<string, qos_table_handler> qos_handler_pair;

    void initColorAcl();
    sai_object_id_t initSystemAclTable();
    void initAclEntryForEcn(sai_object_id_t acl_table_id, sai_uint32_t priority,
                            sai_uint8_t ecn_field, sai_uint8_t dscp_field, sai_int32_t color);

    void initTableHandlers();

    task_process_status handleDscpToTcTable(Consumer& consumer);
    task_process_status handlePfcPrioToPgTable(Consumer& consumer);
    task_process_status handlePfcToQueueTable(Consumer& consumer);
    task_process_status handlePortQosMapTable(Consumer& consumer);
    task_process_status handleTcToPgTable(Consumer& consumer);
    task_process_status handleTcToQueueTable(Consumer& consumer);
    task_process_status handleSchedulerTable(Consumer& consumer);
    task_process_status handleQueueTable(Consumer& consumer);
    task_process_status handleWredProfileTable(Consumer& consumer);

    bool applyMapToPort(Port &port, sai_attr_id_t attr_id, sai_object_id_t sai_dscp_to_tc_map);
    bool applySchedulerToQueueSchedulerGroup(Port &port, size_t queue_ind, sai_object_id_t scheduler_profile_id);
    bool applyWredProfileToQueue(Port &port, size_t queue_ind, sai_object_id_t sai_wred_profile);
    task_process_status ResolveMapAndApplyToPort(Port &port,sai_port_attr_t port_attr,
                                                 string field_name, KeyOpFieldsValuesTuple &tuple, string op);

private:
    qos_table_handler_map m_qos_handler_map;
};
#endif /* SWSS_QOSORCH_H */
