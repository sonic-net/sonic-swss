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

    task_process_status taskUpdateMACsecPort(const std::string & port_name, const TaskArgs & port_attr);
    task_process_status taskDisableMACsecPort(const std::string & port_name, const TaskArgs & port_attr);
    task_process_status taskUpdateEgressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status taskDeleteEgressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status taskUpdateIngressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status taskDeleteIngressSC(const std::string & port_sci, const TaskArgs & sc_attr);
    task_process_status taskUpdateEgressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);
    task_process_status taskDeleteEgressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);
    task_process_status taskUpdateIngressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);
    task_process_status taskDeleteIngressSA(const std::string & port_sci_an, const TaskArgs & sa_attr);

    PortsOrch * m_port_orch;

    Table m_state_macsec_port;
    Table m_state_macsec_egress_sc;
    Table m_state_macsec_ingress_sc;
    Table m_state_macsec_egress_sa;
    Table m_state_macsec_ingress_sa;

    DBConnector         m_counter_db;
    Table               m_macsec_counters_map;
    Table               m_macsec_flow_tx_counters_map;
    Table               m_macsec_flow_rx_counters_map;
    Table               m_macsec_sa_tx_counters_map;
    Table               m_macsec_sa_rx_counters_map;
    Table               m_applPortTable;
    FlexCounterManager  m_macsec_sa_attr_manager;
    FlexCounterManager  m_macsec_sa_stat_manager;
    FlexCounterManager  m_macsec_flow_stat_manager;

    struct MACsecACLTable
    {
        sai_object_id_t         m_table_id;
        sai_object_id_t         m_eapol_packet_forward_entry_id;
        sai_object_id_t         m_pfc_entry_id;
        std::set<sai_uint32_t>  m_available_acl_priorities;
    };
    struct MACsecSC
    {
        macsec_an_t                             m_encoding_an;
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
        MACsecACLTable                      m_egress_acl_table;
        MACsecACLTable                      m_ingress_acl_table;
        sai_macsec_cipher_suite_t           m_cipher_suite;
        bool                                m_enable_encrypt;
        bool                                m_sci_in_sectag;
        bool                                m_enable;
        uint32_t                            m_original_ipg;
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
        sai_object_id_t port_id,
        sai_object_id_t switch_id,
        Port &port,
        const gearbox_phy_t* phy);
    bool createMACsecPort(
        sai_object_id_t &macsec_port_id,
        sai_object_id_t port_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction);
    bool updateMACsecPort(MACsecPort &macsec_port, const TaskArgs & port_attr);
    bool updateMACsecSCs(MACsecPort &macsec_port, std::function<bool(MACsecOrch::MACsecSC &)> action);
    bool deleteMACsecPort(
        const MACsecPort &macsec_port,
        const std::string &port_name,
        const MACsecObject &macsec_obj,
        sai_object_id_t port_id,
        Port &port,
        const gearbox_phy_t* phy);
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
        bool encryption_enable,
        bool send_sci,
        sai_macsec_cipher_suite_t cipher_suite);
    task_process_status deleteMACsecSC(
        const std::string &port_sci,
        sai_macsec_direction_t direction);
    bool deleteMACsecSC(sai_object_id_t sc_id);
    bool setMACsecSC(sai_object_id_t sc_id, const sai_attribute_t &attr);

    bool updateMACsecAttr(sai_object_type_t object_type, sai_object_id_t object_id, const sai_attribute_t &attr);

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
        sai_macsec_sak_t sak,
        sai_macsec_salt_t salt,
        sai_uint32_t ssci,
        sai_macsec_auth_key_t auth_key,
        sai_uint64_t pn);
    bool deleteMACsecSA(sai_object_id_t sa_id);

    /* Counter */
    void installCounter(
        CounterType counter_type,
        sai_macsec_direction_t direction,
        const std::string &obj_name,
        sai_object_id_t obj_id,
        const std::vector<std::string> &stats);
    void uninstallCounter(
        CounterType counter_type,
        sai_macsec_direction_t direction,
        const std::string &obj_name,
        sai_object_id_t obj_id);

    /* MACsec ACL */
    bool initMACsecACLTable(
        MACsecACLTable &acl_table,
        sai_object_id_t port_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        bool sci_in_sectag,
        const std::string &port_name,
        const gearbox_phy_t* phy);
    bool deinitMACsecACLTable(
        const MACsecACLTable &acl_table,
        sai_object_id_t port_id,
        sai_macsec_direction_t direction,
        const gearbox_phy_t* phy);
    bool createMACsecACLTable(
        sai_object_id_t &table_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        bool sci_in_sectag);
    bool deleteMACsecACLTable(sai_object_id_t table_id);
    bool bindMACsecACLTabletoPort(sai_object_id_t table_id, sai_object_id_t port_id, sai_macsec_direction_t direction);
    bool unbindMACsecACLTable(sai_object_id_t port_id, sai_macsec_direction_t direction);
    bool createMACsecACLEAPOLEntry(
        sai_object_id_t &entry_id,
        sai_object_id_t table_id,
        sai_object_id_t switch_id);
    bool createMACsecACLDataEntry(
        sai_object_id_t &entry_id,
        sai_object_id_t table_id,
        sai_object_id_t switch_id,
        bool sci_in_sectag,
        sai_uint64_t sci,
        sai_uint32_t priority);
    bool setMACsecFlowActive(
        sai_object_id_t entry_id,
        sai_object_id_t flow_id,
        bool active);
    bool deleteMACsecACLEntry(sai_object_id_t entry_id);
    bool getAclPriority(
        sai_object_id_t switch_id,
        sai_attr_id_t priority_id,
        sai_uint32_t &priority) const;

    /* PFC */
    bool setPFCForward(sai_object_id_t port_id, bool enable);
    bool createPFCEntry(sai_object_id_t &entry_id,
        sai_object_id_t table_id,
        sai_object_id_t switch_id,
        sai_macsec_direction_t direction,
        sai_uint32_t priority,
        const std::string &pfc_mode);
    sai_attribute_t identifyPFC() const;
    sai_attribute_t bypassPFC() const;
    sai_attribute_t dropPFC() const;

};

#endif  // ORCHAGENT_MACSECORCH_H_
