#pragma once

#include <cstdint>

#include <vector>
#include <string>

#include "orchagent/port/portcnt.h"

class PortHelper final
{
public:
    PortHelper() = default;
    ~PortHelper() = default;

public:
    bool fecToStr(std::string &str, sai_port_fec_mode_t value) const;
    bool fecToSaiFecMode(const std::string &str, sai_port_fec_mode_t &value) const;
    bool fecIsOverrideRequired(const std::string &str) const;

    std::string getAutonegStr(const PortConfig &port) const;
    std::string getPortInterfaceTypeStr(const PortConfig &port) const;
    std::string getAdvInterfaceTypesStr(const PortConfig &port) const;
    std::string getFecStr(const PortConfig &port) const;
    std::string getPfcAsymStr(const PortConfig &port) const;
    std::string getLearnModeStr(const PortConfig &port) const;
    std::string getLinkTrainingStr(const PortConfig &port) const;
    std::string getAdminStatusStr(const PortConfig &port) const;
    std::string getPtTimestampTemplateStr(const PortConfig &port) const;
    std::string getDampingAlgorithm(const PortConfig &port) const;

    bool parsePortConfig(PortConfig &port) const;
    bool validatePortConfig(PortConfig &port) const;

private:
    std::string getFieldValueStr(const PortConfig &port, const std::string &field) const;

    template<typename T>
    bool parsePortSerdes(T &serdes, const std::string &field, const std::string &value) const;

    bool parsePortLinkEventDampingAlgorithm(PortConfig &port, const std::string &field, const std::string &value) const;
    template<typename T>
    bool parsePortLinkEventDampingConfig(T &damping_config_attr, const std::string &field, const std::string &value) const;

    bool parsePortAlias(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortIndex(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortLanes(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSpeed(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortAutoneg(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortAdvSpeeds(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortInterfaceType(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortAdvInterfaceTypes(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortFec(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortMtu(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortTpid(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortPfcAsym(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortLearnMode(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortLinkTraining(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortRole(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortAdminStatus(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortDescription(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSubport(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortPtIntfId(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortPtTimestampTemplate(PortConfig &port, const std::string &field, const std::string &value) const;
};
