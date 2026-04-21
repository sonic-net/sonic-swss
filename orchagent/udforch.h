#ifndef SWSS_UDFORCH_H
#define SWSS_UDFORCH_H

#include "orch.h"
#include "udf_constants.h"

extern "C" {
#include "sai.h"
}

#include <map>
#include <string>
#include <memory>
#include <tuple>

using namespace std;
using namespace swss;

struct UdfGroupConfig
{
    string name;
    sai_udf_group_type_t type;
    uint16_t length;
};

struct UdfMatchConfig
{
    string name;
    uint16_t l2_type      = 0;
    uint16_t l2_type_mask = 0;
    bool     l2_type_set  = false;
    uint8_t  l3_type      = 0;
    uint8_t  l3_type_mask = 0;
    bool     l3_type_set  = false;
    uint16_t gre_type      = 0;
    uint16_t gre_type_mask = 0;
    bool     gre_type_set  = false;
    uint16_t l4_dst_port      = 0;
    uint16_t l4_dst_port_mask = 0;
    bool     l4_dst_port_set  = false;
    uint8_t  priority = 0;
};

struct UdfConfig
{
    string name;
    sai_object_id_t match_id;
    sai_object_id_t group_id;
    sai_udf_base_t base;
    uint16_t offset;
};

class UdfGroup
{
public:
    UdfGroup(const UdfGroupConfig& config);
    ~UdfGroup();

    sai_status_t create();
    bool remove();

    sai_object_id_t getOid() const { return m_oid; }
    const string& getName() const { return m_config.name; }
    const UdfGroupConfig& getConfig() const { return m_config; }

private:
    UdfGroupConfig m_config;
    sai_object_id_t m_oid;
};

class UdfMatch
{
public:
    UdfMatch(const UdfMatchConfig& config);
    ~UdfMatch();

    bool create();
    bool remove();

    sai_object_id_t getOid() const { return m_oid; }
    const string& getName() const { return m_config.name; }
    const UdfMatchConfig& getConfig() const { return m_config; }

private:
    UdfMatchConfig m_config;
    sai_object_id_t m_oid;
};

class Udf
{
public:
    Udf(const UdfConfig& config);
    ~Udf();

    bool create();
    bool remove();

    sai_object_id_t getOid() const { return m_oid; }
    const string& getName() const { return m_config.name; }
    const UdfConfig& getConfig() const { return m_config; }

private:
    UdfConfig m_config;
    sai_object_id_t m_oid;
};

/* Dedup key for shared UDF_MATCH objects — selectors with identical match
   criteria share a single SAI object, ref-counted. */

struct UdfMatchSignature
{
    uint16_t l2_type      = 0;
    uint16_t l2_type_mask = 0;
    bool     l2_type_set  = false;
    uint8_t  l3_type      = 0;
    uint8_t  l3_type_mask = 0;
    bool     l3_type_set  = false;
    uint16_t gre_type      = 0;
    uint16_t gre_type_mask = 0;
    bool     gre_type_set  = false;
    uint16_t l4_dst_port      = 0;
    uint16_t l4_dst_port_mask = 0;
    bool     l4_dst_port_set  = false;
    uint8_t  priority      = 0;

    bool operator<(const UdfMatchSignature& o) const
    {
        return tie(l2_type, l2_type_mask, l2_type_set, l3_type, l3_type_mask, l3_type_set,
                   gre_type, gre_type_mask, gre_type_set, l4_dst_port, l4_dst_port_mask, l4_dst_port_set, priority)
             < tie(o.l2_type, o.l2_type_mask, o.l2_type_set, o.l3_type, o.l3_type_mask, o.l3_type_set,
                   o.gre_type, o.gre_type_mask, o.gre_type_set, o.l4_dst_port, o.l4_dst_port_mask, o.l4_dst_port_set, o.priority);
    }

    bool operator==(const UdfMatchSignature& o) const
    {
        return tie(l2_type, l2_type_mask, l2_type_set, l3_type, l3_type_mask, l3_type_set,
                   gre_type, gre_type_mask, gre_type_set, l4_dst_port, l4_dst_port_mask, l4_dst_port_set, priority)
            == tie(o.l2_type, o.l2_type_mask, o.l2_type_set, o.l3_type, o.l3_type_mask, o.l3_type_set,
                   o.gre_type, o.gre_type_mask, o.gre_type_set, o.l4_dst_port, o.l4_dst_port_mask, o.l4_dst_port_set, o.priority);
    }
};

class UdfOrch : public Orch
{
public:
    UdfOrch(DBConnector *configDb, const vector<string> &tableNames);
    ~UdfOrch();

    bool addUdfGroup(const string& name, const UdfGroupConfig& config, sai_status_t* saiStatus = nullptr);
    bool removeUdfGroup(const string& name);
    UdfGroup* getUdfGroup(const string& name);
    sai_object_id_t getUdfGroupOid(const string& name);

    bool addUdfMatch(const string& name, const UdfMatchConfig& config);
    bool removeUdfMatch(const string& name);
    UdfMatch* getUdfMatch(const string& name);

    bool addUdf(const string& name, const UdfConfig& config);
    bool removeUdf(const string& name);
    Udf* getUdf(const string& name);

    void incrementGroupRefCount(const string& groupName);
    void decrementGroupRefCount(const string& groupName);

    void incrementUdfRuleRefCount(const string& udfName);
    void decrementUdfRuleRefCount(const string& udfName);

private:
    void doTask(Consumer& consumer) override;

    void doUdfFieldTask(Consumer& consumer);
    void doUdfSelectorTask(Consumer& consumer);

    UdfMatchSignature buildMatchSignature(const UdfMatchConfig& config);
    string makeSharedMatchName(const UdfMatchSignature& sig) const;
    string getOrCreateSharedMatch(const UdfMatchSignature& sig, const UdfMatchConfig& config);
    void releaseSharedMatch(const string& matchName);

    void flushStaleAsicUdfObjects();
    void probeUdfSupport();

    sai_object_id_t getUdfMatchOid(const string& name);
    uint32_t getGroupRefCount(const string& groupName) const;
    uint32_t getUdfRuleRefCount(const string& udfName) const;

    map<string, unique_ptr<UdfGroup>> m_udfGroups;
    map<string, unique_ptr<UdfMatch>> m_udfMatches;
    map<string, unique_ptr<Udf>> m_udfs;

    map<UdfMatchSignature, string> m_matchSigToName;
    map<string, uint32_t> m_matchRefCount;

    map<string, string> m_selectorToMatchName;

    map<string, uint32_t> m_udfGroupRefCount;
    map<string, uint32_t> m_udfRuleRefCount;

    bool m_udfSupported = false;
};

extern UdfOrch* gUdfOrch;

#endif /* SWSS_UDFORCH_H */
