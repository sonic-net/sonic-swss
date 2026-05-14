// includes -----------------------------------------------------------------------------------------------------------

#include <unordered_map>
#include <string>

#include <tokenize.h>

#include "bufferschema.h"

#include "bufferhelper.h"

using namespace swss;

// helper -------------------------------------------------------------------------------------------------------------

void BufferHelper::parseBufferConfig(BufferProfileConfig &cfg) const
{
    auto &map = cfg.fieldValueMap;

    const auto &cit = map.find(BUFFER_PROFILE_PACKET_DISCARD_ACTION);
    if (cit != map.cend())
    {
        cfg.isTrimmingEligible = cit->second == BUFFER_PROFILE_PACKET_DISCARD_ACTION_TRIM ? true : false;
    }
}

void BufferHelper::parseBufferConfig(BufferPriorityGroupConfig &cfg) const
{
    auto &map = cfg.fieldValueMap;

    const auto &cit = map.find(BUFFER_PG_PROFILE);
    if (cit != map.cend())
    {
        cfg.profile.value = cit->second;
        cfg.profile.is_set = true;
    }
}

void BufferHelper::parseBufferConfig(IngressBufferProfileListConfig &cfg) const
{
    auto &map = cfg.fieldValueMap;

    const auto &cit = map.find(BUFFER_PORT_INGRESS_PROFILE_LIST_PROFILE_LIST);
    if (cit != map.cend())
    {
        auto profList = tokenize(cit->second, ',');

        cfg.profile_list.value.insert(profList.begin(), profList.end());
        cfg.profile_list.is_set = true;
    }
}

void BufferHelper::parseBufferConfig(EgressBufferProfileListConfig &cfg) const
{
    auto &map = cfg.fieldValueMap;

    const auto &cit = map.find(BUFFER_PORT_EGRESS_PROFILE_LIST_PROFILE_LIST);
    if (cit != map.cend())
    {
        auto profList = tokenize(cit->second, ',');

        cfg.profile_list.value.insert(profList.begin(), profList.end());
        cfg.profile_list.is_set = true;
    }
}

template<>
void BufferHelper::setObjRef(const BufferProfileConfig &cfg)
{
    // No actions are required
}

template<>
void BufferHelper::setObjRef(const BufferPriorityGroupConfig &cfg)
{
    if (cfg.profile.is_set)
    {
        const auto &cit = profMap.find(cfg.profile.value);
        if (cit != profMap.cend())
        {
            cit->second.pgRefCount++;
        }
    }
}

template<>
void BufferHelper::setObjRef(const IngressBufferProfileListConfig &cfg)
{
    if (cfg.profile_list.is_set)
    {
        for (const auto &cit1 : cfg.profile_list.value)
        {
            const auto &cit2 = profMap.find(cit1);
            if (cit2 != profMap.cend())
            {
                cit2->second.iBufProfListRefCount++;
            }
        }
    }
}

template<>
void BufferHelper::setObjRef(const EgressBufferProfileListConfig &cfg)
{
    if (cfg.profile_list.is_set)
    {
        for (const auto &cit1 : cfg.profile_list.value)
        {
            const auto &cit2 = profMap.find(cit1);
            if (cit2 != profMap.cend())
            {
                cit2->second.eBufProfListRefCount++;
            }
        }
    }
}

template<>
void BufferHelper::delObjRef(const BufferProfileConfig &cfg)
{
    // No actions are required
}

template<>
void BufferHelper::delObjRef(const BufferPriorityGroupConfig &cfg)
{
    if (cfg.profile.is_set)
    {
        const auto &cit = profMap.find(cfg.profile.value);
        if (cit != profMap.cend())
        {
            cit->second.pgRefCount--;
        }
    }
}

template<>
void BufferHelper::delObjRef(const IngressBufferProfileListConfig &cfg)
{
    if (cfg.profile_list.is_set)
    {
        for (const auto &cit1 : cfg.profile_list.value)
        {
            const auto &cit2 = profMap.find(cit1);
            if (cit2 != profMap.cend())
            {
                cit2->second.iBufProfListRefCount--;
            }
        }
    }
}

template<>
void BufferHelper::delObjRef(const EgressBufferProfileListConfig &cfg)
{
    if (cfg.profile_list.is_set)
    {
        for (const auto &cit1 : cfg.profile_list.value)
        {
            const auto &cit2 = profMap.find(cit1);
            if (cit2 != profMap.cend())
            {
                cit2->second.eBufProfListRefCount--;
            }
        }
    }
}

template<>
auto BufferHelper::getBufferObjMap() const -> const std::unordered_map<std::string, BufferProfileConfig>&
{
    return profMap;
}

template<>
auto BufferHelper::getBufferObjMap() const -> const std::unordered_map<std::string, BufferPriorityGroupConfig>&
{
    return pgMap;
}

template<>
auto BufferHelper::getBufferObjMap() const -> const std::unordered_map<std::string, IngressBufferProfileListConfig>&
{
    return iBufProfListMap;
}

template<>
auto BufferHelper::getBufferObjMap() const -> const std::unordered_map<std::string, EgressBufferProfileListConfig>&
{
    return eBufProfListMap;
}

template<>
auto BufferHelper::getBufferObjMap() -> std::unordered_map<std::string, BufferProfileConfig>&
{
    return profMap;
}

template<>
auto BufferHelper::getBufferObjMap() -> std::unordered_map<std::string, BufferPriorityGroupConfig>&
{
    return pgMap;
}

template<>
auto BufferHelper::getBufferObjMap() -> std::unordered_map<std::string, IngressBufferProfileListConfig>&
{
    return iBufProfListMap;
}

template<>
auto BufferHelper::getBufferObjMap() -> std::unordered_map<std::string, EgressBufferProfileListConfig>&
{
    return eBufProfListMap;
}

template<typename T>
void BufferHelper::setBufferConfig(const std::string &key, const T &cfg)
{
    auto &map = getBufferObjMap<T>();

    const auto &cit = map.find(key);
    if (cit != map.cend())
    {
        delObjRef(cit->second);
    }
    setObjRef(cfg);

    map[key] = cfg;
}

template void BufferHelper::setBufferConfig(const std::string &key, const BufferProfileConfig &cfg);
template void BufferHelper::setBufferConfig(const std::string &key, const BufferPriorityGroupConfig &cfg);
template void BufferHelper::setBufferConfig(const std::string &key, const IngressBufferProfileListConfig &cfg);
template void BufferHelper::setBufferConfig(const std::string &key, const EgressBufferProfileListConfig &cfg);

template<typename T>
bool BufferHelper::getBufferConfig(T &cfg, const std::string &key) const
{
    auto &map = getBufferObjMap<T>();

    const auto &cit = map.find(key);
    if (cit != map.cend())
    {
        cfg = cit->second;
        return true;
    }

    return false;
}

template bool BufferHelper::getBufferConfig(BufferProfileConfig &cfg, const std::string &key) const;
template bool BufferHelper::getBufferConfig(BufferPriorityGroupConfig &cfg, const std::string &key) const;
template bool BufferHelper::getBufferConfig(IngressBufferProfileListConfig &cfg, const std::string &key) const;
template bool BufferHelper::getBufferConfig(EgressBufferProfileListConfig &cfg, const std::string &key) const;

void BufferHelper::delBufferProfileConfig(const std::string &key)
{
    const auto &cit = profMap.find(key);
    if (cit == profMap.cend())
    {
        return;
    }

    delObjRef(cit->second);
    profMap.erase(cit);
}

void BufferHelper::delBufferPriorityGroupConfig(const std::string &key)
{
    const auto &cit = pgMap.find(key);
    if (cit == pgMap.cend())
    {
        return;
    }

    delObjRef(cit->second);
    pgMap.erase(cit);
}

void BufferHelper::delIngressBufferProfileListConfig(const std::string &key)
{
    const auto &cit = iBufProfListMap.find(key);
    if (cit == iBufProfListMap.cend())
    {
        return;
    }

    delObjRef(cit->second);
    iBufProfListMap.erase(cit);
}

void BufferHelper::delEgressBufferProfileListConfig(const std::string &key)
{
    const auto &cit = eBufProfListMap.find(key);
    if (cit == eBufProfListMap.cend())
    {
        return;
    }

    delObjRef(cit->second);
    eBufProfListMap.erase(cit);
}
