#include "hftelgroup.h"
#include "hftelutils.h"

#include <swss/logger.h>

using namespace std;

HFTelGroup::HFTelGroup(const string &group_name) : m_group_name(group_name)
{
    SWSS_LOG_ENTER();
}

void HFTelGroup::updateObjects(const set<string> &object_names)
{
    SWSS_LOG_ENTER();

    m_objects.clear();
    sai_uint16_t lable = 1;
    for (auto &name : object_names)
    {
        m_objects[name] = lable++;
    }
}

void HFTelGroup::updateStatsIDs(const std::set<sai_stat_id_t> &stats_ids)
{
    SWSS_LOG_ENTER();

    m_stats_ids = move(stats_ids);
}

bool HFTelGroup::isSameObjects(const std::set<std::string> &object_names) const
{
    SWSS_LOG_ENTER();

    if (m_objects.size() == object_names.size())
    {
        for (const auto &name : object_names)
        {
            if (m_objects.find(name) == m_objects.end())
            {
                return false;
            }
        }
        return true;
    }

    return false;
}

bool HFTelGroup::isObjectInGroup(const string &object_name) const
{
    SWSS_LOG_ENTER();

    return m_objects.find(object_name) != m_objects.end();
}
