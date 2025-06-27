#pragma once

#include <string>

#include "container.h"

class SwitchTrimmingHelper final
{
public:
    SwitchTrimmingHelper() = default;
    ~SwitchTrimmingHelper() = default;

    bool isStaticQueueMode(const SwitchTrimming &cfg) const;

    const SwitchTrimming& getConfig() const;
    void setConfig(const SwitchTrimming &cfg);

    bool parseConfig(SwitchTrimming &cfg) const;

private:
    bool parseSize(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;
    bool parseDscp(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;
    bool parseQueue(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;

    bool validateConfig(SwitchTrimming &cfg) const;

private:
    SwitchTrimming cfg;
};
