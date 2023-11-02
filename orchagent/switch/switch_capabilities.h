#pragma once

extern "C" {
#include <saitypes.h>
#include <saiobject.h>
#include <saihash.h>
#include <saiswitch.h>
}

#include <vector>
#include <set>

#include <dbconnector.h>
#include <table.h>

class SwitchCapabilities final
{
public:
    SwitchCapabilities();
    ~SwitchCapabilities() = default;

    bool isSwitchEcmpHashSupported() const;
    bool isSwitchLagHashSupported() const;

    bool isSwitchEcmpHashAlgorithmSupported() const;
    bool isSwitchLagHashAlgorithmSupported() const;

    bool validateSwitchHashFieldCap(const std::set<sai_native_hash_field_t> &hfSet) const;

    bool validateSwitchEcmpHashAlgorithmCap(sai_hash_algorithm_t haValue) const;
    bool validateSwitchLagHashAlgorithmCap(sai_hash_algorithm_t haValue) const;

private:
    template<typename T>
    bool validateSwitchHashAlgorithmCap(const T &obj, sai_hash_algorithm_t haValue) const;

    swss::FieldValueTuple makeHashFieldCapDbEntry() const;

    swss::FieldValueTuple makeEcmpHashCapDbEntry() const;
    swss::FieldValueTuple makeLagHashCapDbEntry() const;

    std::vector<swss::FieldValueTuple> makeEcmpHashAlgorithmCapDbEntry() const;
    std::vector<swss::FieldValueTuple> makeLagHashAlgorithmCapDbEntry() const;

    sai_status_t queryEnumCapabilitiesSai(std::vector<sai_int32_t> &capList, sai_object_type_t objType, sai_attr_id_t attrId) const;
    sai_status_t queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const;

    void queryHashNativeHashFieldListEnumCapabilities();
    void queryHashNativeHashFieldListAttrCapabilities();

    void querySwitchEcmpHashAttrCapabilities();
    void querySwitchLagHashAttrCapabilities();

    void querySwitchEcmpHashAlgorithmEnumCapabilities();
    void querySwitchEcmpHashAlgorithmAttrCapabilities();
    void querySwitchLagHashAlgorithmEnumCapabilities();
    void querySwitchLagHashAlgorithmAttrCapabilities();

    void queryHashCapabilities();
    void querySwitchCapabilities();

    void writeHashCapabilitiesToDb();
    void writeSwitchCapabilitiesToDb();

    // Hash SAI capabilities
    struct {
        struct {
            std::set<sai_native_hash_field_t> hfSet;
            bool isEnumSupported = false;
            bool isAttrSupported = false;
        } nativeHashFieldList;
    } hashCapabilities;

    // Switch SAI capabilities
    struct {
        struct {
            bool isAttrSupported = false;
        } ecmpHash;

        struct {
            bool isAttrSupported = false;
        } lagHash;

        struct {
            std::set<sai_hash_algorithm_t> haSet;
            bool isEnumSupported = false;
            bool isAttrSupported = false;
        } ecmpHashAlgorithm;

        struct {
            std::set<sai_hash_algorithm_t> haSet;
            bool isEnumSupported = false;
            bool isAttrSupported = false;
        } lagHashAlgorithm;
    } switchCapabilities;

    static swss::DBConnector stateDb;
    static swss::Table capTable;
};
