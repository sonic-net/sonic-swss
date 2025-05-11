#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "orch.h"

// HashOrch is responsible for managing hash objects for hash field selection.
class HashOrch : public Orch {
 public:
  HashOrch(swss::DBConnector *db, std::string tableName);

  HashOrch(const HashOrch &) = delete;
  HashOrch &operator=(const HashOrch &) = delete;

  // Gets hash object's ID based on its name.
  // Rerturns true on success and the OID will be stored in the space pointed by
  // hash_object_oid; otherwise, returns false.
  bool getHashObjectId(const std::string &hash_object_name,
                       sai_object_id_t *hash_object_oid);

  // Increases the reference count of the hash object with the specified ID by
  // one.
  void increaseRefCount(const std::string &hash_object_name);

  // Decreases the reference count of the hash object with the specified ID by
  // one.
  void decreaseRefCount(const std::string &hash_object_name);

  struct HashObject {
    std::string name = "";
    sai_object_id_t oid = 0;
    uint32_t ref_count = 0;

    std::unordered_set<int32_t> hash_fields;
  };

 private:
  void doTask(Consumer &consumer) override;

  ReturnCode createHashObject(
      const std::string &hash_object_name,
      const std::vector<swss::FieldValueTuple> &field_value_pairs);

  ReturnCode updateHashObject(
      const HashObject &hash_object,
      const std::vector<swss::FieldValueTuple> &field_value_pairs);

  ReturnCode deleteHashObject(const std::string &hash_object_name);

  // A map from hash object names to SAI hash object IDs.
  std::unordered_map<std::string, HashObject> m_hash_objects;
};