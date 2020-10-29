#ifndef SWSS_MACSECSORCH_H
#define SWSS_MACSECSORCH_H

#include "orch.h"

#include "portsorch.h"
#include "flex_counter_manager.h"

#include <dbconnector.h>
#include <swss/schema.h>

#include <map>
#include <string>
#include <vector>
#include <memory>

using namespace swss;

// AN is a 2 bit number, it can only be 0, 1, 2 or 3
#define MAX_SA_NUMBER (3)

using macsec_an_t = std::uint16_t;

class MACsecOrchContext;

class MACsecOrch : public Orch
{
    friend class MACsecOrchContext;
public:
    MACsecOrch(
        DBConnector *app_db,
        DBConnector *state_db,
        const std::vector<std::string> &tables,
        PortsOrch * port_orch);
    ~MACsecOrch();

private:
    void doTask(Consumer &consumer);

public:
    using TaskArgs = std::vector<FieldValueTuple>;

private:

    task_process_status updateMACsecPort(const std::string & port_name, const TaskArgs & port_attr);
    task_process_status disableMACsecPort(const std::string & port_name, const TaskArgs & port_attr);
    task_process_status updateEgressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status deleteEgressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status updateIngressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status deleteIngressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status updateEgressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);
    task_process_status deleteEgressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);
    task_process_status updateIngressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);
    task_process_status deleteIngressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);

    PortsOrch * m_port_orch;

    Table m_state_macsec_port;
    Table m_state_macsec_egress_sc;
    Table m_state_macsec_ingress_sc;
    Table m_state_macsec_egress_sa;
    Table m_state_macsec_ingress_sa;

    DBConnector         m_counter_db;
    Table               m_macsec_counters_map;
    FlexCounterManager  m_macsec_flex_counter_manager;

    struct ACLTable
    {
        sai_object_id_t         m_table_id;
        sai_object_id_t         m_eapol_packet_forward_entry_id;
        std::set<sai_uint32_t>  m_available_acl_priorities;
    };
    struct MACsecSC
    {
        macsec_an_t                             m_encoding_an;
        bool                                    m_xpn64_enable;
        sai_object_id_t                         m_sc_id;
        std::map<macsec_an_t, sai_object_id_t>  m_sa_ids;
        sai_object_id_t                         m_flow_id;
        sai_object_id_t                         m_entry_id;
        sai_uint32_t                            m_acl_priority;
    };
    struct MACsecPort
    {
        sai_object_id_t                     m_egress_port_id;
        sai_object_id_t                     m_ingress_port_id;
        sai_object_id_t                     m_egress_flow_id;
        sai_object_id_t                     m_ingress_flow_id;
        std::map<sai_uint64_t, MACsecSC>    m_egress_scs;
        std::map<sai_uint64_t, MACsecSC>    m_ingress_scs;
        ACLTable                            m_egress_acl_table;
        ACLTable                            m_ingress_acl_table;
        bool                                m_enable_encrypt;
        bool                                m_sci_in_sectag;
        bool                                m_enable;
    };
    struct MACsecObject
    {
        sai_object_id_t                                 m_egress_id;
        sai_object_id_t                                 m_ingress_id;
        map<std::string, std::shared_ptr<MACsecPort> >  m_macsec_ports;
        bool                                            m_sci_in_ingress_macsec_acl;
    };
    map<sai_object_id_t, MACsecObject>              m_macsec_objs;
    map<std::string, std::shared_ptr<MACsecPort> >  m_macsec_ports;

    /* MACsec Object */
    bool initMACsecObject(sai_object_id_t switch_id);
    bool deinitMACsecObject(sai_object_id_t switch_id);

    /* MACsec Port */
    bool createMACsecPort(
        MACsecPort &macsec_port,
        const std::string &port_name,
        const TaskArgs & port_attr,
        const MACsecObject &macsec_obj,
        sai_object_id_t line_port_id,
        sai_object_id_t switch_id);
    bool createMACsecPort(
        sai_object_id_t &macsec_port_id,
        sai_object_id_t line_port_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction);
    bool updateMACsecPort(MACsecPort &macsec_port, const TaskArgs & port_attr);
    bool deleteMACsecPort(
        const MACsecPort &macsec_port,
        const std::string &port_name,
        const MACsecObject &macsec_obj,
        sai_object_id_t line_port_id);
    bool deleteMACsecPort(sai_object_id_t macsec_port_id);

    /* MACsec Flow */
    bool createMACsecFlow(
        sai_object_id_t &flow_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction);
    bool deleteMACsecFlow(sai_object_id_t flow_id);

    /* MACsec SC */
    task_process_status updateMACsecSC(
        const std::string &port_sci,
        const TaskArgs &sc_attr,
        sai_macsec_direction_t direction);
    bool setEncodingAN(
        MACsecSC &sc,
        const TaskArgs &sc_attr,
        sai_macsec_direction_t direction);
    bool createMACsecSC(
        MACsecPort &macsec_port,
        const std::string &port_name,
        const TaskArgs &sc_attr,
        const MACsecObject &macsec_obj,
        sai_uint64_t sci,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction);
    bool createMACsecSC(
        sai_object_id_t &sc_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        sai_object_id_t flow_id,
        sai_uint64_t sci,
        sai_uint32_t ssci,
        bool send_sci,
        bool xpn64_enable);
    task_process_status deleteMACsecSC(
        const std::string &port_sci,
        sai_macsec_direction_t direction);
    bool deleteMACsecSC(sai_object_id_t sc_id);

    /* MACsec SA */
    task_process_status createMACsecSA(
        const std::string &port_sci_an,
        const TaskArgs &sa_attr,
        sai_macsec_direction_t direction);
    task_process_status deleteMACsecSA(
        const std::string &port_sci_an,
        sai_macsec_direction_t direction);
    bool createMACsecSA(
        sai_object_id_t &sa_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        sai_object_id_t sc_id,
        macsec_an_t an,
        bool encryption_enable,
        bool sak_256_bit,
        sai_macsec_sak_t sak,
        bool xpn64_enable,
        sai_macsec_salt_t salt,
        sai_macsec_auth_key_t auth_key,
        sai_uint64_t pn);
    bool deleteMACsecSA(sai_object_id_t sa_id);

    /* Counter */
    void installCounter(
        CounterType counter_type,
        const std::string &obj_name,
        sai_object_id_t obj_id,
        const std::vector<std::string> &stats);
    void uninstallCounter(const std::string &obj_name, sai_object_id_t obj_id);

    /* ACL */
    bool initACLTable(
        ACLTable &acl_table,
        sai_object_id_t port_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        bool sci_in_sectag);
    bool deinitACLTable(
        const ACLTable &acl_table,
        sai_object_id_t port_id,
        sai_macsec_direction_t direction);
    bool createACLTable(
        sai_object_id_t &table_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        bool sci_in_sectag);
    bool deleteACLTable(sai_object_id_t table_id);
    bool bindACLTabletoPort(sai_object_id_t table_id, sai_object_id_t port_id, sai_macsec_direction_t direction);
    bool unbindACLTable(sai_object_id_t port_id, sai_macsec_direction_t direction);
    bool createACLEAPOLEntry(
        sai_object_id_t &entry_id,
        sai_object_id_t table_id,
        sai_object_id_t switch_id);
    bool createACLDataEntry(
        sai_object_id_t &entry_id,
        sai_object_id_t table_id,
        sai_object_id_t switch_id,
        bool sci_in_sectag,
        sai_uint64_t sci,
        sai_uint32_t priority);
    bool setACLEntryMACsecFlowActive(
        sai_object_id_t entry_id,
        sai_object_id_t flow_id,
        bool active);
    bool deleteACLEntry(sai_object_id_t entry_id);
    bool get_acl_maximum_priority(
        sai_object_id_t switch_id,
        sai_uint32_t &priority) const;
    bool get_acl_minimum_priority(
        sai_object_id_t switch_id,
        sai_uint32_t &priority) const;
};

#endif  // ORCHAGENT_MACSECORCH_H_
