#ifndef SWSS_NEXTHOPGROUPKEY_H
#define SWSS_NEXTHOPGROUPKEY_H

#include "nexthopkey.h"

class NextHopGroupKey
{
public:
    NextHopGroupKey() = default;

    /* ip_string@if_alias separated by ',' */
    NextHopGroupKey(const std::string &nexthops)
    {
        auto nhv = tokenize(nexthops, NHG_DELIMITER);
        for (const auto &nh : nhv)
        {
            m_nexthops.insert(nh);
        }
    }

    inline const std::set<NextHopKey> &getNextHops() const
    {
        return m_nexthops;
    }

    inline size_t getSize() const
    {
        return m_nexthops.size();
    }

    inline bool operator<(const NextHopGroupKey &o) const
    {
        return m_nexthops < o.m_nexthops;
    }

    inline bool operator==(const NextHopGroupKey &o) const
    {
        return m_nexthops == o.m_nexthops;
    }

    inline bool operator!=(const NextHopGroupKey &o) const
    {
        return !(*this == o);
    }

    void add(const std::string &ip, const std::string &alias)
    {
        m_nexthops.emplace(ip, alias);
    }

    void add(const std::string &nh)
    {
        m_nexthops.insert(nh);
    }

    void add(const NextHopKey &nh)
    {
        m_nexthops.insert(nh);
    }

    bool contains(const std::string &ip, const std::string &alias) const
    {
        NextHopKey nh(ip, alias);
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const std::string &nh) const
    {
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const NextHopKey &nh) const
    {
        return m_nexthops.find(nh) != m_nexthops.end();
    }

    bool contains(const NextHopGroupKey &nhs) const
    {
        for (const auto &nh : nhs.getNextHops())
        {
            if (!contains(nh))
            {
                return false;
            }
        }
        return true;
    }

    bool hasIntfNextHop() const
    {
        for (const auto &nh : m_nexthops)
        {
            if (nh.isIntfNextHop())
            {
                return true;
            }
        }
        return false;
    }

    void remove(const std::string &ip, const std::string &alias)
    {
        NextHopKey nh(ip, alias);
        m_nexthops.erase(nh);
    }

    void remove(const std::string &nh)
    {
        m_nexthops.erase(nh);
    }

    void remove(const NextHopKey &nh)
    {
        m_nexthops.erase(nh);
    }

    const std::string to_string() const
    {
        string nhs_str;

        for (auto it = m_nexthops.begin(); it != m_nexthops.end(); ++it)
        {
            if (it != m_nexthops.begin())
            {
                nhs_str += NHG_DELIMITER;
            }

            nhs_str += it->to_string();
        }

        return nhs_str;
    }

private:
    std::set<NextHopKey> m_nexthops;
};

#endif /* SWSS_NEXTHOPGROUPKEY_H */
