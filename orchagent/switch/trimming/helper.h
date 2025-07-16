#pragma once

#include <string>

#include "container.h"

class SwitchTrimmingHelper final
{
public:
    SwitchTrimmingHelper() = default;
    ~SwitchTrimmingHelper() = default;

    bool isSymDscpMode(const SwitchTrimming &cfg) const;
    bool isStaticQueueMode(const SwitchTrimming &cfg) const;

    const SwitchTrimming& getConfig() const;
    void setConfig(const SwitchTrimming &cfg);

    bool parseTrimConfig(SwitchTrimming &cfg) const;

private:
    bool parseTrimSize(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;
    bool parseTrimDscp(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;
    bool parseTrimTc(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;
    bool parseTrimQueue(SwitchTrimming &cfg, const std::string &field, const std::string &value) const;

    bool validateTrimConfig(SwitchTrimming &cfg) const;

private:
    SwitchTrimming cfg;
};
