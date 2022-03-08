#include "config_cache.h"

using namespace swss;

ConfigCache::ConfigCache(ConfigChangeCb configChangeCb):
mConfigChangeCb(configChangeCb)
{}

void ConfigCache::config(const std::string& key, const std::string& field, const std::string& value)
{
    auto iter = mConfigData.find(key);
    if (iter == mConfigData.end())
    {
        mConfigData.emplace(key, ConfigEntry({{field, value}}));
        mConfigChangeCb(key, field, "", value);
    }
    else
    {
        auto entry_iter = iter->second.find(field);
        if (entry_iter == iter->second.end())
        {
            iter->second.emplace(field, value);
            mConfigChangeCb(key, field, "", value);
        }
        else
        {
            if (value != entry_iter->second)
            {
                mConfigChangeCb(key, field, entry_iter->second, value);
                entry_iter->second = value;
            }
        }
    }
}

void ConfigCache::applyDefault(const std::string& key, const ConfigEntry &defaultConfig)
{
    auto &entry = mConfigData[key];
    for (auto &fv : defaultConfig)
    {
        auto iter = entry.find(fv.first);
        if (iter == entry.end())
        {
            entry.emplace(fv.first, fv.second);
            mConfigChangeCb(key, fv.first, "", fv.second);
        }
    }
}

bool ConfigCache::exist(const std::string& key) const
{
    return mConfigData.find(key) != mConfigData.end();
}
