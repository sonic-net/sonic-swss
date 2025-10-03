#include "p4oidmapper.h"

#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "logger.h"
#include "sai_serialize.h"

extern "C"
{
#include "sai.h"
}

using ::nlohmann::json;

P4OidMapper::P4OidMapper() : m_db("APPL_STATE_DB", 0) {}

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
    return true;
}

bool P4OidMapper::getOID(_In_ sai_object_type_t object_type, _In_ const std::string &key,
                         _Out_ sai_object_id_t *oid) const
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

    *oid = m_oidTables[object_type].at(key).sai_oid;
    return true;
}

bool P4OidMapper::getRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key,
                              _Out_ uint32_t *ref_count) const
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

    *ref_count = m_oidTables[object_type].at(key).ref_count;
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
    return true;
}

void P4OidMapper::eraseAllOIDs(_In_ sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    m_oidTables[object_type].clear();
}

size_t P4OidMapper::getNumEntries(_In_ sai_object_type_t object_type) const
{
    SWSS_LOG_ENTER();

    return (m_oidTables[object_type].size());
}

bool P4OidMapper::existsOID(_In_ sai_object_type_t object_type, _In_ const std::string &key) const
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

std::string P4OidMapper::verifyOIDMapping(_In_ sai_object_type_t object_type, _In_ const std::string &key,
                                          _In_ sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    sai_object_id_t mapper_oid;
    if (!getOID(object_type, key, &mapper_oid))
    {
        std::stringstream msg;
        msg << "OID not found in mapper for key " << key;
        return msg.str();
    }
    if (mapper_oid != oid)
    {
        std::stringstream msg;
        msg << "OID mismatched in mapper for key " << key << ": " << sai_serialize_object_id(oid) << " vs "
            << sai_serialize_object_id(mapper_oid);
        return msg.str();
    }

    return "";
}

std::string P4OidMapper::dumpStateCache() {
  json cache = json({});
  for (int i = 0; i < SAI_OBJECT_TYPE_MAX; i++) {
    if (m_oidTables[i].empty()) {
      continue;
    }

    json oid_mapper_j = json({});
    for (const auto& kv_pair : m_oidTables[i]) {
      MapperEntry m = kv_pair.second;
      json mapper_entry_j = {{"sai_oid", sai_serialize_object_id(m.sai_oid)}, {"ref_count", m.ref_count}};
      oid_mapper_j[kv_pair.first] = mapper_entry_j;
    }
    std::string sai_object_type = sai_serialize_object_type(static_cast<sai_object_type_t>(i));
    cache[sai_object_type] = oid_mapper_j;
  }
  return cache.dump(4);
}
