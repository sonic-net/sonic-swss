#ifndef SWSS_BFDORCH_H
#define SWSS_BFDORCH_H

#include "orch.h"
#include "intfsorch.h"

#define BFD_SRCPORTINIT 49152
#define BFD_SRCPORTMAX 65536

class BfdOrch: public Orch
{
public:
    void doTask(Consumer &consumer);
    BfdOrch(swss::DBConnector *db, std::string tableName);
    virtual ~BfdOrch(void);

private:
    bool create_bfd_session(const string& key, const vector<FieldValueTuple>& data);
    bool remove_bfd_session(const string& key);

    uint32_t bfd_gen_id(void);
    uint32_t bfd_src_port(void);

    std::map<std::string, sai_object_id_t> bfd_session_map;
};

#endif /* SWSS_BFDORCH_H */
