#pragma once

#include <string>
#include <unordered_map>

#include "dbconnector.h"
#include "table.h"

extern "C"
{
#include "sai.h"
}

// Interface for mapping P4 ID to SAI OID.
// This class is not thread safe.
class P4OidMapper
{
  public:
    // This is a dummy value for non-oid based objects only.
    static constexpr sai_object_id_t kDummyOid = 0xdeadf00ddeadf00d;

    P4OidMapper();
    ~P4OidMapper() = default;

    // Sets oid for the given key for the specific object_type. Returns false if
    // the key already exists.
    bool setOID(_In_ sai_object_type_t object_type, _In_ const std::string &key, _In_ sai_object_id_t oid,
                _In_ uint32_t ref_count = 0);

    // Sets dummy oid for the given key for the specific object_type. Should only
    // be used for non-oid based object type. Returns false if the key
    // already exists.
    bool setDummyOID(_In_ sai_object_type_t object_type, _In_ const std::string &key, _In_ uint32_t ref_count = 0)
    {
        return setOID(object_type, key, /*oid=*/kDummyOid, ref_count);
    }

    // Gets oid for the given key for the SAI object_type.
    // Returns true on success.
    bool getOID(_In_ sai_object_type_t object_type, _In_ const std::string &key, _Out_ sai_object_id_t *oid);

    // Gets the reference count for the given key for the SAI object_type.
    // Returns true on success.
    bool getRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key, _Out_ uint32_t *ref_count);

    // Erases oid for the given key for the SAI object_type.
    // This function checks if the reference count is zero or not before the
    // operation.
    // Returns true on success.
    bool eraseOID(_In_ sai_object_type_t object_type, _In_ const std::string &key);

    // Erases all oids for the SAI object_type.
    // This function will erase all oids regardless of the reference counts.
    void eraseAllOIDs(_In_ sai_object_type_t object_type);

    // Gets the number of oids for the SAI object_type.
    size_t getNumEntries(_In_ sai_object_type_t object_type);

    // Checks whether OID mapping exists for the given key for the specific
    // object type.
    bool existsOID(_In_ sai_object_type_t object_type, _In_ const std::string &key);

    // Increases the reference count for the given object.
    // Returns true on success.
    bool increaseRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key);

    // Decreases the reference count for the given object.
    // Returns true on success.
    bool decreaseRefCount(_In_ sai_object_type_t object_type, _In_ const std::string &key);

  private:
    struct MapperEntry
    {
        sai_object_id_t sai_oid;
        uint32_t ref_count;
    };

    // Buckets of map tables, one for every SAI object type.
    std::unordered_map<std::string, MapperEntry> m_oidTables[SAI_OBJECT_TYPE_MAX];

    swss::DBConnector m_db;
    swss::Table m_table;
};
