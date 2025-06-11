#include "hashorch.h"

#include <nlohmann/json.hpp>

using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_hash_api_t* sai_hash_api;

namespace {

const std::unordered_map<std::string, sai_native_hash_field_t> hash_field_map =
    {{"src_ip", SAI_NATIVE_HASH_FIELD_SRC_IP},
     {"dst_ip", SAI_NATIVE_HASH_FIELD_DST_IP},
     {"l4_src_port", SAI_NATIVE_HASH_FIELD_L4_SRC_PORT},
     {"l4_dst_port", SAI_NATIVE_HASH_FIELD_L4_DST_PORT},
     {"ipv6_flow_label", SAI_NATIVE_HASH_FIELD_IPV6_FLOW_LABEL},
     {"ip_protocol", SAI_NATIVE_HASH_FIELD_IP_PROTOCOL}};

// Parses hash_field_list in json format
//   e.g. "[\"src_ip\", \"dst_ip\", \"l4_src_port\", \"l4_dst_port\",
//          \"ip_protocol\"]"
// to get a list of hash fields in it. The parsed fields will be stored in
// parsed_hash_field_list. Return error on failure.
ReturnCode parseHashFieldList(const std::string& hash_field_list,
                              std::vector<int32_t>* parsed_hash_field_list) {
  SWSS_LOG_ENTER();

  if (parsed_hash_field_list == nullptr) {
    LOG_ERROR_AND_RETURN(ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM,
                                    "parsed_hash_field_list is nullptr."));
  }

  try {
    nlohmann::json j = nlohmann::json::parse(hash_field_list);
    if (!j.is_array()) {
      LOG_ERROR_AND_RETURN(ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Failed to parse hash field list "
                           << hash_field_list << ". It is not an array.");
    }

    parsed_hash_field_list->clear();
    for (const auto& field : j) {
      std::string str_field = field.get<std::string>();
      if (hash_field_map.find(str_field) == hash_field_map.end()) {
        LOG_ERROR_AND_RETURN(ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Failed to parse hash field list "
                             << hash_field_list << ". Unknown field "
                             << str_field);
      }
      parsed_hash_field_list->push_back(hash_field_map.at(str_field));
    }
  } catch (std::exception& ex) {
    LOG_ERROR_AND_RETURN(ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM)
                         << "Failed to parse hash field list "
                         << hash_field_list << ". Exception " << ex.what());
  }

  return ReturnCode();
}

// Prepares attrs for SAI call based on given field_value_pairs. Returns error
// on failure.
// Note that SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST attribute in attrs points to
// the data of *parsed_hash_field_list.
ReturnCode prepareAttributes(
    const std::vector<swss::FieldValueTuple>& field_value_pairs,
    std::vector<sai_attribute_t>* attrs,
    std::vector<int32_t>* parsed_hash_field_list) {
  SWSS_LOG_ENTER();

  if (attrs == nullptr) {
    LOG_ERROR_AND_RETURN(
        ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM,
                   "Failed to prepare attributes. attrs is nullptr."));
  }
  if (parsed_hash_field_list == nullptr) {
    LOG_ERROR_AND_RETURN(ReturnCode(
        swss::StatusCode::SWSS_RC_INVALID_PARAM,
        "Failed to prepare attributes. parsed_hash_field_list is nullptr."));
  }

  sai_attribute_t attr;
  for (const auto& field_value_pair : field_value_pairs) {
    const auto& field = fvField(field_value_pair);
    const auto& value = fvValue(field_value_pair);

    if (field == "hash_field_list") {
      attr.id = SAI_HASH_ATTR_NATIVE_HASH_FIELD_LIST;
      RETURN_IF_ERROR(parseHashFieldList(value, parsed_hash_field_list));
      attr.value.s32list.count = (uint32_t)parsed_hash_field_list->size();
      attr.value.s32list.list = parsed_hash_field_list->data();
      attrs->push_back(attr);
    } else {
      LOG_ERROR_AND_RETURN(
          ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM)
          << "Failed to prepare attributes due to unsupported field: "
          << field.c_str());
    }
  }

  return ReturnCode();
}

}  // namespace

HashOrch::HashOrch(swss::DBConnector* db, std::string tableName)
    : Orch(db, tableName) {}

bool HashOrch::getHashObjectId(const std::string& hash_object_name,
                               sai_object_id_t* hash_object_oid) {
  SWSS_LOG_ENTER();

  if (hash_object_oid == nullptr) {
    SWSS_LOG_ERROR("hash_object_oid is nullptr.");
    return false;
  }

  if (m_hash_objects.find(hash_object_name) == m_hash_objects.end()) {
    SWSS_LOG_NOTICE("Hash object with name (%s) doesn't exist.",
                    hash_object_name.c_str());
    return false;
  }

  *hash_object_oid = m_hash_objects.at(hash_object_name).oid;
  return true;
}

void HashOrch::increaseRefCount(const std::string& hash_object_name) {
  SWSS_LOG_ENTER();

  if (m_hash_objects.find(hash_object_name) == m_hash_objects.end()) {
    SWSS_LOG_NOTICE("Hash object with name (%s) doesn't exist.",
                    hash_object_name.c_str());
    return;
  }
  m_hash_objects.at(hash_object_name).ref_count++;
}

void HashOrch::decreaseRefCount(const std::string& hash_object_name) {
  SWSS_LOG_ENTER();

  if (m_hash_objects.find(hash_object_name) == m_hash_objects.end()) {
    SWSS_LOG_NOTICE("Hash object with name (%s) doesn't exist.",
                    hash_object_name.c_str());
    return;
  }
  m_hash_objects.at(hash_object_name).ref_count--;
}

void HashOrch::doTask(Consumer& consumer) {
  SWSS_LOG_ENTER();

  auto it = consumer.m_toSync.begin();
  while (it != consumer.m_toSync.end()) {
    auto t = it->second;
    auto key = kfvKey(t);
    auto op = kfvOp(t);
    const auto hash_object = m_hash_objects.find(key);

    ReturnCode status;
    if (op == SET_COMMAND) {
      if (hash_object == m_hash_objects.end()) {
        status = createHashObject(key, kfvFieldsValues(t));
      } else {
        status = updateHashObject(hash_object->second, kfvFieldsValues(t));
      }
    } else if (op == DEL_COMMAND) {
      status = deleteHashObject(key);
    } else {
      status = ReturnCode(swss::StatusCode::SWSS_RC_INVALID_PARAM)
               << "Unsupported operation: " << op.c_str();
      SWSS_LOG_ERROR("%s", status.message().c_str());
    }

    m_publisher.publish(consumer.getTableName(), key, kfvFieldsValues(t),
                        status);
    it = consumer.m_toSync.erase(it);
  }
}

ReturnCode HashOrch::createHashObject(
    const std::string& hash_object_name,
    const std::vector<FieldValueTuple>& field_value_pairs) {
  SWSS_LOG_ENTER();

  HashObject hash_object;
  hash_object.name = hash_object_name;

  std::vector<int32_t> parsed_hash_field_list;
  std::vector<sai_attribute_t> attrs;
  RETURN_IF_ERROR(
      prepareAttributes(field_value_pairs, &attrs, &parsed_hash_field_list));
  for (const auto field : parsed_hash_field_list) {
    hash_object.hash_fields.insert(field);
  }

  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_hash_api->create_hash(&hash_object.oid, gSwitchId,
                                (uint32_t)attrs.size(), attrs.data()),
      "Failed to create hash object with name " << hash_object_name);

  m_hash_objects.emplace(hash_object_name, hash_object);

  return ReturnCode();
}

ReturnCode HashOrch::updateHashObject(
    const HashObject& hash_object,
    const std::vector<swss::FieldValueTuple>& field_value_pairs) {
  SWSS_LOG_ENTER();

  HashObject hash_object_tmp;
  std::vector<int32_t> parsed_hash_field_list;
  std::vector<sai_attribute_t> attrs;
  RETURN_IF_ERROR(
      prepareAttributes(field_value_pairs, &attrs, &parsed_hash_field_list));
  for (const auto field : parsed_hash_field_list) {
    hash_object_tmp.hash_fields.insert(field);
  }

  // If there is no diff, skip SAI call.
  if (hash_object.hash_fields == hash_object_tmp.hash_fields)
    return ReturnCode();

  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_hash_api->set_hash_attribute(hash_object.oid, attrs.data()),
      "Failed to update hash object with name " << hash_object.name);

  return ReturnCode();
}

ReturnCode HashOrch::deleteHashObject(const std::string& hash_object_name) {
  SWSS_LOG_ENTER();

  const auto it_hash_object = m_hash_objects.find(hash_object_name);
  if (it_hash_object == m_hash_objects.end()) {
    return ReturnCode();
  }
  const auto& hash_object = it_hash_object->second;

  if (hash_object.ref_count > 0) {
    LOG_ERROR_AND_RETURN(ReturnCode(swss::StatusCode::SWSS_RC_IN_USE)
                         << "Failed to delete hash object with name "
                         << hash_object_name << " with ref count "
                         << hash_object.ref_count);
  }

  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_hash_api->remove_hash(hash_object.oid),
      "Failed to delete hash object with name " << hash_object_name);

  m_hash_objects.erase(hash_object_name);

  return ReturnCode();
}
