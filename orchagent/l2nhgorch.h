/*
 * Copyright 2024 GlobalLogic.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __L2NHGORCH_H__
#define __L2NHGORCH_H__

#include "orch.h"
#include "observer.h"

class L2NhgOrch : public Orch
{
public:
    L2NhgOrch(DBConnector *db, const vector<string> &tableNames);

    ~L2NhgOrch();

    static uint64_t m_max_group_count;
    string getL2EcmpGroupPortName(const std::string& nhg_id);
    /*
     * Check if the given next hop group index exists.
     */
    inline bool hasL2EcmpGroup(const std::string &nhg_id) const
    {
        SWSS_LOG_ENTER();
        return m_nhg.find(nhg_id) != m_nhg.end();
    }

private:

    struct l2nh_info
    {
        string ip;
        int refcnt;
    };
    unordered_map<string, l2nh_info> m_nh;

    struct l2nhg_info
    {
        map<string, sai_object_id_t> hops;
        sai_object_id_t oid;
    };
    unordered_map<string, l2nhg_info> m_nhg;

    void doTask(Consumer &consumer);
    void doL2NhgTask(Consumer &consumer);

    bool addL2NexthopGroup(string nhg_id, string nh_ids);
    bool delL2Nexthop(string nhid);
    bool delL2NexthopGroup(string nhg_id);

    sai_status_t addL2EcmpGroup(sai_object_id_t &oid);
    sai_status_t addL2EcmpGroupMember(sai_object_id_t l2_ecmp_group_id,
                                 sai_object_id_t tunnel_id, sai_object_id_t &oid);
    void delL2EcmpGroup(sai_object_id_t l2_ecmp_group_id);
    void delL2EcmpGroupMember(sai_object_id_t member_id);
};


#endif /* __L2NHGORCH_H__ */
