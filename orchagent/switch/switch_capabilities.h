#pragma once

extern "C" {
#include <saiobject.h>
#include <saitypes.h>
#include <saihash.h>
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

    bool validateSwitchHashFieldCap(const std::set<sai_native_hash_field_t> &hfSet) const;

private:
    swss::FieldValueTuple makeHashFieldCapDbEntry() const;
    swss::FieldValueTuple makeEcmpHashCapDbEntry() const;
    swss::FieldValueTuple makeLagHashCapDbEntry() const;

    sai_status_t queryEnumCapabilitiesSai(std::vector<sai_int32_t> &capList, sai_object_type_t objType, sai_attr_id_t attrId) const;
    sai_status_t queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const;

    void queryHashNativeHashFieldListEnumCapabilities();
    void queryHashNativeHashFieldListAttrCapabilities();

    void querySwitchEcmpHashCapabilities();
    void querySwitchLagHashCapabilities();

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
    } switchCapabilities;

    static swss::DBConnector stateDb;
    static swss::Table capTable;
};
