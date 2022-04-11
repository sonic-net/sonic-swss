#pragma once

#include "aclorch.h"

class AclRulePbh: public AclRule
{
public:
    AclRulePbh(AclOrch *pAclOrch, string rule, string table, bool createCounter = false);

    bool validateAddPriority(const sai_uint32_t &value);
    bool validateAddMatch(const sai_attribute_t &attr);
    bool validateAddAction(const sai_attribute_t &attr);
    bool validate() override;
    void onUpdate(SubjectType, void *) override;
    bool validateAddAction(string attr_name, string attr_value) override;
    bool disableAction();
};
