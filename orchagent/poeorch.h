#pragma once

#include "orch.h"
#include "observer.h"
#include "poecfg.h"
#include "selectabletimer.h"

#include <string>
#include <vector>

using namespace swss;

class PoeOrch : public Orch
{
public:
    PoeOrch(DBConnector *applDb, DBConnector *cfgDb, DBConnector *stateDb, const std::vector<std::string> &poeTables);

private:
    bool initPoe();
    bool initPoeDevice(const sai_object_id_t &switchOid, poe_device_t &dev);
    bool initPoePse(const sai_object_id_t &switchOid, const sai_object_id_t &devOid, poe_pse_t &pse);
    bool initPoePort(const sai_object_id_t &switchOid, const sai_object_id_t &devOid, poe_port_t &port);
    void initStateTables();
    bool initDone = false;

    Table m_cfgPoeTable;
    Table m_appPoeTable;
    Table m_deviceStateTable;
    Table m_pseStateTable;
    Table m_portStateTable;

    std::map<uint32_t, poe_device_t> m_poeDeviceMap;
    std::map<uint32_t, poe_pse_t> m_poePseMap;
    std::map<uint32_t, poe_port_t> m_poePortMap;

    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);
};
