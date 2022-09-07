#pragma once

#include <vector>

#include "orch.h"
#include "aclorch.h"
#include "portsorch.h"

#include "pbh/pbhrule.h"
#include "pbh/pbhmgr.h"
#include "pbh/pbhcap.h"

class PbhOrch final : public Orch
{
public:
    PbhOrch() = delete;
    ~PbhOrch() = default;

    PbhOrch(
        std::vector<TableConnector> &connectorList,
        AclOrch *aclOrch,
        PortsOrch *portsOrch
    );

    using Orch::doTask;  // Allow access to the basic doTask

private:
    template<typename T>
    std::vector<std::string> getPbhAddedFields(const T &obj, const T &nObj) const;
    template<typename T>
    std::vector<std::string> getPbhUpdatedFields(const T &obj, const T &nObj) const;
    template<typename T>
    std::vector<std::string> getPbhRemovedFields(const T &obj, const T &nObj) const;

    template<typename T>
    auto getPbhSetupTaskMap() const -> const std::unordered_map<std::string, T>&;
    template<typename T>
    auto getPbhRemoveTaskMap() const -> const std::unordered_map<std::string, T>&;

    template<typename T>
    bool pbhSetupTaskExists(const T &obj) const;
    template<typename T>
    bool pbhRemoveTaskExists(const T &obj) const;

    template<typename T>
    bool pbhTaskExists(const T &obj) const;

    bool createPbhTable(const PbhTable &table);
    bool updatePbhTable(const PbhTable &table);
    bool removePbhTable(const PbhTable &table);

    bool createPbhRule(const PbhRule &rule);
    bool updatePbhRule(const PbhRule &rule);
    bool removePbhRule(const PbhRule &rule);

    bool createPbhHash(const PbhHash &hash);
    bool updatePbhHash(const PbhHash &hash);
    bool removePbhHash(const PbhHash &hash);

    bool createPbhHashField(const PbhHashField &hashField);
    bool updatePbhHashField(const PbhHashField &hashField);
    bool removePbhHashField(const PbhHashField &hashField);

    void deployPbhTableSetupTasks();
    void deployPbhTableRemoveTasks();

    void deployPbhRuleSetupTasks();
    void deployPbhRuleRemoveTasks();

    void deployPbhHashSetupTasks();
    void deployPbhHashRemoveTasks();

    void deployPbhHashFieldSetupTasks();
    void deployPbhHashFieldRemoveTasks();

    void deployPbhTasks();

    void doPbhTableTask(Consumer &consumer);
    void doPbhRuleTask(Consumer &consumer);
    void doPbhHashTask(Consumer &consumer);
    void doPbhHashFieldTask(Consumer &consumer);
    void doTask(Consumer &consumer);

    AclOrch *aclOrch;
    PortsOrch *portsOrch;

    PbhHelper pbhHlpr;
    PbhCapabilities pbhCap;
};
