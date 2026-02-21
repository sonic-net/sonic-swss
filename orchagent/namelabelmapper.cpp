#include "namelabelmapper.h"
#include <limits>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>
#include "logger.h"
#include "sai_serialize.h"
extern "C" {
#include "sai.h"
}
using ::nlohmann::json;
namespace {
std::string convertToDBField(_In_ const sai_object_type_t object_type,
                             _In_ const std::string& key) {
  return sai_serialize_object_type(object_type) + "|" + key;
}
}  // namespace
NameLabelMapper::NameLabelMapper()
    : m_db("STATE_DB", 0), m_table(&m_db, "SAI_KEY_LABEL_MAP") {}
bool NameLabelMapper::setLabel(_In_ sai_object_type_t object_type,
                               _In_ const std::string& key,
                               _In_ std::string& label) {
  SWSS_LOG_ENTER();
  if (existsLabel(object_type, key)) {
    SWSS_LOG_ERROR(
        "Key %s with SAI object type %d already exists in label mapper",
        key.c_str(), object_type);
    return false;
  }
  m_labelTables[object_type][key] = label;
  SWSS_LOG_INFO("Created new label %s for Key %s with SAI object type %d",
                label.c_str(), key.c_str(), object_type);
  return true;
}
bool NameLabelMapper::getLabel(_In_ sai_object_type_t object_type,
                               _In_ const std::string& key,
                               _Out_ std::string& label) {
  SWSS_LOG_ENTER();
  if (!existsLabel(object_type, key)) {
    SWSS_LOG_INFO(
        "Key %s with SAI object type %d does not exist in label mapper",
        key.c_str(), object_type);
    return false;
  }
  label = m_labelTables[object_type][key];
  return true;
}
bool NameLabelMapper::eraseLabel(_In_ sai_object_type_t object_type,
                                 _In_ const std::string& key) {
  SWSS_LOG_ENTER();
  if (!existsLabel(object_type, key)) {
    SWSS_LOG_ERROR(
        "Key %s with SAI object type %d does not exist in "
        "label mapper when erasing",
        key.c_str(), object_type);
    return false;
  }
  m_labelTables[object_type].erase(key);
  return true;
}
void NameLabelMapper::deleteMapperInDb() {
  SWSS_LOG_ENTER();
  m_table.del("");
}
size_t NameLabelMapper::getNumEntries(
    _In_ sai_object_type_t object_type) const {
  SWSS_LOG_ENTER();
  return (m_labelTables[object_type].size());
}
bool NameLabelMapper::existsLabel(_In_ sai_object_type_t object_type,
                                  _In_ const std::string& key) const {
  SWSS_LOG_ENTER();
  return m_labelTables[object_type].find(key) !=
         m_labelTables[object_type].end();
}
std::string NameLabelMapper::generateKeyFromTableAndObjectName(
    std::string table_name, std::string object_name) {
  return table_name + ":" + object_name;
}
std::string NameLabelMapper::generateUniqueLabel() {
  char label_buf[UNIQUE_LABEL_SIZE];
  uint64_t msec = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  snprintf(label_buf, UNIQUE_LABEL_SIZE, "%" PRIu64 "", msec);
  return std::string(reinterpret_cast<char const*>(label_buf));
}
bool NameLabelMapper::allocateLabel(_In_ sai_object_type_t object_type,
                                    _In_ const std::string& key,
                                    _Out_ std::string& label) {
  if (!getLabel(object_type, key, label)) {
    label = generateUniqueLabel();
    return false;
  }
  return true;
}

bool NameLabelMapper::addLabelToAttr(
    sai_object_type_t object_type, const std::string& table_name,
    const std::string& key, sai_attribute_t& attr, sai_attr_id_t attr_id,
    std::string& mapper_key, std::string& label) {
  mapper_key = generateKeyFromTableAndObjectName(table_name, key);
  bool label_present = allocateLabel(object_type, mapper_key, label);
  attr.id = attr_id;
  auto size = sizeof(attr.value.chardata);
  snprintf(attr.value.chardata, size, "%s", label.c_str());
  SWSS_LOG_NOTICE("Add ATTR_LABEL %s for sai object %s for %s", label.c_str(),
                  sai_serialize_object_type(object_type).c_str(),
                  mapper_key.c_str());
  return label_present;
}

std::string NameLabelMapper::dumpStateCache() {
  json cache = json({});
  for (int i = 0; i < SAI_OBJECT_TYPE_MAX; i++) {
    if (m_labelTables[i].empty()) {
      continue;
    }
    json label_mapper_j = json({});
    for (const auto& kv_pair : m_labelTables[i]) {
      label_mapper_j[kv_pair.first] = kv_pair.second;
    }
    std::string sai_object_type =
        sai_serialize_object_type(static_cast<sai_object_type_t>(i));
    cache[sai_object_type] = label_mapper_j;
  }
  return cache.dump(4);
}
void NameLabelMapper::saveMapperToDb() {
  for (int i = 0; i < SAI_OBJECT_TYPE_MAX; i++) {
    if (m_labelTables[i].empty()) {
      continue;
    }
    for (const auto& kv_pair : m_labelTables[i]) {
      auto key = kv_pair.first;
      auto label = kv_pair.second;
      m_table.hset("", convertToDBField(static_cast<sai_object_type_t>(i), key),
                   label);
      SWSS_LOG_INFO(
          "label %s for Key %s with SAI object type %d save into state_db",
          label.c_str(), key.c_str(), i);
    }
  }
}
void NameLabelMapper::readMapperFromDb() {
  std::vector<swss::FieldValueTuple> tuples;
  m_table.get("", tuples);
  SWSS_LOG_INFO("m_table->get size %zd", tuples.size());
  for (auto& fv : tuples) {
    std::string combo = fvField(fv);
    std::string label = fvValue(fv);
    SWSS_LOG_INFO("Got field %s label %s from db", combo.c_str(),
                  label.c_str());
    size_t pos = 0;
    std::string obj_type_str, key;
    if ((pos = combo.find("|")) != std::string::npos) {
      obj_type_str = combo.substr(0, pos);
      key = combo.substr(pos + 1, combo.size() - pos - 1);
      sai_object_type_t sai_object_type;
      sai_deserialize_object_type(obj_type_str, sai_object_type);
      setLabel(sai_object_type, key, label);
    }
  }
}

std::string NameLabelMapper::verifyLabelMapping(
    _In_ sai_object_type_t object_type, _In_ const std::string& key,
    _In_ std::string label) {
  SWSS_LOG_ENTER();

  std::string mapper_label;
  if (!getLabel(object_type, key, mapper_label)) {
    std::stringstream msg;
    msg << "Label not found in mapper for key " << key;
    return msg.str();
  }
  if (mapper_label != label) {
    std::stringstream msg;
    msg << "Label mismatched in mapper for key " << key << ": " << label
        << " vs " << mapper_label;
    return msg.str();
  }

  return "";
}
