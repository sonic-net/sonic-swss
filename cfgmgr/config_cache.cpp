#include "config_cache.h"

using namespace swss;

ConfigCache::ConfigCache(ConfigChangeCb configChangeCb):
mConfigChangeCb(configChangeCb)
{}

void ConfigCache::config(const std::string& key, const std::string& field, const std::string& value, void *context)
{
    auto iter = mConfigData.find(key);
    if (iter == mConfigData.end())
    {
        if (mConfigChangeCb(key, field, "", value, context))
        {
            mConfigData.emplace(key, ConfigEntry({{field, value}}));
        }
    }
    else
    {
        auto entry_iter = iter->second.find(field);
        if (entry_iter == iter->second.end())
        {
            if (mConfigChangeCb(key, field, "", value, context))
            {
                iter->second.emplace(field, value);
            }
        }
        else
        {
            if (mConfigChangeCb(key, field, entry_iter->second, value, context))
            {
                entry_iter->second = value;
            }
        }
    }
}

void ConfigCache::applyDefault(const std::string& key, const ConfigEntry &defaultConfig, void *context)
{
    auto &entry = mConfigData[key];
    for (auto &fv : defaultConfig)
    {
        auto iter = entry.find(fv.first);
        if (iter == entry.end())
        {
            if (mConfigChangeCb(key, fv.first, "", fv.second, context))
            {
                entry.emplace(fv.first, fv.second);
            }
        }
    }
}

bool ConfigCache::exist(const std::string& key) const
{
    return mConfigData.find(key) != mConfigData.end();
}
