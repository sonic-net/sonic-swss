// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saitypes.h>
#include <saiobject.h>
#include <saistatus.h>
#include <saimirror.h>
}

#include <string>
#include <vector>

#include <sai_serialize.h>
#include <dbconnector.h>
#include <table.h>
#include <schema.h>
#include <logger.h>

#include "mirror_capabilities.h"

using namespace swss;

// defines ------------------------------------------------------------------------------------------------------------

#define CAPABILITY_MIRROR_SESSION_LABEL_CAPABLE_FIELD "MIRROR_SESSION_LABEL_CAPABLE"
#define CAPABILITY_KEY "switch"

#define SWITCH_STATE_DB_NAME    "STATE_DB"
#define SWITCH_STATE_DB_TIMEOUT 0

// variables ----------------------------------------------------------------------------------------------------------

extern sai_object_id_t gSwitchId;

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(sai_object_type_t objType, sai_attr_id_t attrId)
{
    const auto *meta = sai_metadata_get_attr_metadata(objType, attrId);

    return meta != nullptr ? meta->attridname : "UNKNOWN";
}

static std::string toStr(sai_status_t value)
{
    const auto *name = sai_metadata_get_status_name(value);

    return name != nullptr ? name : "UNKNOWN";
}

static std::string toStr(bool value)
{
    return value ? "true" : "false";
}

// capabilities -------------------------------------------------------------------------------------------------------

MirrorSessionCapabilities::MirrorSessionCapabilities()
{
    queryCapabilities();
    writeCapabilitiesToDb();
}

bool MirrorSessionCapabilities::isMirrorSessionLabelSupported() const
{
    // The capability query may fail for reasons unrelated to LABEL: a platform
    // that does not implement sai_query_attribute_capability still accepts LABEL
    // on create. Without a conclusive answer keep passing LABEL; reject only when
    // the query succeeded and reported create_implemented as false.
    if (label.capability == AttributeCapability::Unknown)
    {
        return true;
    }

    return label.capability == AttributeCapability::Supported;
}

sai_status_t MirrorSessionCapabilities::queryAttrCapabilitiesSai(sai_attr_capability_t &attrCap, sai_object_type_t objType, sai_attr_id_t attrId) const
{
    return sai_query_attribute_capability(gSwitchId, objType, attrId, &attrCap);
}

void MirrorSessionCapabilities::queryLabelAttrCapabilities()
{
    SWSS_LOG_ENTER();

    sai_attr_capability_t attrCap;

    auto status = queryAttrCapabilitiesSai(
        attrCap, SAI_OBJECT_TYPE_MIRROR_SESSION, SAI_MIRROR_SESSION_ATTR_LABEL
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE(
            "Attribute(%s) capabilities are not available: unexpected status(%s)",
            toStr(SAI_OBJECT_TYPE_MIRROR_SESSION, SAI_MIRROR_SESSION_ATTR_LABEL).c_str(),
            toStr(status).c_str()
        );
        return;
    }

    if (!attrCap.create_implemented)
    {
        SWSS_LOG_WARN(
            "Attribute(%s) CREATE is not implemented in SAI",
            toStr(SAI_OBJECT_TYPE_MIRROR_SESSION, SAI_MIRROR_SESSION_ATTR_LABEL).c_str()
        );
        label.capability = AttributeCapability::Unsupported;
        return;
    }

    label.capability = AttributeCapability::Supported;
}

void MirrorSessionCapabilities::queryCapabilities()
{
    queryLabelAttrCapabilities();
}

FieldValueTuple MirrorSessionCapabilities::makeLabelCapDbEntry() const
{
    auto field = CAPABILITY_MIRROR_SESSION_LABEL_CAPABLE_FIELD;
    auto value = toStr(isMirrorSessionLabelSupported());

    return FieldValueTuple(field, value);
}

void MirrorSessionCapabilities::writeCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    DBConnector stateDb(SWITCH_STATE_DB_NAME, SWITCH_STATE_DB_TIMEOUT);
    Table capTable(&stateDb, STATE_SWITCH_CAPABILITY_TABLE_NAME);

    std::vector<FieldValueTuple> fvList = {
        makeLabelCapDbEntry()
    };

    capTable.set(CAPABILITY_KEY, fvList);

    SWSS_LOG_NOTICE(
        "Wrote mirror session label capabilities to State DB: %s key",
        capTable.getKeyName(CAPABILITY_KEY).c_str()
    );
}
