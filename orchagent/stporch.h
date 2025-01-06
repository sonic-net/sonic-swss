#ifndef SWSS_STPORCH_H
#define SWSS_STPORCH_H

#include <map>
#include <set>
#include "orch.h"

#define STP_INVALID_INSTANCE 0xFFFF

typedef enum _stp_state
{
    STP_STATE_DISABLED				= 0,
	STP_STATE_BLOCKING				= 1,
	STP_STATE_LISTENING				= 2,
	STP_STATE_LEARNING				= 3,
	STP_STATE_FORWARDING			= 4,
	STP_STATE_INVALID               = 5
}stp_state;


class StpOrch : public Orch
{
public:
    StpOrch(DBConnector *db, DBConnector *stateDb, vector<string> &tableNames);
    bool stpVlanFdbFlush(string vlan_alias);
    bool updateMaxStpInstance(uint32_t max_stp_instance);
    bool removeStpPorts(Port &port);
    bool removeVlanFromStpInstance(string vlan, sai_uint16_t stp_instance);

private:
    unique_ptr<Table> m_stpTable;
    std::map<sai_uint16_t, sai_object_id_t> m_stpInstToOid;//Mapping from STP instance id to corresponding object id
    sai_object_id_t m_defaultStpId;
    
    void doStpTask(Consumer &consumer);
    void doStpPortStateTask(Consumer &consumer);
    void doStpFastageTask(Consumer &consumer);
    void doStpVlanIntfFlushTask(Consumer &consumer);
    
    sai_object_id_t addStpInstance(sai_uint16_t stp_instance);
    bool removeStpInstance(sai_uint16_t stp_instance);
    bool addVlanToStpInstance(string vlan, sai_uint16_t stp_instance);
    sai_object_id_t getStpInstanceOid(sai_uint16_t stp_instance);
    
    sai_object_id_t addStpPort(Port &port, sai_uint16_t stp_instance);
    bool removeStpPort(Port &port, sai_uint16_t stp_instance);
    sai_stp_port_state_t getStpSaiState(sai_uint8_t stp_state);
    bool updateStpPortState(Port &port, sai_uint16_t stp_instance, sai_uint8_t stp_state);

    void doTask(Consumer& consumer);
};
#endif /* SWSS_STPORCH_H */

