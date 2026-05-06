#include "tamorch.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include <arpa/inet.h>

#include "logger.h"
#include "portsorch.h"
#include "rediscommand.h"
#include "schema.h"
#include "table.h"

extern "C" {
#include "sai.h"
#include "saiacl.h"
#include "saitam.h"
#include "saitypes.h"
}

extern sai_tam_api_t        *sai_tam_api;
extern sai_acl_api_t        *sai_acl_api;
extern sai_object_id_t       gSwitchId;
extern PortsOrch             *gPortsOrch;

using std::string;
using std::vector;
using swss::DBConnector;
using swss::FieldValueTuple;
using swss::Table;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

namespace {

bool getField(const vector<FieldValueTuple> &values,
              const string &name, string &out)
{
    for (const auto &fv : values)
    {
        if (fvField(fv) == name)
        {
            out = fvValue(fv);
            return true;
        }
    }
    return false;
}

bool parseBool(const string &s, bool &out)
{
    if (s == "true" || s == "TRUE" || s == "True" || s == "1")
    {
        out = true;
        return true;
    }
    if (s == "false" || s == "FALSE" || s == "False" || s == "0")
    {
        out = false;
        return true;
    }
    return false;
}

bool parseUint(const string &s, uint64_t &out)
{
    try
    {
        out = std::stoull(s);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

/* List value parser: SONiC stores leaf-list as a comma-separated string,
 * e.g. "IFAV2_IPFIX,DROP_REPORT". */
vector<string> parseList(const string &s)
{
    vector<string> out;
    string cur;
    for (char c : s)
    {
        if (c == ',')
        {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

sai_int32_t lookupReportType(const string &s)
{
    if (s == "ipfix")        return SAI_TAM_REPORT_TYPE_IPFIX;
    if (s == "vendor_extn")  return SAI_TAM_REPORT_TYPE_VENDOR_EXTN;
    if (s == "sflow")        return SAI_TAM_REPORT_TYPE_SFLOW;
    if (s == "proto")        return SAI_TAM_REPORT_TYPE_PROTO;
    if (s == "histogram")    return SAI_TAM_REPORT_TYPE_HISTOGRAM;
    if (s == "genetlink")    return SAI_TAM_REPORT_TYPE_GENETLINK;
    return -1;
}

/* Reserved for M3 — used by the future SAI_TAM_TRANSPORT path. */
[[maybe_unused]] sai_int32_t lookupTransportType(const string &s)
{
    if (s == "udp")    return SAI_TAM_TRANSPORT_TYPE_UDP;
    if (s == "tcp")    return SAI_TAM_TRANSPORT_TYPE_TCP;
    if (s == "gre")    return SAI_TAM_TRANSPORT_TYPE_GRE;
    if (s == "mirror") return SAI_TAM_TRANSPORT_TYPE_MIRROR;
    if (s == "none")   return SAI_TAM_TRANSPORT_TYPE_NONE;
    return -1;
}

sai_int32_t lookupBindPoint(const string &s)
{
    if (s == "switch") return SAI_TAM_BIND_POINT_TYPE_SWITCH;
    if (s == "port")   return SAI_TAM_BIND_POINT_TYPE_PORT;
    if (s == "lag")    return SAI_TAM_BIND_POINT_TYPE_LAG;
    if (s == "vlan")   return SAI_TAM_BIND_POINT_TYPE_VLAN;
    if (s == "queue")  return SAI_TAM_BIND_POINT_TYPE_QUEUE;
    return -1;
}

/* TODO(M3): wire src_ip / dst_ip / src_mac / dst_mac into
 * SAI_TAM_TRANSPORT_ATTR_SRC_IP / DST_IP / SRC_MAC_ADDRESS /
 * DST_MAC_ADDRESS. Today the v1 transport relies on the loopback IP
 * + ARP-resolved next-hop MAC the OS already programs, which is
 * sufficient for the IPFIX-to-collector flow on Spectrum-X. */

} /* anonymous namespace */

/* ------------------------------------------------------------------ */
/* Construction                                                       */
/* ------------------------------------------------------------------ */

TamOrch::TamOrch(DBConnector *config_db,
                 const vector<string> &table_names)
    : Orch(config_db, table_names)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("TamOrch initialized for tables: %zu", table_names.size());
}

TamOrch::~TamOrch()
{
    SWSS_LOG_ENTER();

    /* Tear down in reverse-dependency order. ACL first (references TAM_INT). */
    destroyTamIntAcl();
    for (auto &kv : m_tamMap)        removeSaiObject(kv.second, SAI_OBJECT_TYPE_TAM);
    for (auto &kv : m_telemetryMap)  removeSaiObject(kv.second, SAI_OBJECT_TYPE_TAM_TELEMETRY);
    for (auto &kv : m_intMap)        removeSaiObject(kv.second, SAI_OBJECT_TYPE_TAM_INT);
    for (auto &kv : m_transportMap)  removeSaiObject(kv.second, SAI_OBJECT_TYPE_TAM_TRANSPORT);
    for (auto &kv : m_reportMap)     removeSaiObject(kv.second, SAI_OBJECT_TYPE_TAM_REPORT);
}

sai_object_id_t TamOrch::getTamOid(const string &name) const
{
    auto it = m_tamMap.find(name);
    return it == m_tamMap.end() ? SAI_NULL_OBJECT_ID : it->second;
}

sai_object_id_t TamOrch::getTamIntOid(const string &name) const
{
    auto it = m_intMap.find(name);
    return it == m_intMap.end() ? SAI_NULL_OBJECT_ID : it->second;
}

/* ------------------------------------------------------------------ */
/* Capability + enable gates                                          */
/* ------------------------------------------------------------------ */

bool TamOrch::isPlatformSupported()
{
    if (m_capability_checked)
    {
        return m_capable;
    }

    m_capability_checked = true;

    sai_attr_capability_t cap{};
    sai_status_t status = sai_query_attribute_capability(
        gSwitchId,
        SAI_OBJECT_TYPE_TAM_INT,
        SAI_TAM_INT_ATTR_TYPE,
        &cap);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("TAM_INT not supported on this platform "
                      "(sai_query_attribute_capability rc=%d). IFAv2 disabled.",
                      status);
        m_capable = false;
        return false;
    }

    m_capable = cap.create_implemented;
    if (!m_capable)
    {
        SWSS_LOG_WARN("TAM_INT_ATTR_TYPE not creatable on this platform "
                      "(create=%d). IFAv2 disabled.",
                      cap.create_implemented);
    }
    else
    {
        SWSS_LOG_NOTICE("TAM_INT capability OK (create=%d set=%d get=%d) "
                        "— IFAv2 supported on this platform.",
                        cap.create_implemented, cap.set_implemented,
                        cap.get_implemented);
    }

    return m_capable;
}

bool TamOrch::isTamIntEnabled()
{
    DBConnector cfgDb("CONFIG_DB", 0);
    Table dmTable(&cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);

    string value;
    if (!dmTable.hget("localhost", "tam_int_enable", value))
    {
        return false;  /* default off */
    }

    bool b = false;
    if (!parseBool(value, b))
    {
        SWSS_LOG_WARN("DEVICE_METADATA|localhost.tam_int_enable has bad value '%s', treating as off",
                      value.c_str());
        return false;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* doTask                                                             */
/* ------------------------------------------------------------------ */

void TamOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    /* Refresh enable flag once per dispatch — orchagent calls
     * doTask repeatedly, this is cheap and lets `config reload`
     * flip the gate without an orchagent restart. */
    m_enabled = isTamIntEnabled();

    if (!m_enabled)
    {
        /* When disabled, drain the queue without touching SAI. We do not
         * tear down already-installed TAM objects on flag transition —
         * that path needs explicit operator action via `config tam-int
         * teardown` to avoid clobbering live telemetry on a config typo. */
        consumer.m_toSync.clear();
        return;
    }

    if (!isPlatformSupported())
    {
        consumer.m_toSync.clear();
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;
        const string key = kfvKey(t);
        const string op  = kfvOp(t);
        const auto values = kfvFieldsValues(t);

        const string table_name = consumer.getTableName();

        task_process_status status = task_process_status::task_ignore;

        try
        {
            if      (table_name == TAM_REPORT_TABLE_NAME)    status = doTaskTamReport(op, key, values);
            else if (table_name == TAM_TRANSPORT_TABLE_NAME) status = doTaskTamTransport(op, key, values);
            else if (table_name == TAM_TELEMETRY_TABLE_NAME) status = doTaskTamTelemetry(op, key, values);
            else if (table_name == TAM_INT_TABLE_NAME)       status = doTaskTamInt(op, key, values);
            else if (table_name == TAM_TABLE_NAME)           status = doTaskTam(op, key, values);
            else if (table_name == TAM_FLOW_TABLE_NAME)      status = doTaskTamFlow(op, key, values);
            else
            {
                SWSS_LOG_ERROR("TamOrch: unknown table '%s'", table_name.c_str());
            }
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("TamOrch: exception while processing %s|%s: %s",
                           table_name.c_str(), key.c_str(), e.what());
            status = task_process_status::task_failed;
        }

        if (status == task_process_status::task_need_retry)
        {
            ++it;  /* leave in queue for next pass */
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

/* ------------------------------------------------------------------ */
/* TAM_REPORT                                                         */
/* ------------------------------------------------------------------ */

bool TamOrch::createSaiTamReport(const string &name,
                                 const vector<FieldValueTuple> &values,
                                 sai_object_id_t &oid)
{
    string type_str = "ipfix";
    getField(values, "type", type_str);

    sai_int32_t type = lookupReportType(type_str);
    if (type < 0)
    {
        SWSS_LOG_ERROR("TAM_REPORT '%s': unknown type '%s'",
                       name.c_str(), type_str.c_str());
        return false;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr{};

    attr.id = SAI_TAM_REPORT_ATTR_TYPE;
    attr.value.s32 = type;
    attrs.push_back(attr);

    string s;
    if (getField(values, "ipfix_template_interval", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_REPORT_ATTR_TEMPLATE_REPORT_INTERVAL;
            attr.value.u32 = static_cast<uint32_t>(v);
            attrs.push_back(attr);
        }
    }
    if (getField(values, "ipfix_enterprise_id", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_REPORT_ATTR_ENTERPRISE_NUMBER;
            attr.value.u32 = static_cast<uint32_t>(v);
            attrs.push_back(attr);
        }
    }

    sai_status_t st = sai_tam_api->create_tam_report(
        &oid, gSwitchId,
        static_cast<uint32_t>(attrs.size()), attrs.data());
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("create_tam_report '%s' failed rc=%d", name.c_str(), st);
        return false;
    }
    SWSS_LOG_NOTICE("TAM_REPORT '%s' created OID=0x%" PRIx64,
                    name.c_str(), oid);
    return true;
}

task_process_status TamOrch::doTaskTamReport(const string &op,
                                             const string &name,
                                             const vector<FieldValueTuple> &values)
{
    if (op == SET_COMMAND)
    {
        auto it = m_reportMap.find(name);
        if (it != m_reportMap.end())
        {
            /* TODO: support attribute updates. For v1, require delete+recreate. */
            SWSS_LOG_WARN("TAM_REPORT '%s' already exists; updates not yet supported, "
                          "delete and re-create to change attributes.", name.c_str());
            return task_process_status::task_success;
        }
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (!createSaiTamReport(name, values, oid))
        {
            return task_process_status::task_failed;
        }
        m_reportMap[name] = oid;
        return task_process_status::task_success;
    }
    if (op == DEL_COMMAND)
    {
        auto it = m_reportMap.find(name);
        if (it == m_reportMap.end())
        {
            return task_process_status::task_success;
        }
        if (!removeSaiObject(it->second, SAI_OBJECT_TYPE_TAM_REPORT))
        {
            return task_process_status::task_failed;
        }
        m_reportMap.erase(it);
        return task_process_status::task_success;
    }
    return task_process_status::task_ignore;
}

/* ------------------------------------------------------------------ */
/* TAM_TRANSPORT                                                      */
/*                                                                    */
/* v1 scope: parse and cache only. SAI_OBJECT_TYPE_TAM_TRANSPORT is    */
/* part of the richer SAI_TAM_COLLECTOR / SAI_TAM_TELEMETRY plumbing  */
/* that lands in M3 of the IFAv2 visibility milestone. The proven v1  */
/* path is the same 3-object dance PortsOrch uses for Path Tracing:   */
/* TAM_REPORT → TAM_INT → TAM. Reports flow over the platform's       */
/* default telemetry plumbing (sx_api_tele.h on Spectrum-X).          */
/* ------------------------------------------------------------------ */

bool TamOrch::createSaiTamTransport(const string &name,
                                    const vector<FieldValueTuple> &values,
                                    sai_object_id_t &oid)
{
    (void)values;
    SWSS_LOG_NOTICE("TAM_TRANSPORT '%s' parsed; SAI_TAM_TRANSPORT object "
                    "creation deferred to M3 (collector / telemetry plumbing)",
                    name.c_str());
    oid = SAI_NULL_OBJECT_ID;
    return true;
}

task_process_status TamOrch::doTaskTamTransport(const string &op,
                                                const string &name,
                                                const vector<FieldValueTuple> &values)
{
    if (op == SET_COMMAND)
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        createSaiTamTransport(name, values, oid);
        m_transportMap[name] = oid;  /* stub OID for future reference */
        return task_process_status::task_success;
    }
    if (op == DEL_COMMAND)
    {
        m_transportMap.erase(name);
        return task_process_status::task_success;
    }
    return task_process_status::task_ignore;
}

/* ------------------------------------------------------------------ */
/* TAM_TELEMETRY                                                      */
/*                                                                    */
/* v1 scope: parse + cache only. The full SAI mapping requires        */
/* SAI_OBJECT_TYPE_TAM_COLLECTOR (which pairs IP+MAC+TRANSPORT)       */
/* and SAI_OBJECT_TYPE_TAM_TELEMETRY (which bundles collectors and    */
/* TAM_TEL_TYPEs). Per the M3 milestone plan, this lands once we've   */
/* validated the simpler path (TAM_REPORT + TAM_INT + TAM) on real    */
/* Spectrum-X hardware. Documented in 06-poc-test-plan.md T13a/T13b.  */
/* ------------------------------------------------------------------ */

bool TamOrch::createSaiTamTelemetry(const string &name,
                                    const vector<FieldValueTuple> &values,
                                    sai_object_id_t &oid)
{
    (void)values;
    SWSS_LOG_NOTICE("TAM_TELEMETRY '%s' parsed; SAI_TAM_COLLECTOR + SAI_TAM_TELEMETRY "
                    "object creation deferred to M3 milestone",
                    name.c_str());
    oid = SAI_NULL_OBJECT_ID;
    return true;
}

task_process_status TamOrch::doTaskTamTelemetry(const string &op,
                                                const string &name,
                                                const vector<FieldValueTuple> &values)
{
    if (op == SET_COMMAND)
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        createSaiTamTelemetry(name, values, oid);
        m_telemetryMap[name] = oid;
        return task_process_status::task_success;
    }
    if (op == DEL_COMMAND)
    {
        m_telemetryMap.erase(name);
        return task_process_status::task_success;
    }
    return task_process_status::task_ignore;
}

/* ------------------------------------------------------------------ */
/* TAM_INT                                                            */
/* ------------------------------------------------------------------ */

bool TamOrch::createSaiTamInt(const string &name,
                              const vector<FieldValueTuple> &values,
                              sai_object_id_t &oid)
{
    string type_str = "ifa2";
    getField(values, "type", type_str);
    if (type_str != "ifa2")
    {
        SWSS_LOG_ERROR("TAM_INT '%s': type='%s' not supported on Spectrum-4 "
                       "(only 'ifa2' is implemented in mlnx_sai)",
                       name.c_str(), type_str.c_str());
        return false;
    }

    string report_name;
    sai_object_id_t report_oid = SAI_NULL_OBJECT_ID;
    if (getField(values, "report", report_name) && !report_name.empty())
    {
        auto r_it = m_reportMap.find(report_name);
        if (r_it == m_reportMap.end())
        {
            SWSS_LOG_INFO("TAM_INT '%s': report '%s' not yet ready, retry",
                          name.c_str(), report_name.c_str());
            return false;
        }
        report_oid = r_it->second;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr{};

    attr.id = SAI_TAM_INT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_INT_TYPE_IFA2;
    attrs.push_back(attr);

    uint64_t device_id_val = 0;
    string s;
    if (getField(values, "device_id", s) && parseUint(s, device_id_val))
    {
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_INT_ATTR_DEVICE_ID;
        attr.value.u32 = static_cast<uint32_t>(device_id_val);
        attrs.push_back(attr);
    }
    else
    {
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_INT_ATTR_DEVICE_ID;
        attr.value.u32 = 0;
        attrs.push_back(attr);
    }

    attr = sai_attribute_t{};
    attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_TYPE;
    attr.value.s32 = SAI_TAM_INT_PRESENCE_TYPE_L3_PROTOCOL;
    attrs.push_back(attr);

    attr = sai_attribute_t{};
    attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_L3_PROTOCOL;
    attr.value.u8 = 0x7F;
    attrs.push_back(attr);

    attr = sai_attribute_t{};
    attr.id = SAI_TAM_INT_ATTR_INLINE;
    bool inl = true;
    if (getField(values, "inline", s)) parseBool(s, inl);
    attr.value.booldata = inl;
    attrs.push_back(attr);

    if (getField(values, "max_hop_count", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_INT_ATTR_MAX_HOP_COUNT;
            attr.value.u8 = static_cast<uint8_t>(v);
            attrs.push_back(attr);
        }
    }
    if (getField(values, "flow_liveness_period", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_INT_ATTR_FLOW_LIVENESS_PERIOD;
            attr.value.u16 = static_cast<uint16_t>(v);
            attrs.push_back(attr);
        }
    }
    if (getField(values, "latency_sensitivity", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_INT_ATTR_LATENCY_SENSITIVITY;
            attr.value.u8 = static_cast<uint8_t>(v);
            attrs.push_back(attr);
        }
    }
    if (getField(values, "metadata_checksum_enable", s))
    {
        bool b = false; parseBool(s, b);
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_INT_ATTR_METADATA_CHECKSUM_ENABLE;
        attr.value.booldata = b;
        attrs.push_back(attr);
    }
    if (getField(values, "metadata_fragment_enable", s))
    {
        bool b = true; parseBool(s, b);
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_INT_ATTR_METADATA_FRAGMENT_ENABLE;
        attr.value.booldata = b;
        attrs.push_back(attr);
    }
    if (getField(values, "report_all_packets", s))
    {
        bool b = false; parseBool(s, b);
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_INT_ATTR_REPORT_ALL_PACKETS;
        attr.value.booldata = b;
        attrs.push_back(attr);
    }
    if (getField(values, "trace_vector", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_INT_ATTR_TRACE_VECTOR;
            attr.value.u16 = static_cast<uint16_t>(v);
            attrs.push_back(attr);
        }
    }
    if (getField(values, "action_vector", s))
    {
        uint64_t v = 0;
        if (parseUint(s, v))
        {
            attr = sai_attribute_t{};
            attr.id = SAI_TAM_INT_ATTR_ACTION_VECTOR;
            attr.value.u16 = static_cast<uint16_t>(v);
            attrs.push_back(attr);
        }
    }
    if (report_oid != SAI_NULL_OBJECT_ID)
    {
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_INT_ATTR_REPORT_ID;
        attr.value.oid = report_oid;
        attrs.push_back(attr);
    }

    sai_status_t st = sai_tam_api->create_tam_int(
        &oid, gSwitchId,
        static_cast<uint32_t>(attrs.size()), attrs.data());
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("create_tam_int '%s' failed rc=%d", name.c_str(), st);
        return false;
    }
    SWSS_LOG_NOTICE("TAM_INT '%s' created OID=0x%" PRIx64 " (IFAv2)",
                    name.c_str(), oid);
    return true;
}

task_process_status TamOrch::doTaskTamInt(const string &op,
                                          const string &name,
                                          const vector<FieldValueTuple> &values)
{
    if (op == SET_COMMAND)
    {
        if (m_intMap.count(name))
        {
            SWSS_LOG_WARN("TAM_INT '%s' already exists; updates not yet supported.",
                          name.c_str());
            return task_process_status::task_success;
        }
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (!createSaiTamInt(name, values, oid))
        {
            string report;
            if (getField(values, "report", report) && !report.empty()
                && !m_reportMap.count(report))
            {
                return task_process_status::task_need_retry;
            }
            return task_process_status::task_failed;
        }
        m_intMap[name] = oid;
        return task_process_status::task_success;
    }
    if (op == DEL_COMMAND)
    {
        auto it = m_intMap.find(name);
        if (it == m_intMap.end()) return task_process_status::task_success;
        if (!removeSaiObject(it->second, SAI_OBJECT_TYPE_TAM_INT))
        {
            return task_process_status::task_failed;
        }
        m_intMap.erase(it);
        return task_process_status::task_success;
    }
    return task_process_status::task_ignore;
}

/* ------------------------------------------------------------------ */
/* TAM (top-level handle)                                             */
/* ------------------------------------------------------------------ */

bool TamOrch::createSaiTam(const string &name,
                           const vector<FieldValueTuple> &values,
                           sai_object_id_t &oid)
{
    /* Resolve int_objects + telemetry_objects + bind_points. */
    vector<sai_object_id_t> int_oids;
    vector<sai_object_id_t> telemetry_oids;
    vector<sai_int32_t>     bind_points;

    string s;
    if (getField(values, "int_objects", s))
    {
        for (const auto &n : parseList(s))
        {
            auto it = m_intMap.find(n);
            if (it == m_intMap.end())
            {
                SWSS_LOG_INFO("TAM '%s': int_object '%s' not yet ready, retry",
                              name.c_str(), n.c_str());
                return false;
            }
            int_oids.push_back(it->second);
        }
    }
    if (getField(values, "telemetry_objects", s))
    {
        /* Validate references but do not push to SAI in v1. Once
         * createSaiTamTelemetry actually creates SAI_TAM_COLLECTOR +
         * SAI_TAM_TELEMETRY objects (M3 milestone), the OIDs here will
         * become non-NULL and we'll start passing them to SAI_TAM_ATTR_
         * TELEMETRY_OBJECTS_LIST. */
        for (const auto &n : parseList(s))
        {
            auto it = m_telemetryMap.find(n);
            if (it == m_telemetryMap.end())
            {
                SWSS_LOG_INFO("TAM '%s': telemetry_object '%s' not yet ready, retry",
                              name.c_str(), n.c_str());
                return false;
            }
            if (it->second != SAI_NULL_OBJECT_ID)
            {
                telemetry_oids.push_back(it->second);
            }
        }
    }
    if (getField(values, "bind_points", s))
    {
        for (const auto &n : parseList(s))
        {
            sai_int32_t bp = lookupBindPoint(n);
            if (bp < 0)
            {
                SWSS_LOG_ERROR("TAM '%s': unknown bind_point '%s'",
                               name.c_str(), n.c_str());
                return false;
            }
            bind_points.push_back(bp);
        }
    }

    /* Honor admin_state — only push to SAI when up. */
    string admin_state = "down";
    getField(values, "admin_state", admin_state);
    if (admin_state != "up")
    {
        SWSS_LOG_NOTICE("TAM '%s' admin_state=%s — caching config only, not pushing to SAI.",
                        name.c_str(), admin_state.c_str());
        oid = SAI_NULL_OBJECT_ID;
        return true;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr{};

    if (!int_oids.empty())
    {
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_ATTR_INT_OBJECTS_LIST;
        attr.value.objlist.count = static_cast<uint32_t>(int_oids.size());
        attr.value.objlist.list  = int_oids.data();
        attrs.push_back(attr);
    }
    if (!telemetry_oids.empty())
    {
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_ATTR_TELEMETRY_OBJECTS_LIST;
        attr.value.objlist.count = static_cast<uint32_t>(telemetry_oids.size());
        attr.value.objlist.list  = telemetry_oids.data();
        attrs.push_back(attr);
    }
    if (!bind_points.empty())
    {
        attr = sai_attribute_t{};
        attr.id = SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST;
        attr.value.s32list.count = static_cast<uint32_t>(bind_points.size());
        attr.value.s32list.list  = bind_points.data();
        attrs.push_back(attr);
    }

    sai_status_t st = sai_tam_api->create_tam(
        &oid, gSwitchId,
        static_cast<uint32_t>(attrs.size()), attrs.data());
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("create_tam '%s' failed rc=%d", name.c_str(), st);
        return false;
    }
    SWSS_LOG_NOTICE("TAM '%s' created OID=0x%" PRIx64
                    " (int=%zu telemetry=%zu bind_points=%zu)",
                    name.c_str(), oid,
                    int_oids.size(), telemetry_oids.size(), bind_points.size());
    return true;
}

task_process_status TamOrch::doTaskTam(const string &op,
                                       const string &name,
                                       const vector<FieldValueTuple> &values)
{
    if (op == SET_COMMAND)
    {
        if (m_tamMap.count(name))
        {
            SWSS_LOG_WARN("TAM '%s' already exists; updates not yet supported.",
                          name.c_str());
            return task_process_status::task_success;
        }
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (!createSaiTam(name, values, oid))
        {
            return task_process_status::task_need_retry;
        }
        if (oid != SAI_NULL_OBJECT_ID)
        {
            m_tamMap[name] = oid;

            /* Activate IFAv2 on all ports via ACL (pages 7-8 of NVIDIA
             * IFA/INT overview).  Use the first entry from int_objects
             * (the same field createSaiTam resolved) to find the TAM_INT
             * OID for the ACL entry's ACTION_TAM_INT_OBJECT. */
            string int_objs_str;
            if (getField(values, "int_objects", int_objs_str) && !int_objs_str.empty())
            {
                auto refs = parseList(int_objs_str);
                if (!refs.empty())
                {
                    auto it = m_intMap.find(refs[0]);
                    if (it != m_intMap.end() && it->second != SAI_NULL_OBJECT_ID)
                    {
                        if (!createTamIntAcl(it->second))
                        {
                            SWSS_LOG_WARN("TAM '%s': ACL activation failed; "
                                          "IFAv2 metadata insertion will not work "
                                          "until ACL is created", name.c_str());
                        }
                    }
                }
            }
        }
        return task_process_status::task_success;
    }
    if (op == DEL_COMMAND)
    {
        auto it = m_tamMap.find(name);
        if (it == m_tamMap.end()) return task_process_status::task_success;
        if (!removeSaiObject(it->second, SAI_OBJECT_TYPE_TAM))
        {
            return task_process_status::task_failed;
        }
        m_tamMap.erase(it);
        return task_process_status::task_success;
    }
    return task_process_status::task_ignore;
}

/* ------------------------------------------------------------------ */
/* TAM INT ACL — ACL-based activation of IFAv2 (pages 7-8)           */
/*                                                                    */
/* Creates an ACL TABLE with SAI_ACL_TABLE_ATTR_FIELD_TAM_INT_TYPE,   */
/* adds it to every physical port's ingress ACL group, and creates a  */
/* single ACL ENTRY that matches IFA2 packets and triggers metadata   */
/* insertion via ACTION_INT_INSERT + ACTION_TAM_INT_OBJECT.           */
/* ------------------------------------------------------------------ */

bool TamOrch::createTamIntAcl(sai_object_id_t tam_int_oid)
{
    if (m_tamIntAclTableId != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("TAM INT ACL already created");
        return true;
    }

    sai_status_t st;

    /* --- 1. Create ACL TABLE with TAM INT field --- */
    sai_attribute_t tbl_attrs[3];

    tbl_attrs[0].id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    tbl_attrs[0].value.s32 = SAI_ACL_STAGE_INGRESS;

    tbl_attrs[1].id = SAI_ACL_TABLE_ATTR_FIELD_TAM_INT_TYPE;
    tbl_attrs[1].value.booldata = true;

    sai_int32_t bp_types[] = {SAI_ACL_BIND_POINT_TYPE_PORT, SAI_ACL_BIND_POINT_TYPE_LAG};
    tbl_attrs[2].id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    tbl_attrs[2].value.s32list.count = 2;
    tbl_attrs[2].value.s32list.list = bp_types;

    st = sai_acl_api->create_acl_table(&m_tamIntAclTableId, gSwitchId, 3, tbl_attrs);
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM INT ACL table: rc=%d", st);
        return false;
    }
    SWSS_LOG_NOTICE("TAM INT ACL table created: OID=0x%" PRIx64, m_tamIntAclTableId);

    /* --- 2. Add table to each port's ingress ACL group --- */
    if (gPortsOrch)
    {
        auto &ports = gPortsOrch->getAllPorts();
        for (auto &kv : ports)
        {
            Port &port = kv.second;
            if (port.m_type != Port::PHY) continue;
            if (port.m_ingress_acl_table_group_id == SAI_NULL_OBJECT_ID) continue;

            sai_object_id_t member_id = SAI_NULL_OBJECT_ID;
            sai_attribute_t mem_attrs[3];

            mem_attrs[0].id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID;
            mem_attrs[0].value.oid = port.m_ingress_acl_table_group_id;

            mem_attrs[1].id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID;
            mem_attrs[1].value.oid = m_tamIntAclTableId;

            mem_attrs[2].id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY;
            mem_attrs[2].value.u32 = 100;

            st = sai_acl_api->create_acl_table_group_member(&member_id, gSwitchId, 3, mem_attrs);
            if (st != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_WARN("Failed to add TAM INT ACL table to group 0x%" PRIx64
                              " for port %s: rc=%d",
                              port.m_ingress_acl_table_group_id,
                              port.m_alias.c_str(), st);
                continue;
            }
            m_tamIntAclGroupMemberIds.push_back(member_id);
            SWSS_LOG_NOTICE("TAM INT ACL group member 0x%" PRIx64 " for port %s",
                            member_id, port.m_alias.c_str());
        }
    }

    if (m_tamIntAclGroupMemberIds.empty())
    {
        SWSS_LOG_WARN("No ports bound to TAM INT ACL — metadata insertion will not activate");
    }

    /* --- 3. Create ACL ENTRY with TAM INT match + actions --- */
    sai_attribute_t entry_attrs[5];
    int attr_count = 0;

    entry_attrs[attr_count].id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    entry_attrs[attr_count].value.oid = m_tamIntAclTableId;
    attr_count++;

    entry_attrs[attr_count].id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    entry_attrs[attr_count].value.u32 = 1000;
    attr_count++;

    entry_attrs[attr_count].id = SAI_ACL_ENTRY_ATTR_FIELD_TAM_INT_TYPE;
    entry_attrs[attr_count].value.aclfield.enable = true;
    entry_attrs[attr_count].value.aclfield.data.s32 = SAI_TAM_INT_TYPE_IFA2;
    entry_attrs[attr_count].value.aclfield.mask.s32 = 0xFFFFFFFF;
    attr_count++;

    entry_attrs[attr_count].id = SAI_ACL_ENTRY_ATTR_ACTION_INT_INSERT;
    entry_attrs[attr_count].value.aclaction.enable = true;
    entry_attrs[attr_count].value.aclaction.parameter.booldata = true;
    attr_count++;

    entry_attrs[attr_count].id = SAI_ACL_ENTRY_ATTR_ACTION_TAM_INT_OBJECT;
    entry_attrs[attr_count].value.aclaction.enable = true;
    entry_attrs[attr_count].value.aclaction.parameter.oid = tam_int_oid;
    attr_count++;

    st = sai_acl_api->create_acl_entry(&m_tamIntAclEntryId, gSwitchId, attr_count, entry_attrs);
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM INT ACL entry: rc=%d", st);
        destroyTamIntAcl();
        return false;
    }
    SWSS_LOG_NOTICE("TAM INT ACL entry created: OID=0x%" PRIx64
                    " (TAM_INT=0x%" PRIx64 ", %zu port bindings)",
                    m_tamIntAclEntryId, tam_int_oid,
                    m_tamIntAclGroupMemberIds.size());
    return true;
}

void TamOrch::destroyTamIntAcl()
{
    if (m_tamIntAclEntryId != SAI_NULL_OBJECT_ID)
    {
        sai_acl_api->remove_acl_entry(m_tamIntAclEntryId);
        m_tamIntAclEntryId = SAI_NULL_OBJECT_ID;
    }
    for (auto mid : m_tamIntAclGroupMemberIds)
    {
        sai_acl_api->remove_acl_table_group_member(mid);
    }
    m_tamIntAclGroupMemberIds.clear();
    if (m_tamIntAclTableId != SAI_NULL_OBJECT_ID)
    {
        sai_acl_api->remove_acl_table(m_tamIntAclTableId);
        m_tamIntAclTableId = SAI_NULL_OBJECT_ID;
    }
}

/* ------------------------------------------------------------------ */
/* TAM_FLOW — per-flow ACL programming                                */
/* ------------------------------------------------------------------ */

task_process_status TamOrch::doTaskTamFlow(const string &op,
                                           const string &name,
                                           const vector<FieldValueTuple> &values)
{
    /* TAM_FLOW translates 5-tuple match rules into an ACL_TABLE +
     * ACL_RULE bound to the TAM_INT instance via
     * SAI_TAM_INT_ATTR_ACL_GROUP. v1 logs and ignores — implementation
     * is tracked as M3 of the IFAv2 visibility milestone (see
     * engineering-notes/network-visibility-int-ifa/00-proposal.md).
     *
     * The CONFIG_DB rows are still validated against YANG, so once
     * this is implemented existing fixtures will not need changes. */
    (void)values;
    SWSS_LOG_NOTICE("TAM_FLOW '%s' op=%s — not yet pushed to SAI (M3 milestone)",
                    name.c_str(), op.c_str());
    return task_process_status::task_success;
}

/* ------------------------------------------------------------------ */
/* SAI removal helper                                                 */
/* ------------------------------------------------------------------ */

bool TamOrch::removeSaiObject(sai_object_id_t oid, sai_object_type_t type)
{
    if (oid == SAI_NULL_OBJECT_ID) return true;

    sai_status_t st = SAI_STATUS_SUCCESS;
    switch (type)
    {
    case SAI_OBJECT_TYPE_TAM:           st = sai_tam_api->remove_tam(oid);           break;
    case SAI_OBJECT_TYPE_TAM_INT:       st = sai_tam_api->remove_tam_int(oid);       break;
    case SAI_OBJECT_TYPE_TAM_TELEMETRY: st = sai_tam_api->remove_tam_telemetry(oid); break;
    case SAI_OBJECT_TYPE_TAM_TRANSPORT: st = sai_tam_api->remove_tam_transport(oid); break;
    case SAI_OBJECT_TYPE_TAM_REPORT:    st = sai_tam_api->remove_tam_report(oid);    break;
    default:
        SWSS_LOG_ERROR("removeSaiObject: unsupported object type %d", type);
        return false;
    }
    if (st != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM object OID=0x%" PRIx64 " type=%d rc=%d",
                       oid, type, st);
        return false;
    }
    return true;
}
