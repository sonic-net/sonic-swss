#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <set>

#include "dbconnector.h"
#include "table.h"

enum class PbhAsicVendor : std::int32_t
{
    GENERIC,
    MELLANOX
};

enum class PbhFieldCapability : std::int32_t
{
    ADD,
    UPDATE,
    REMOVE
};

class PbhVendorFieldCapabilities
{
public:
    PbhVendorFieldCapabilities() = default;
    virtual ~PbhVendorFieldCapabilities() = default;

protected:
    void setPbhDefaults(std::set<PbhFieldCapability> &fieldCap) noexcept;

public:
    struct {
        std::set<PbhFieldCapability> interface_list;
        std::set<PbhFieldCapability> description;
    } table;

    struct {
        std::set<PbhFieldCapability> priority;
        std::set<PbhFieldCapability> gre_key;
        std::set<PbhFieldCapability> ether_type;
        std::set<PbhFieldCapability> ip_protocol;
        std::set<PbhFieldCapability> ipv6_next_header;
        std::set<PbhFieldCapability> l4_dst_port;
        std::set<PbhFieldCapability> inner_ether_type;
        std::set<PbhFieldCapability> hash;
        std::set<PbhFieldCapability> packet_action;
        std::set<PbhFieldCapability> flow_counter;
    } rule;

    struct {
        std::set<PbhFieldCapability> hash_field_list;
    } hash;

    struct {
        std::set<PbhFieldCapability> hash_field;
        std::set<PbhFieldCapability> ip_mask;
        std::set<PbhFieldCapability> sequence_id;
    } hashField;
};

class PbhGenericFieldCapabilities final : public PbhVendorFieldCapabilities
{
public:
    PbhGenericFieldCapabilities() noexcept;
    ~PbhGenericFieldCapabilities() = default;
};

class PbhMellanoxFieldCapabilities final : public PbhVendorFieldCapabilities
{
public:
    PbhMellanoxFieldCapabilities() noexcept;
    ~PbhMellanoxFieldCapabilities() = default;
};

class PbhEntityCapabilities
{
public:
    PbhEntityCapabilities() = delete;
    virtual ~PbhEntityCapabilities() = default;

    PbhEntityCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept;

protected:
    bool validate(const std::set<PbhFieldCapability> &fieldCap, PbhFieldCapability value) const;

    std::shared_ptr<PbhVendorFieldCapabilities> fieldCap;
};

class PbhTableCapabilities final : public PbhEntityCapabilities
{
public:
    PbhTableCapabilities() = delete;
    ~PbhTableCapabilities() = default;

    PbhTableCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept;

    bool validatePbhInterfaceList(PbhFieldCapability value) const;
    bool validatePbhDescription(PbhFieldCapability value) const;

    auto getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::table) &;
};

class PbhRuleCapabilities final : public PbhEntityCapabilities
{
public:
    PbhRuleCapabilities() = delete;
    ~PbhRuleCapabilities() = default;

    PbhRuleCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept;

    bool validatePbhPriority(PbhFieldCapability value) const;
    bool validatePbhGreKey(PbhFieldCapability value) const;
    bool validatePbhEtherType(PbhFieldCapability value) const;
    bool validatePbhIpProtocol(PbhFieldCapability value) const;
    bool validatePbhIpv6NextHeader(PbhFieldCapability value) const;
    bool validatePbhL4DstPort(PbhFieldCapability value) const;
    bool validatePbhInnerEtherType(PbhFieldCapability value) const;
    bool validatePbhHash(PbhFieldCapability value) const;
    bool validatePbhPacketAction(PbhFieldCapability value) const;
    bool validatePbhFlowCounter(PbhFieldCapability value) const;

    auto getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::rule) &;
};

class PbhHashCapabilities final : public PbhEntityCapabilities
{
public:
    PbhHashCapabilities() = delete;
    ~PbhHashCapabilities() = default;

    PbhHashCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept;

    bool validatePbhHashFieldList(PbhFieldCapability value) const;

    auto getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::hash) &;
};

class PbhHashFieldCapabilities final : public PbhEntityCapabilities
{
public:
    PbhHashFieldCapabilities() = delete;
    ~PbhHashFieldCapabilities() = default;

    PbhHashFieldCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept;

    bool validatePbhHashField(PbhFieldCapability value) const;
    bool validatePbhIpMask(PbhFieldCapability value) const;
    bool validatePbhSequenceId(PbhFieldCapability value) const;

    auto getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::hashField) &;
};

class PbhCapabilities final
{
public:
    PbhCapabilities() noexcept;
    ~PbhCapabilities() = default;

    bool validatePbhTableCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const;
    bool validatePbhRuleCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const;
    bool validatePbhHashCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const;
    bool validatePbhHashFieldCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const;

    PbhAsicVendor getAsicVendor() const noexcept;

private:
    template<typename T>
    void writePbhEntityCapabilitiesToDb(const std::shared_ptr<T> &entityCap);

    bool parsePbhAsicVendor();
    void initPbhVendorCapabilities();
    void writePbhVendorCapabilitiesToDb();

    PbhAsicVendor asicVendor;

    std::shared_ptr<PbhTableCapabilities> table;
    std::shared_ptr<PbhRuleCapabilities> rule;
    std::shared_ptr<PbhHashCapabilities> hash;
    std::shared_ptr<PbhHashFieldCapabilities> hashField;

    static swss::DBConnector stateDb;
    static swss::Table capTable;
};
