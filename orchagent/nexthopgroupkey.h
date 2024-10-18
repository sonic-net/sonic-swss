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
        m_overlay_nexthops = false;
        m_srv6_nexthops = false;
        m_srv6_vpn = false;
        auto nhv = tokenize(nexthops, NHG_DELIMITER);
        for (const auto &nh : nhv)
        {
            m_nexthops.insert(nh);
        }
    }

    /* ip_string|if_alias|vni|router_mac separated by ',' */
    NextHopGroupKey(const std::string &nexthops, bool overlay_nh, bool srv6_nh = false)
    {
        if (overlay_nh)
        {
            m_overlay_nexthops = true;
            m_srv6_nexthops = false;
            m_srv6_vpn = false;
            auto nhv = tokenize(nexthops, NHG_DELIMITER);
            for (const auto &nh_str : nhv)
            {
                auto nh = NextHopKey(nh_str, overlay_nh, srv6_nh);
                m_nexthops.insert(nh);
            }
        }
        else if (srv6_nh)
        {
            m_overlay_nexthops = false;
            m_srv6_nexthops = true;
            m_srv6_vpn = false;
            auto nhv = tokenize(nexthops, NHG_DELIMITER);
            for (const auto &nh_str : nhv)
            {
                auto nh = NextHopKey(nh_str, overlay_nh, srv6_nh);
                m_nexthops.insert(nh);
                if (nh.isSrv6Vpn())
                {
                    m_srv6_vpn = true;
                }
            }
        }
    }

    NextHopGroupKey(const std::string &nexthops, bool overlay_nh, bool srv6_nh, const std::string& weights)
    {
        auto nhv = tokenize(nexthops, NHG_DELIMITER);
        auto wtv = tokenize(weights, NHG_DELIMITER);
        bool set_weight = wtv.size() == nhv.size();
        if (overlay_nh)
        {
            m_overlay_nexthops = true;
            m_srv6_nexthops = false;
            m_srv6_vpn = false;
            for (uint32_t i = 0; i < nhv.size(); ++i)
            {
                auto nh = NextHopKey(nhv[i], overlay_nh, srv6_nh);
                nh.weight = set_weight ? (uint32_t)std::stoi(wtv[i]) : 0;
                m_nexthops.insert(nh);
            }
        }
        else if (srv6_nh)
        {
            m_overlay_nexthops = false;
            m_srv6_nexthops = true;
            m_srv6_vpn = false;
            for (uint32_t i = 0; i < nhv.size(); ++i)
            {
                auto nh = NextHopKey(nhv[i], overlay_nh, srv6_nh);
                nh.weight = set_weight ? (uint32_t)std::stoi(wtv[i]) : 0;
                m_nexthops.insert(nh);
                if (nh.isSrv6Vpn())
                {
                    m_srv6_vpn = true;
                }
            }
        }
    }

    NextHopGroupKey(const std::string &nexthops, const std::string &weights)
    {
        m_overlay_nexthops = false;
        m_srv6_nexthops = false;
        m_srv6_vpn = false;
        std::vector<std::string> nhv = tokenize(nexthops, NHG_DELIMITER);
        std::vector<std::string> wtv = tokenize(weights, NHG_DELIMITER);
        bool set_weight = wtv.size() == nhv.size();
        for (uint32_t i = 0; i < nhv.size(); i++)
        {
            NextHopKey nh(nhv[i]);
            nh.weight = set_weight? (uint32_t)std::stoi(wtv[i]) : 0;
            m_nexthops.insert(nh);
        }
    }

    inline bool is_srv6_nexthop() const
    {
        return m_srv6_nexthops;
    }

    inline bool is_srv6_vpn() const
    {
        return m_srv6_vpn;
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
        if (m_nexthops < o.m_nexthops)
        {
            return true;
        }
        else if (m_nexthops == o.m_nexthops)
        {
            auto it1 = m_nexthops.begin();
            for (auto& it2 : o.m_nexthops)
            {
                if (it1->weight < it2.weight)
                {
                    return true;
                }
                else if(it1->weight > it2.weight)
                {
                    return false;
                }
                it1++;
            }
        }
        return false;
    }

    inline bool operator==(const NextHopGroupKey &o) const
    {
        if (m_nexthops != o.m_nexthops)
        {
            return false;
        }
        auto it1 = m_nexthops.begin();
        for (auto& it2 : o.m_nexthops)
        {
            if (it2.weight != it1->weight)
            {
                return false;
            }
            it1++;
        }
        return true;
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
            if (m_overlay_nexthops || m_srv6_nexthops) {
                nhs_str += it->to_string(m_overlay_nexthops, m_srv6_nexthops);
            } else {
                nhs_str += it->to_string();
            }
        }

        return nhs_str;
    }

    inline bool is_overlay_nexthop() const
    {
        return m_overlay_nexthops;
    }

    void clear()
    {
        m_nexthops.clear();
    }

private:
    std::set<NextHopKey> m_nexthops;
    bool m_overlay_nexthops;
    bool m_srv6_nexthops;
    bool m_srv6_vpn;
};

#endif /* SWSS_NEXTHOPGROUPKEY_H */
