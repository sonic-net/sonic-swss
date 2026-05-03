#ifndef TAM_ORCH_H
#define TAM_ORCH_H

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "orch.h"

extern "C" {
#include "sai.h"
#include "saitam.h"
}

/* CONFIG_DB table names. Defined locally so tamorch is self-contained
 * — the matching CFG_TAM_* macros in sonic-swss-common/common/schema.h
 * are added in a coordinated PR. Keeping the literals here prevents a
 * circular submodule-bump dance for v1. */
#define TAM_INT_TABLE_NAME           "TAM_INT"
#define TAM_TRANSPORT_TABLE_NAME     "TAM_TRANSPORT"
#define TAM_REPORT_TABLE_NAME        "TAM_REPORT"
#define TAM_TELEMETRY_TABLE_NAME     "TAM_TELEMETRY"
#define TAM_TABLE_NAME               "TAM"
#define TAM_FLOW_TABLE_NAME          "TAM_FLOW"

/* TamOrch — manages SAI TAM objects for IFAv2 (In-band Network Telemetry)
 * on Spectrum-4. Maps CONFIG_DB rows to SAI_OBJECT_TYPE_TAM,
 * SAI_OBJECT_TYPE_TAM_INT, SAI_OBJECT_TYPE_TAM_TRANSPORT,
 * SAI_OBJECT_TYPE_TAM_REPORT, SAI_OBJECT_TYPE_TAM_TELEMETRY.
 *
 * IFAv2 is the only TAM-INT type implemented in mlnx_sai (see
 * mlnx_sai_tam.c:96-99). The schema is defined in
 * sonic-yang-models/yang-models/sonic-tam-int.yang.
 *
 * Global enable lives in DEVICE_METADATA|localhost.tam_int_enable —
 * tamorch refuses to push any SAI calls when this flag is false.
 *
 * v1 deployment topology — IMPORTANT for understanding what this
 * orchagent does and does NOT do:
 *
 *   IFAv2 modifies the original packet inline. The source switch
 *   inserts an IFAv2 shim, transit switches append metadata, and
 *   only the SINK emits a UDP/TCP report to a collector. v1 uses
 *   NIC-terminated IFAv2 (DOCA Flow on CX7/CX8/BF3/BF4 is the sink),
 *   so all switches in v1 are source + transit only — they never
 *   open a socket to a collector.
 *
 *   Therefore in v1:
 *     * doTaskTamReport, doTaskTamInt, doTaskTam     — push SAI.
 *     * doTaskTamTransport, doTaskTamTelemetry       — parse and
 *       validate but do not push SAI (v1 has no sink-on-switch).
 *     * doTaskTamFlow                                — parsed only;
 *       per-flow ACL programming via SAI_TAM_INT_ATTR_ACL_GROUP is
 *       deferred to M3.
 *
 *   The dead parsers are intentional, not stubbed-out work: they keep
 *   the orchagent's CONFIG_DB surface 1:1 with the YANG module so
 *   that M3 (switch-sink topology for non-IFA-aware receive NICs)
 *   can light them up without a schema rev. Reviewers expecting
 *   sink behavior in v1 should look for it in M3 instead.
 */
class TamOrch : public Orch
{
public:
    TamOrch(swss::DBConnector *config_db,
            const std::vector<std::string> &table_names);
    ~TamOrch() override;

    /* Public for unit-test introspection only. */
    sai_object_id_t getTamOid(const std::string &name) const;
    sai_object_id_t getTamIntOid(const std::string &name) const;

private:
    void doTask(Consumer &consumer) override;

    /* Per-table dispatch. Each returns a task_process_status:
     *   task_success      — done
     *   task_need_retry   — referenced object not yet present
     *   task_failed       — bad input or SAI error (will not retry)
     *   task_ignore       — unknown op
     */
    task_process_status doTaskTamReport(const std::string &op,
                                        const std::string &name,
                                        const std::vector<swss::FieldValueTuple> &values);
    task_process_status doTaskTamTransport(const std::string &op,
                                           const std::string &name,
                                           const std::vector<swss::FieldValueTuple> &values);
    task_process_status doTaskTamTelemetry(const std::string &op,
                                           const std::string &name,
                                           const std::vector<swss::FieldValueTuple> &values);
    task_process_status doTaskTamInt(const std::string &op,
                                     const std::string &name,
                                     const std::vector<swss::FieldValueTuple> &values);
    task_process_status doTaskTam(const std::string &op,
                                  const std::string &name,
                                  const std::vector<swss::FieldValueTuple> &values);
    task_process_status doTaskTamFlow(const std::string &op,
                                      const std::string &name,
                                      const std::vector<swss::FieldValueTuple> &values);

    /* SAI lifecycle helpers. */
    bool createSaiTamReport(const std::string &name,
                            const std::vector<swss::FieldValueTuple> &values,
                            sai_object_id_t &oid);
    bool createSaiTamTransport(const std::string &name,
                               const std::vector<swss::FieldValueTuple> &values,
                               sai_object_id_t &oid);
    bool createSaiTamTelemetry(const std::string &name,
                               const std::vector<swss::FieldValueTuple> &values,
                               sai_object_id_t &oid);
    bool createSaiTamInt(const std::string &name,
                        const std::vector<swss::FieldValueTuple> &values,
                        sai_object_id_t &oid);
    bool createSaiTam(const std::string &name,
                      const std::vector<swss::FieldValueTuple> &values,
                      sai_object_id_t &oid);

    bool removeSaiObject(sai_object_id_t oid, sai_object_type_t type);

    /* Global gate — pull from CONFIG_DB|DEVICE_METADATA|localhost. */
    bool isTamIntEnabled();

    /* Hardware capability check — returns false on SPC1/2/3 (the SAI
     * layer at mlnx_tam_int_enabled() also rejects, but checking here
     * gives a cleaner orchagent-side error). */
    bool isPlatformSupported();

    /* Caches: name → SAI OID. */
    std::unordered_map<std::string, sai_object_id_t> m_reportMap;
    std::unordered_map<std::string, sai_object_id_t> m_transportMap;
    std::unordered_map<std::string, sai_object_id_t> m_telemetryMap;
    std::unordered_map<std::string, sai_object_id_t> m_intMap;
    std::unordered_map<std::string, sai_object_id_t> m_tamMap;

    /* Cached enable flag — refreshed on each doTask iteration. */
    bool m_enabled = false;
    bool m_capability_checked = false;
    bool m_capable = false;
};

#endif /* TAM_ORCH_H */
