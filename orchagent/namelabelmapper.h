#pragma once
#include <inttypes.h>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include "dbconnector.h"
#include "table.h"
extern "C" {
#include "sai.h"
}
#define UNIQUE_LABEL_SIZE 32
// Interface for mapping object name/key to unique Label.
// This class is not thread safe.
class NameLabelMapper {
 public:
  NameLabelMapper();
  ~NameLabelMapper() = default;
  // Sets label for the given key for the specific object_type. Returns false if
  // the key already exists.
  bool setLabel(_In_ sai_object_type_t object_type, _In_ const std::string& key,
                _In_ std::string& label);
  // Return true if label present in the mapper, and copy the label in the 3rd
  // argument; or false, and a new unique label is allocated and saved in mapper
  bool allocateLabel(_In_ sai_object_type_t object_type,
                     _In_ const std::string& key, _Out_ std::string& label);
  // Gets label from mapper for the given key for the SAI object_type.
  // Returns true on success.
  bool getLabel(_In_ sai_object_type_t object_type, _In_ const std::string& key,
                _Out_ std::string& label);
  // Erases label for the given key for the SAI object_type.
  // Returns true on success.
  bool eraseLabel(_In_ sai_object_type_t object_type,
                  _In_ const std::string& key);
  // Delete mapper table in db
  void deleteMapperInDb();
  // Gets the number of labels for the SAI object_type.
  size_t getNumEntries(_In_ sai_object_type_t object_type) const;
  // SAI object subtype and name to label mapper name
  // Returns concat of (subtype + object_name)
  // For example, for POLICER, there are 4 subtypes:
  // COPP trap group, ACL policer, storm policer, regular policer
  // One way is use APPL_DB table name in subtype field, that is
  // subtype = APPL_DB table name
  std::string generateKeyFromTableAndObjectName(std::string table_name,
                                                std::string object_name);

  // Add the unique label to an SAI attribute
  // return true if the label was already present;
  // return false if a new label is generated.
  bool addLabelToAttr(sai_object_type_t object_type,
                      const std::string& table_name, const std::string& key,
                      sai_attribute_t& attr, sai_attr_id_t attr_id,
                      std::string& mapper_key, std::string& label);

  // Save the all entries to state db
  void saveMapperToDb();
  // Read the all entries from state db
  void readMapperFromDb();
  // Returns a json string that contains each non-empty label mapper.
  std::string dumpStateCache();
  // Checks whether label mapping exists for the given key for the specific
  // object type.
  bool existsLabel(_In_ sai_object_type_t object_type,
                   _In_ const std::string& key) const;

  // Verify the given label in the label mapper
  std::string verifyLabelMapping(_In_ sai_object_type_t object_type,
                                 _In_ const std::string& key,
                                 _In_ std::string label);

 private:
  // Generate and return a unique label
  std::string generateUniqueLabel();
  // Buckets of map tables, one for every SAI object type.
  std::unordered_map<std::string, std::string>
      m_labelTables[SAI_OBJECT_TYPE_MAX];
  swss::DBConnector m_db;
  swss::Table m_table;
};
