#include "p4oidmapper.h"

#include <limits>
#include <string>

#include "logger.h"
#include "sai_serialize.h"

extern "C"
{
#include "sai.h"
}

namespace
{

std::string convertToDBField(_In_ const sai_object_type_t object_type, _In_ const std::string &key)
{
    return sai_serialize_object_type(object_type) + ":" + key;
}

} // namespace

P4OidMapper::P4OidMapper() : m_db("APPL_STATE_DB", 0), m_table(&m_db, "P4RT_KEY_TO_OID")
{
}

bool P4OidMapper::setOID(_In_ sai_object_type_t object_type, _In_ const std::string &key, _In_ sai_object_id_t oid,
                         _In_ uint32_t ref_count)
{
    SWSS_LOG_ENTER();

    if (m_oidTables[object_type].find(key) != m_oidTables[object_type].end())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d already exists in centralized mapper", key.c_str(), object_type);
        return false;
    }

    m_oidTables[object_type][key] = {oid, ref_count};
    m_table.hset("", convertToDBField(object_type, key), sai_serialize_object_id(oid));
    return true;
}

bool P4OidMapper::getOID(_In_ sai_object_type_t object_type, _In_ const std::string &key, _Out_ sai_object_id_t *oid)
{
    SWSS_LOG_ENTER();

    if (oid == nullptr)
    {
        SWSS_LOG_ERROR("nullptr input in centralized mapper");
        return false;
    }

    if (m_oidTables[object_type].find(key) == m_oidTables[object_type].end())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d does not exist in centralized mapper", key.c_str(), object_type);
        return false;
    }

    *oid = m_oidTables[object_type][key].sai_oid;
    return true;
}

bool P4OidMapper::getRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key,
                              _Out_ uint32_t *ref_count)
{
    SWSS_LOG_ENTER();

    if (ref_count == nullptr)
    {
        SWSS_LOG_ERROR("nullptr input in centralized mapper");
        return false;
    }

    if (m_oidTables[object_type].find(key) == m_oidTables[object_type].end())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d does not exist in "
                       "centralized mapper",
                       key.c_str(), object_type);
        return false;
    }

    *ref_count = m_oidTables[object_type][key].ref_count;
    return true;
}

bool P4OidMapper::eraseOID(_In_ sai_object_type_t object_type, _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    if (m_oidTables[object_type].find(key) == m_oidTables[object_type].end())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d does not exist in "
                       "centralized mapper",
                       key.c_str(), object_type);
        return false;
    }

    if (m_oidTables[object_type][key].ref_count != 0)
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d has non-zero reference count in "
                       "centralized mapper",
                       key.c_str(), object_type);
        return false;
    }

    m_oidTables[object_type].erase(key);
    m_table.hdel("", convertToDBField(object_type, key));
    return true;
}

void P4OidMapper::eraseAllOIDs(_In_ sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    m_oidTables[object_type].clear();
    m_table.del("");
}

size_t P4OidMapper::getNumEntries(_In_ sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    return (m_oidTables[object_type].size());
}

bool P4OidMapper::existsOID(_In_ sai_object_type_t object_type, _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    return m_oidTables[object_type].find(key) != m_oidTables[object_type].end();
}

bool P4OidMapper::increaseRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    if (m_oidTables[object_type].find(key) == m_oidTables[object_type].end())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d does not exist in "
                       "centralized mapper",
                       key.c_str(), object_type);
        return false;
    }

    if (m_oidTables[object_type][key].ref_count == std::numeric_limits<uint32_t>::max())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d reached maximum ref_count %u in "
                       "centralized mapper",
                       key.c_str(), object_type, m_oidTables[object_type][key].ref_count);
        return false;
    }

    m_oidTables[object_type][key].ref_count++;
    return true;
}

bool P4OidMapper::decreaseRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    if (m_oidTables[object_type].find(key) == m_oidTables[object_type].end())
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d does not exist in "
                       "centralized mapper",
                       key.c_str(), object_type);
        return false;
    }

    if (m_oidTables[object_type][key].ref_count == 0)
    {
        SWSS_LOG_ERROR("Key %s with SAI object type %d reached zero ref_count in "
                       "centralized mapper",
                       key.c_str(), object_type);
        return false;
    }

    m_oidTables[object_type][key].ref_count--;
    return true;
}
