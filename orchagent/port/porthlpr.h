#pragma once

#include <cstdint>

#include <vector>
#include <string>

#include "portcnt.h"

class PortHelper final
{
public:
    typedef std::vector<std::uint32_t> PortSerdesList_t;

public:
    PortHelper() = default;
    ~PortHelper() = default;

public:
    bool fecToStr(std::string &str, sai_port_fec_mode_t value) const;

    std::string getPortInterfaceTypeStr(const PortConfig &port) const;
    std::string getAdvInterfaceTypesStr(const PortConfig &port) const;
    std::string getFecStr(const PortConfig &port) const;
    std::string getPfcAsymStr(const PortConfig &port) const;
    std::string getLearnModeStr(const PortConfig &port) const;
    std::string getLinkTrainingStr(const PortConfig &port) const;
    std::string getAdminStatusStr(const PortConfig &port) const;

    bool parsePortConfig(PortConfig &port) const;

private:
    std::string getFieldValueStr(const PortConfig &port, const std::string &field) const;

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
    bool parsePortSerdes(PortSerdesList_t &list, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPreemphasis(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesIdriver(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesIpredriver(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPre1(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPre2(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPre3(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesMain(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPost1(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPost2(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesPost3(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortSerdesAttn(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortRole(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortAdminStatus(PortConfig &port, const std::string &field, const std::string &value) const;
    bool parsePortDescription(PortConfig &port, const std::string &field, const std::string &value) const;

    bool validatePortConfig(PortConfig &port) const;
};
