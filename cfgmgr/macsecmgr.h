#ifndef __MACSECMGR__
#define __MACSECMGR__

// The following definitions should be moved to schema.h

#define CFG_MACSEC_PROFILE_TABLE_NAME           "MACSEC_PROFILE"

// End define

#include <orch.h>


namespace swss {

class MACsecMgr : public Orch
{
public:
    using Orch::doTask;
    MACsecMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);

private:
    void doTask(Consumer &consumer);
};

}

#endif
