#pragma once

#include <string>

#include "orchagent/switch/switch_container.h"

class SwitchHelper final
{
public:
    SwitchHelper() = default;
    ~SwitchHelper() = default;

    const SwitchHash& getSwHash() const;
    void setSwHash(const SwitchHash &hash);

    bool parseSwHash(SwitchHash &hash) const;

private:
    template<typename T>
    bool parseSwHashFieldList(T &obj, const std::string &field, const std::string &value) const;
    template<typename T>
    bool parseSwHashAlgorithm(T &obj, const std::string &field, const std::string &value) const;

    bool parseSwHashEcmpHash(SwitchHash &hash, const std::string &field, const std::string &value) const;
    bool parseSwHashLagHash(SwitchHash &hash, const std::string &field, const std::string &value) const;
    bool parseSwHashEcmpHashAlgorithm(SwitchHash &hash, const std::string &field, const std::string &value) const;
    bool parseSwHashLagHashAlgorithm(SwitchHash &hash, const std::string &field, const std::string &value) const;

    bool validateSwHash(SwitchHash &hash) const;

private:
    SwitchHash swHash;
};
