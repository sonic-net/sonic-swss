#pragma once

#include "dbconnector.h"
#include "orchagent/orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {

#define ERROR_SPEED "error"
#define NA_SPEED "N/A"

struct SflowPortInfo
{
    bool        local_rate_cfg;
    bool        local_admin_cfg;
    bool        local_dir_cfg;
    std::string speed;
    std::string oper_speed;
    std::string rate;
    std::string admin;
    std::string dir;
};

/* Port to Local config map  */
typedef std::map<std::string, SflowPortInfo> SflowPortConfMap;

class SflowMgr : public Orch
{
public:
    SflowMgr(DBConnector *appDb, const std::vector<TableConnector>& tableNames);
    void readPortConfig();

    using Orch::doTask;
private:
    ProducerStateTable     m_appSflowTable;
    ProducerStateTable     m_appSflowSessionTable;
    SflowPortConfMap       m_sflowPortConfMap;
    bool                   m_intfAllConf;
    bool                   m_gEnable;
    std::string            m_intfAllDir;
    std::string            m_gDirection;

    void doTask(Consumer &consumer);
    void sflowHandleService(bool enable);
    void sflowUpdatePortInfo(Consumer &consumer);
    void sflowProcessOperSpeed(Consumer &consumer);
    void sflowHandleSessionAll(bool enable, std::string direction);
    void sflowHandleSessionLocal(bool enable);
    void sflowCheckAndFillValues(std::string alias, std::vector<FieldValueTuple> &values, std::vector<FieldValueTuple> &fvs);
    void sflowGetPortInfo(std::vector<FieldValueTuple> &fvs, SflowPortInfo &local_info);
    void sflowGetGlobalInfo(std::vector<FieldValueTuple> &fvs, const std::string& alias, const std::string& direction);
    bool isPortEnabled(const std::string& alias);
    std::string findSamplingRate(const std::string& speed);
};

}
