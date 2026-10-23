#pragma once

extern "C" {
#include <saitypes.h>
#include <saiobject.h>
}

#include <table.h>

class MirrorSessionCapabilities final
{
public:
    MirrorSessionCapabilities();
    ~MirrorSessionCapabilities() = default;

    bool isMirrorSessionLabelSupported() const;

private:
    enum class AttributeCapability
    {
        Unknown,
        Supported,
        Unsupported
    };

    swss::FieldValueTuple makeLabelCapDbEntry() const;

    sai_status_t queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const;

    void queryLabelAttrCapabilities();
    void queryCapabilities();
    void writeCapabilitiesToDb();

    struct {
        AttributeCapability capability = AttributeCapability::Unknown;
    } label; // SAI_MIRROR_SESSION_ATTR_LABEL
};
