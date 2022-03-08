#pragma once
#include <map>
#include <string>
#include <functional>

namespace swss {

typedef std::function<void(const std::string&, const std::string&, const std::string&, const std::string &)> ConfigChangeCb;
typedef std::map<std::string, std::string> ConfigEntry;
typedef std::map<std::string, ConfigEntry> ConfigData;

class ConfigCache
{
public:
    
    ConfigCache(ConfigChangeCb configChangeCb);

    void config(const std::string& key, const std::string& field, const std::string& value);
    void applyDefault(const std::string& key, const ConfigEntry &defaultConfig);
    bool exist(const std::string& key) const;
    void remove(const std::string &key) { mConfigData.erase(key); }
private:
    ConfigData mConfigData;
    ConfigChangeCb mConfigChangeCb;
};

}
