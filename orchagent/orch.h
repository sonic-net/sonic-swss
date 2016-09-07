#ifndef SWSS_ORCH_H
#define SWSS_ORCH_H

#include <map>
#include <assert.h>

extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include "dbconnector.h"
#include "consumertable.h"
#include "producertable.h"
#include "ipaddress.h"
#include "ipprefix.h"

using namespace std;
using namespace swss;

const char delimiter           = ':';
const char list_item_delimiter = ',';

typedef enum
{
    task_success,
    task_invalid_entry,
    task_failed,
    task_need_retry,
    task_ignore
} task_process_status;

typedef std::map<string, sai_object_id_t> object_map;
typedef std::pair<string, sai_object_id_t> object_map_pair;
typedef map<string, KeyOpFieldsValuesTuple> SyncMap;
struct Consumer {
    Consumer(ConsumerTable* consumer) :m_consumer(consumer)  { }
    ConsumerTable* m_consumer;
    /* Store the latest 'golden' status */
    SyncMap m_toSync;
};
typedef std::pair<string, Consumer> ConsumerMapPair;
typedef map<string, Consumer> ConsumerMap;

class Orch
{
public:
    Orch(DBConnector *db, string tableName);
    Orch(DBConnector *db, vector<string> &tableNames);
    virtual ~Orch();

    std::vector<Selectable*> getSelectables();
    bool hasSelectable(ConsumerTable* s) const;

    bool execute(string tableName);
    /* Iterate all consumers in m_consumerMap and run doTask(Consumer) */
    void doTask();
protected:
    /* Run doTask against a specific consumer */
    virtual void doTask(Consumer &consumer) = 0;
    void dumpTuple(Consumer &consumer, KeyOpFieldsValuesTuple &tuple);
    
    
    inline static sai_ip_address_t& copy(sai_ip_address_t& dst, const ip_addr_t& src)
    {
        switch(src.family)
        {
            case AF_INET:
                dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
                dst.addr.ip4 = src.ip_addr.ipv4_addr;
                break;
            case AF_INET6:
                dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
                memcpy(dst.addr.ip6, src.ip_addr.ipv6_addr, 16);
                break;
            default:
                assert(false); // unreachable code
        }
        return dst;
    }
    
    inline static sai_ip_address_t& copy(sai_ip_address_t& dst, const IpAddress& src)
    {
        return copy(dst, src.getIp());
    }
    
    inline static sai_ip_prefix_t& copy(sai_ip_prefix_t& dst, const IpPrefix& src)
    {
        ip_addr_t ia = src.getIp().getIp();
        ip_addr_t ma = src.getMask().getIp();
        switch(ia.family)
        {
            case AF_INET:
                dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
                dst.addr.ip4 = ia.ip_addr.ipv4_addr;
                dst.mask.ip4 = ma.ip_addr.ipv4_addr;
                break;
            case AF_INET6:
                dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
                memcpy(dst.addr.ip6, ia.ip_addr.ipv6_addr, 16);
                memcpy(dst.mask.ip6, ma.ip_addr.ipv6_addr, 16);
                break;
            default:
                assert(false); // unreachable code
        }
        return dst;
    }
    
    inline static sai_ip_prefix_t& copy(sai_ip_prefix_t& dst, const ip_addr_t& src)
    {
        switch(src.family)
        {
            case AF_INET:
                dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
                dst.addr.ip4 = src.ip_addr.ipv4_addr;
                dst.mask.ip4 = 0xFFFFFFFF;
                break;
            case AF_INET6:
                dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
                memcpy(dst.addr.ip6, src.ip_addr.ipv6_addr, 16);
                memset(dst.mask.ip6, 0xFF, 16);
                break;
            default:
                assert(false); // unreachable code
        }
        return dst;
    }
    
    inline static sai_ip_prefix_t& copy(sai_ip_prefix_t& dst, const IpAddress& src)
    {
        return copy(dst, src.getIp());
    }
    
    inline static sai_ip_prefix_t& subnet(sai_ip_prefix_t& dst, const sai_ip_prefix_t& src)
    {
        dst.addr_family = src.addr_family;
        switch(src.addr_family)
        {
            case SAI_IP_ADDR_FAMILY_IPV4:
                dst.addr.ip4 = src.addr.ip4 & src.mask.ip4;
                dst.mask.ip4 = 0xFFFFFFFF;
                break;
            case SAI_IP_ADDR_FAMILY_IPV6:
                for (size_t i = 0; i < 16; i++)
                {
                    dst.addr.ip6[i] = src.addr.ip6[i] & src.mask.ip6[i];
                    dst.mask.ip6[i] = 0xFF;
                }
                break;
            default:
                assert(false); // unreachable code
        }
        return dst;
    }
    
private:
    DBConnector *m_db;

protected:
    ConsumerMap m_consumerMap;

};

#endif /* SWSS_ORCH_H */
