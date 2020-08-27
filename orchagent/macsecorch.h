#ifndef SWSS_MACSECSORCH_H
#define SWSS_MACSECSORCH_H

// The following definitions should be moved to schema.h

#define APP_MACSEC_PORT_TABLE_NAME          "MACSEC_PORT_TABLE"
#define APP_MACSEC_EGRESS_SC_TABLE_NAME     "MACSEC_EGRESS_SC_TABLE"
#define APP_MACSEC_INGRESS_SC_TABLE_NAME    "MACSEC_INGRESS_SC_TABLE"
#define APP_MACSEC_EGRESS_SA_TABLE_NAME     "MACSEC_EGRESS_SA_TABLE"
#define APP_MACSEC_INGRESS_SA_TABLE_NAME    "MACSEC_INGRESS_SA_TABLE"

#define STATE_MACSEC_PORT_TABLE_NAME        "MACSEC_PORT_TABLE"
#define STATE_MACSEC_EGRESS_SC_TABLE_NAME   "MACSEC_EGRESS_SC_TABLE"
#define STATE_MACSEC_INGRESS_SC_TABLE_NAME  "MACSEC_INGRESS_SC_TABLE"
#define STATE_MACSEC_EGRESS_SA_TABLE_NAME   "MACSEC_EGRESS_SA_TABLE"
#define STATE_MACSEC_INGRESS_SA_TABLE_NAME  "MACSEC_INGRESS_SA_TABLE"

#define COUNTERS_MACSEC_NAME_MAP            "COUNTERS_MACSEC_NAME_MAP"
#define COUNTERS_MACSEC_TABLE               "COUNTERS_MACSEC"

// End define

#include "orch.h"

#include "portsorch.h"

#include <dbconnector.h>

#include <string>
#include <vector>
#include <memory>

using namespace swss;

// AN is a 2 bit number, it can only be 0, 1, 2 or 3
#define MAX_SA_NUMBER (3)

class MACsecOrch : public Orch
{
public:
    MACsecOrch(DBConnector *app_db, DBConnector *state_db, const std::vector<std::string> &tables, PortsOrch * port_orch);
    ~MACsecOrch();

private:
    void doTask(Consumer &consumer);

public:
    using TaskArgs = std::vector<FieldValueTuple>;

private:

    task_process_status enableMACsecPort(const std::string & port_name, const TaskArgs & port_attr);
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

    Table m_gearbox_table;
    bool m_gearbox_enabled;
    map<int, gearbox_phy_t> m_gearbox_phy_map;
    map<int, gearbox_interface_t> m_gearbox_interface_map;
    struct MACsecSC
    {
        sai_uint8_t     m_encoding_an;
        bool            m_xpn64_enable;
        sai_object_id_t m_sc_id;
        sai_object_id_t m_sa_ids[MAX_SA_NUMBER + 1];
    };
    struct MACsecPort
    {
        sai_object_id_t                     m_egress_port_id;
        sai_object_id_t                     m_ingress_port_id;
        sai_object_id_t                     m_egress_flow_id;
        sai_object_id_t                     m_ingress_flow_id;
        std::map<sai_uint64_t, MACsecSC>    m_egress_scs;
        std::map<sai_uint64_t, MACsecSC>    m_ingress_scs;
        bool                                m_enable_encrypt;
    };
    struct MACsecObject
    {
        sai_object_id_t                                 m_egress_id;
        sai_object_id_t                                 m_ingress_id;
        map<std::string, std::shared_ptr<MACsecPort> >  m_ports;
    };
    map<sai_object_id_t, MACsecObject>              m_macsec_objs;
    map<std::string, std::shared_ptr<MACsecPort> >  m_macsec_ports;

    bool initGearbox();
    bool getGearboxSwitchId(const Port & port, sai_object_id_t & switch_id) const;
    bool getGearboxSwitchId(const std::string & port_name, sai_object_id_t & switch_id) const;

    map<sai_object_id_t, MACsecObject>::iterator initMACsecObject(sai_object_id_t switch_id);
    bool deinitMACsecObject(map<sai_object_id_t, MACsecObject>::iterator switch_id);
    bool deinitMACsecObject(sai_object_id_t switch_id);

    std::shared_ptr<MACsecPort> createMACsecPort(const Port & port, sai_object_id_t switch_id);
    bool deleteMACsecPort(const std::string & port_name);

    // bool createACL;
    // bool deleteACL;

    task_process_status updateMACsecSC(
        const std::string & port_sci,
        const TaskArgs & sc_attr,
        sai_int32_t direction);
    task_process_status deleteMACsecSC(
        const std::string & port_sci,
        const TaskArgs & sc_attr,
        sai_int32_t direction);
    bool createMACsecSC(
        sai_object_id_t & sc_id,
        sai_object_id_t switch_id,
        sai_int32_t direction,
        sai_object_id_t flow_id,
        sai_uint64_t sci,
        sai_uint32_t ssci,
        bool xpn64_enable);
    bool deleteMACsecSC(sai_object_id_t sc_id);

    task_process_status createMACsecSA(
        const std::string & port_sci_an,
        const TaskArgs & sa_attr,
        sai_int32_t direction);
    task_process_status deleteMACsecSA(
        const std::string & port_sci_an,
        const TaskArgs & sa_attr,
        sai_int32_t direction);
    bool createMACsecSA(
        sai_object_id_t & sa_id,
        sai_object_id_t switch_id,
        sai_int32_t direction,
        sai_object_id_t sc_id,
        sai_uint8_t an,
        bool encryption_enable,
        bool sak_256_bit,
        sai_macsec_sak_t sak,
        bool xpn64_enable,
        sai_macsec_salt_t salt,
        sai_macsec_auth_key_t auth_key,
        sai_uint64_t pn
        );
    bool deleteMACsecSA(sai_object_id_t sa_id);
};

#endif
