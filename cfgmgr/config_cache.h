#pragma once
#include <map>
#include <string>
#include <functional>

namespace swss {

typedef std::function<bool(const std::string&, const std::string&, const std::string&, const std::string &, void *context)> ConfigChangeCb;
typedef std::map<std::string, std::string> ConfigEntry;
typedef std::map<std::string, ConfigEntry> ConfigData;

class ConfigCache
{
public:
    
    ConfigCache(ConfigChangeCb configChangeCb);

    void config(const std::string& key, const std::string& field, const std::string& value, void *context = nullptr);
    void applyDefault(const std::string& key, const ConfigEntry &defaultConfig, void *context = nullptr);
    bool exist(const std::string& key) const;
    void remove(const std::string &key) { mConfigData.erase(key); }
private:
    ConfigData mConfigData;
    ConfigChangeCb mConfigChangeCb;
};

}
