#ifndef SWSS_L2NHGORCH_H
#define SWSS_L2NHGORCH_H

#include <vector>
#include <nhgorch.h>

#include "orch.h"
#include "observer.h"

class L2NhgOrch : public NhgOrchCommon<NextHopGroup>
{
public:
    L2NhgOrch(DBConnector* appDbConnector,string appL2NhgTable);

    ~L2NhgOrch();

    /*
     * Getters.
     */
    string getNextHopGroupPortName(const std::string& nhg_id);
    unsigned long getL2NhgCount();
    unsigned long getNumL2NhgNextHops(const std::string &nhg_id);
    unsigned long getL2NhVtepRefCount(const std::string &nhg_id);
    bool hasActiveL2Nhg(const std::string &nhg_id);
    bool isL2NextHop(const std::string &nhg_id);

private:
    vector<Table*> m_appTables;

    struct l2nhg_vtep_info
    {
        string ip;
        int ref_count;
        string source_vtep;
    };
    unordered_map<string, l2nhg_vtep_info> m_nhg_vtep;

    /*
     * Structure to store Next hop related SAI OIDs
     */
    struct NhIds
    {
        sai_object_id_t nhgm_oid;
        sai_object_id_t nh_oid;
    };

    struct l2nhg_nh_info
    {
        map<string, NhIds> next_hops;
        sai_object_id_t oid;
        bool is_active = false; // True only when bridge port exists
        string source_vtep;
    };
    unordered_map<string, l2nhg_nh_info> m_nhg_nh;

    bool deleteL2NextHop(string nhg_id);
    bool deleteL2NextHopGroup(string nhg_id);
    bool addL2NextHopGroupEntry(string nhg_id, string nh_ids, string source_vtep = "");

    bool removeSaiNextHop(NhIds nh_ids);
    pair<sai_object_id_t, sai_object_id_t> createSaiNextHop(sai_object_id_t l2_nhg_id,
                                                            sai_object_id_t tunnel_id,
                                                            const string& remote_vtep_ip);
    bool removeSaiNextHopGroup(sai_object_id_t l2_nhg_id);
    sai_object_id_t createSaiNextHopGroup();

    bool deleteL2Nhg(string& key, Consumer& consumer);
    bool updateL2Nhg(string& key, KeyOpFieldsValuesTuple& t, Consumer& consumer);
    bool updateL2NhgVtepIp(string nh_id, string new_vtep_ip);

    void doL2NhgTask(Consumer &consumer);
    void doTask(Consumer &consumer);
};


#endif /* SWSS_L2NHGORCH_H */
