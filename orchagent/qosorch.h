#ifndef SWSS_QOSORCH_H
#define SWSS_QOSORCH_H

#include <map>
#include "orch.h"
#include "portsorch.h"

namespace swss {

const std::string delimiter                         = ":";
const std::string tc_to_queue_field_name            = "tc_to_queue_map";
const std::string dscp_to_tc_field_name             = "dscp_to_tc_map";
const std::string scheduler_field_name              = "scheduler";
const std::string wred_profile_field_name           = "wred_profile";
const char        ref_start                         = '[';
const char        ref_end                           = '[';
const std::string yellow_max_threshold_field_name   = "yellow_max_threshold";
const std::string green_max_threshold_field_name    = "green_max_threshold";

const std::string scheduler_algo_type_field_name    = "type";
const std::string scheduler_algo_DWRR               = "DWRR";
const std::string scheduler_algo_WRR                = "WRR";
const std::string scheduler_algo_PRIORITY           = "PRIORITY";
const std::string scheduler_weight_field            = "weight";
const std::string scheduler_priority_field          = "priority";

typedef std::map<string, sai_object_id_t> qos_object_map;
typedef std::pair<string, sai_object_id_t> qos_object_map_pair;

typedef std::map<string, qos_object_map*> qos_type_map;
typedef std::pair<string, qos_object_map*> qos_type_map_pair;


class QosMapHandler
{
public:
    bool processWorkItem(Consumer& consumer);
    virtual bool isValidTable(string &tableName) = 0;
    virtual bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, std::vector<sai_attribute_t> &attributes) = 0;
    virtual void freeAttribResources(std::vector<sai_attribute_t> &attributes) = 0;
    virtual bool modifyExistingSaiObject(sai_object_id_t, std::vector<sai_attribute_t> &attributes) = 0;
    virtual sai_object_id_t createSaiObject(std::vector<sai_attribute_t> &attributes) = 0;
    virtual bool deleteSaiObject(sai_object_id_t sai_object) = 0;
};

class DscpToTcMapHandler : public QosMapHandler
{
public:
    bool isValidTable(string &tableName);
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, std::vector<sai_attribute_t> &attributes);
    void freeAttribResources(std::vector<sai_attribute_t> &attributes);
    bool modifyExistingSaiObject(sai_object_id_t, std::vector<sai_attribute_t> &attributes);
    sai_object_id_t createSaiObject(std::vector<sai_attribute_t> &attributes);
    bool deleteSaiObject(sai_object_id_t sai_object);
};

class TcToQueueMapHandler : public QosMapHandler
{
public:
    bool isValidTable(string &tableName);
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, std::vector<sai_attribute_t> &attributes);
    void freeAttribResources(std::vector<sai_attribute_t> &attributes);
    bool modifyExistingSaiObject(sai_object_id_t, std::vector<sai_attribute_t> &attributes);
    sai_object_id_t createSaiObject(std::vector<sai_attribute_t> &attributes);
    bool deleteSaiObject(sai_object_id_t sai_object);
};

class WredMapHandler : public QosMapHandler
{
public:
    bool isValidTable(string &tableName);
    bool convertFieldValuesToAttributes(KeyOpFieldsValuesTuple &tuple, std::vector<sai_attribute_t> &attributes);
    void freeAttribResources(std::vector<sai_attribute_t> &attributes);
    bool modifyExistingSaiObject(sai_object_id_t, std::vector<sai_attribute_t> &attributes);
    sai_object_id_t createSaiObject(std::vector<sai_attribute_t> &attributes);
    bool deleteSaiObject(sai_object_id_t sai_object);
};

class QosOrch : public Orch
{
public:
    
    QosOrch(DBConnector *db, vector<string> &tableNames, PortsOrch *portsOrch);
    
    typedef enum {
        success,
        field_not_found,
        multiple_instances,
        failure
    }resolve_status;

    static qos_type_map& getTypeMap();
    static qos_type_map m_qos_type_maps;
private:
    virtual void doTask(Consumer& consumer);

    typedef bool (QosOrch::*qos_table_handler)(Consumer& consumer);
    typedef std::map<std::string, qos_table_handler> qos_table_handler_map;
    typedef std::pair<string, qos_table_handler> qos_handler_pair;
    
    void initTableHandlers();
    bool handleDscpToTcTable(Consumer& consumer);
    bool handleTcToQueueTable(Consumer& consumer);
    bool handleSchedulerTable(Consumer& consumer);
    bool handleQueueTable(Consumer& consumer);
    bool applyObjectToQueue(
        Port                &port,
        size_t              queue_ind,
        sai_queue_attr_t    queue_attr,
        sai_object_id_t     sai_object);
    bool handlePortQosMapTable(Consumer& consumer);
    bool applyMapToPort(Port &port, sai_attr_id_t attr_id, sai_object_id_t sai_dscp_to_tc_map);
    bool handleWredProfileTable(Consumer& consumer);
    resolve_status resolveFieldRefValue(
        const string            &field_name, 
        KeyOpFieldsValuesTuple  &tuple, 
        sai_object_id_t         &sai_object);
    
    bool parseReference(string &ref, string &table_name, string &object_name);
    bool tokenizeString(string str, const string &separator, vector<string> &tokens);

private:    
    PortsOrch *m_portsOrch;
    qos_table_handler_map m_qos_handler_map;
};

}
#endif /* SWSS_QOSORCH_H */
