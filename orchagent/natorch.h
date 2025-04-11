/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SWSS_NATORCH_H
#define SWSS_NATORCH_H

#include "orch.h"
#include "orchnotificationconsumer.h"
#include "observer.h"
#include "portsorch.h"
#include "intfsorch.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "nfnetlink.h"
#include "timer.h"
#include "routeorch.h"
#include "nexthopgroupkey.h"
#include "notificationproducer.h"
#ifdef DEBUG_FRAMEWORK
#include "debugdumporch.h"
#endif

#define VALUES                            "Values" // Global Values Key
#define NAT_HITBIT_N_CNTRS_QUERY_PERIOD   5        // 5 secs
#define NAT_CONNTRACK_TIMEOUT_PERIOD      86400    // 1 day
#define NAT_HITBIT_QUERY_MULTIPLE         6        // Hit bits are queried every 30 secs

struct NatEntryValue
{
    IpAddress      translated_ip;      // Translated IP address
    string         nat_type;           // Nat Type - SNAT or DNAT
    string         entry_type;         // Entry type - Static or Dynamic 
    time_t         activeTime;         // Timestamp in secs when the entry was last seen as active
    time_t         ageOutTime;         // Timestamp in secs when the entry expires
    bool           addedToHw;          // Boolean to represent added to hardware

    bool operator<(const NatEntryValue& other) const
    {
        return tie(translated_ip, nat_type, entry_type) < tie(other.translated_ip, other.nat_type, other.entry_type);
    }
};

struct NaptEntryKey
{
    IpAddress      ip_address;      // IP address
    int            l4_port;         // Port address
    string         prototype;       // Prototype - TCP or UDP

    bool operator<(const NaptEntryKey& other) const
    {
        return tie(ip_address, l4_port, prototype) < tie(other.ip_address, other.l4_port, other.prototype);
    }
};

struct NaptEntryValue
{
    IpAddress      translated_ip;      // Translated IP address
    int            translated_l4_port; // Translated port address
    string         nat_type;           // Nat Type - SNAT or DNAT
    string         entry_type;         // Entry type - Static or Dynamic
    time_t         activeTime;         // Timestamp in secs when the entry was last seen as active
    time_t         ageOutTime;         // Timestamp in secs when the entry expires
    bool           addedToHw;          // Boolean to represent added to hardware

    bool operator<(const NaptEntryValue& other) const
    {
        return tie(translated_ip, translated_l4_port, nat_type, entry_type) < tie(other.translated_ip, other.translated_l4_port, other.nat_type, other.entry_type);
    }
};

struct TwiceNatEntryKey
{
    IpAddress      src_ip;
    IpAddress      dst_ip;

    bool operator<(const TwiceNatEntryKey& other) const
    {
        return tie(src_ip, dst_ip) < tie(other.src_ip, other.dst_ip);
    }
};

struct TwiceNatEntryValue
{
    IpAddress      translated_src_ip;
    IpAddress      translated_dst_ip;
    string         entry_type;         // Entry type - Static or Dynamic 
    time_t         activeTime;         // Timestamp in secs when the entry was last seen as active
    time_t         ageOutTime;         // Timestamp in secs when the entry expires
    bool           addedToHw;          // Boolean to represent added to hardware

    bool operator<(const TwiceNatEntryValue& other) const
    {
        return tie(translated_src_ip, translated_dst_ip, entry_type) < tie(other.translated_src_ip, other.translated_dst_ip, other.entry_type);
    }
};

struct TwiceNaptEntryKey
{
    IpAddress      src_ip;
    int            src_l4_port;
    IpAddress      dst_ip;
    int            dst_l4_port;
    string         prototype;

    bool operator<(const TwiceNaptEntryKey& other) const
    {
        return tie(src_ip, src_l4_port, dst_ip, dst_l4_port, prototype) < tie(other.src_ip, other.src_l4_port, other.dst_ip, other.dst_l4_port, other.prototype);
    }
};

struct TwiceNaptEntryValue
{
    IpAddress      translated_src_ip;
    int            translated_src_l4_port;
    IpAddress      translated_dst_ip;
    int            translated_dst_l4_port;
    string         entry_type;         // Entry type - Static or Dynamic
    time_t         activeTime;         // Timestamp in secs when the entry was last seen as active
    time_t         ageOutTime;         // Timestamp in secs when the entry expires
    bool           addedToHw;          // Boolean to represent added to hardware

    bool operator<(const TwiceNaptEntryValue& other) const
    {
        return tie(translated_src_ip, translated_src_l4_port, translated_dst_ip, translated_dst_l4_port, entry_type) < 
               tie(other.translated_src_ip, other.translated_src_l4_port, other.translated_dst_ip, other.translated_dst_l4_port, other.entry_type);
    }
};

/* Here Key is IpAddress (global address) and
 * NatEntryValue contains translated values (local address, nat_type, entry_type)
 */
typedef std::map<IpAddress, NatEntryValue> NatEntry;

/* Here Key is NaptEntryKey (global address, global port and prototype)
 * NaptEntryValue contains translated values (local address, local port, nat_type and entry_type)
 */
typedef std::map<NaptEntryKey, NaptEntryValue> NaptEntry;

typedef std::map<TwiceNatEntryKey, TwiceNatEntryValue> TwiceNatEntry;

typedef std::map<TwiceNaptEntryKey, TwiceNaptEntryValue> TwiceNaptEntry;

/* Cache of DNAT entries that are dependent on the 
 * nexthop resolution of the translated destination ip address.
 */
typedef std::set<NaptEntryKey> DnaptCache;
typedef std::set<TwiceNatEntryKey> TwiceNatCache;
typedef std::set<TwiceNaptEntryKey> TwiceNaptCache;

// Cache of DNAT Pool destIp 
typedef std::set<IpAddress> DnatPoolEntry;

struct DnatEntries
{
    IpAddress        dnatIp;       /* NAT entry cache */
    DnaptCache       dnapt;        /* NAPT entries cache */
    TwiceNatCache    twiceNat;     /* Twice NAT entries cache */
    TwiceNaptCache   twiceNapt;    /* Twice NAPT entries cache */

    NextHopGroupKey  nextHopGroup;
    bool             neighResolved;
};

typedef std::map<IpAddress, DnatEntries> DnatNhResolvCache;

class NatOrch: public Orch, public Subject, public Observer
{
public:

    NatOrch(DBConnector *appDb, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, RouteOrch *routeOrch, NeighOrch *neighOrch);

    ~NatOrch()
    {
        // do nothing
    }

    void update(SubjectType, void *);
    bool debugdumpCLI(KeyOpFieldsValuesTuple t);
    void debugdumpALL();

    NeighOrch *m_neighOrch;
    RouteOrch *m_routeOrch;

private:

    NatEntry                m_natEntries;
    NaptEntry               m_naptEntries;
    TwiceNatEntry           m_twiceNatEntries;
    TwiceNaptEntry          m_twiceNaptEntries;
    SelectableTimer        *m_natQueryTimer;
    SelectableTimer        *m_natTimeoutTimer;
    DBConnector             m_countersDb;
    Table                   m_countersNatTable;
    Table                   m_countersNaptTable;
    Table                   m_countersTwiceNatTable;
    Table                   m_countersTwiceNaptTable;
    Table                   m_countersGlobalNatTable;
    Table                   m_natQueryTable;
    Table                   m_naptQueryTable;
    Table                   m_twiceNatQueryTable;
    Table                   m_twiceNaptQueryTable;
    OrchNotificationConsumer   *m_flushNotificationsConsumer;
    OrchNotificationConsumer   *m_cleanupNotificationConsumer;
    mutex                   m_natMutex;
    string                  m_dbgCompName;
    IpAddress               nullIpv4Addr;
    DnatPoolEntry           m_dnatPoolEntries;

    std::shared_ptr<NotificationProducer> setTimeoutNotifier;

    /* DNAT/DNAPT entry is cached, to delete and re-add it whenever the direct NextHop (connected neighbor)
     * or indirect NextHop (via route) to reach the DNAT IP is changed. */
    DnatNhResolvCache       m_nhResolvCache;

    int              timeout;
    int              tcp_timeout;
    int              udp_timeout;
    int              totalEntries;
    int              totalStaticNatEntries;
    int              totalDynamicNatEntries;
    int              totalStaticTwiceNatEntries;
    int              totalDynamicTwiceNatEntries;
    int              totalStaticNaptEntries;
    int              totalDynamicNaptEntries;
    int              totalStaticTwiceNaptEntries;
    int              totalDynamicTwiceNaptEntries;
    int              totalSnatEntries;
    int              totalDnatEntries;
    int              maxAllowedSNatEntries;
    string           admin_mode;

    void doTask(Consumer& consumer);
    void doTask(SelectableTimer &timer);
    void doTask(OrchNotificationConsumer& consumer);
    void doNatTableTask(Consumer& consumer);
    void doNaptTableTask(Consumer& consumer);
    void doTwiceNatTableTask(Consumer& consumer);
    void doTwiceNaptTableTask(Consumer& consumer);
    void doNatGlobalTableTask(Consumer& consumer);
    void doDnatPoolTableTask(Consumer& consumer);

    bool addNatEntry(const IpAddress &ip_address, const NatEntryValue &entry);
    bool removeNatEntry(const IpAddress &ip_address);
    bool addNaptEntry(const NaptEntryKey &keyEntry, const NaptEntryValue &entry);
    bool removeNaptEntry(const NaptEntryKey &keyEntry);

    bool addTwiceNatEntry(const TwiceNatEntryKey &key, const TwiceNatEntryValue &value);
    bool removeTwiceNatEntry(const TwiceNatEntryKey &key);
    bool addTwiceNaptEntry(const TwiceNaptEntryKey &key, const TwiceNaptEntryValue &value);
    bool removeTwiceNaptEntry(const TwiceNaptEntryKey &key);

    void updateNextHop(const NextHopUpdate& update);
    void updateNeighbor(const NeighborUpdate& update);
    bool isNextHopResolved(const NextHopUpdate &update);
    void addNhCacheDnatEntries(const IpAddress &nhIp, bool add);
    void addDnatToNhCache(const IpAddress &translatedIp, const IpAddress &dstIp);
    void removeDnatFromNhCache(const IpAddress &translatedIp, const IpAddress &dstIp);
    void addDnaptToNhCache(const IpAddress &translatedIp, const NaptEntryKey &key);
    void removeDnaptFromNhCache(const IpAddress &translatedIp, const NaptEntryKey &key);
    void addTwiceNatToNhCache(const IpAddress &translatedIp, const TwiceNatEntryKey &key);
    void addTwiceNaptToNhCache(const IpAddress &translatedIp, const TwiceNaptEntryKey &key);
    void removeTwiceNatFromNhCache(const IpAddress &translatedIp, const TwiceNatEntryKey &key);
    void removeTwiceNaptFromNhCache(const IpAddress &translatedIp, const TwiceNaptEntryKey &key);
    bool addHwSnatEntry(const IpAddress &ip_address);
    bool addHwSnaptEntry(const NaptEntryKey &key);
    bool addHwTwiceNatEntry(const TwiceNatEntryKey &key);
    bool addHwTwiceNaptEntry(const TwiceNaptEntryKey &key);
    bool removeHwSnatEntry(const IpAddress &dstIp);
    bool removeHwSnaptEntry(const NaptEntryKey &key);
    bool removeHwTwiceNatEntry(const TwiceNatEntryKey &key);
    bool removeHwTwiceNaptEntry(const TwiceNaptEntryKey &key);
    bool addHwDnatEntry(const IpAddress &ip_address);
    bool addHwDnaptEntry(const NaptEntryKey &key);
    bool removeHwDnatEntry(const IpAddress &dstIp);
    bool removeHwDnaptEntry(const NaptEntryKey &key);
    bool addHwDnatPoolEntry(const IpAddress &dstIp);
    bool removeHwDnatPoolEntry(const IpAddress &dstIp);

    bool checkIfNatEntryIsActive(const NatEntry::iterator &iter, time_t now);
    bool checkIfNaptEntryIsActive(const NaptEntry::iterator &iter, time_t now);
    bool checkIfTwiceNatEntryIsActive(const TwiceNatEntry::iterator &iter, time_t now);
    bool checkIfTwiceNaptEntryIsActive(const TwiceNaptEntry::iterator &iter, time_t now);

    void enableNatFeature(void);
    void disableNatFeature(void);
    void addAllNatEntries(void);
    void addAllDnatPoolEntries(void);
    void clearAllDnatEntries(void);
    void cleanupAppDbEntries(void);
    void clearCounters(void);
    void queryCounters(void);
    void queryHitBits(void);
    bool isNatEnabled(void);
    bool getNatCounters(const NatEntry::iterator &iter);
    bool getTwiceNatCounters(const TwiceNatEntry::iterator &iter);
    bool getNaptCounters(const NaptEntry::iterator &iter);
    bool getTwiceNaptCounters(const TwiceNaptEntry::iterator &iter);
    bool setNatCounters(const NatEntry::iterator &iter);
    bool setTwiceNatCounters(const TwiceNatEntry::iterator &iter);
    bool setNaptCounters(const NaptEntry::iterator &iter);
    bool setTwiceNaptCounters(const TwiceNaptEntry::iterator &iter);
    void updateStaticNatCounters(int count);
    void updateDynamicNatCounters(int count);
    void updateStaticNaptCounters(int count);
    void updateDynamicNaptCounters(int count);
    void updateStaticTwiceNatCounters(int count);
    void updateDynamicTwiceNatCounters(int count);
    void updateStaticTwiceNaptCounters(int count);
    void updateDynamicTwiceNaptCounters(int count);
    void updateSnatCounters(int count);
    void updateDnatCounters(int count);
    void updateNatCounters(const IpAddress &ipAddr,
                           uint64_t snat_translations_pkts, uint64_t snat_translations_bytes);
    void updateNaptCounters(const string &protocol, const IpAddress &ipAddr, int l4_port,
                            uint64_t snat_translations_pkts, uint64_t snat_translations_bytes);
    void deleteNatCounters(const IpAddress &ipAddr);
    void deleteNaptCounters(const string &protocol, const IpAddress &ipAddr, int l4_port);
    void deleteTwiceNatCounters(const TwiceNatEntryKey &key);
    void deleteTwiceNaptCounters(const TwiceNaptEntryKey &key);
    void updateTwiceNatCounters(const TwiceNatEntryKey &key,
                                uint64_t nat_translations_pkts, uint64_t nat_translations_bytes);
    void updateTwiceNaptCounters(const TwiceNaptEntryKey &key,
                                 uint64_t nat_translations_pkts, uint64_t nat_translations_bytes);

    void updateAllConntrackEntries();
};

#endif /* SWSS_NATORCH_H */
