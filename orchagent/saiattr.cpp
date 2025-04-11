#include "orchagent/saiattr.h"

#include <swss/logger.h>
#include <sai_serialize.h>

SaiAttrWrapper::SaiAttrWrapper(sai_object_type_t objectType, const sai_attribute_t& attr)
{
    auto meta = sai_metadata_get_attr_metadata(objectType, attr.id);
    if (!meta)
    {
        SWSS_LOG_THROW("Failed to get attribute %d metadata", attr.id);
    }

    init(objectType, *meta, attr);
}

SaiAttrWrapper::~SaiAttrWrapper()
{
    if (m_meta)
    {
        sai_deserialize_free_attribute_value(m_meta->attrvaluetype, m_attr);
    }
}

SaiAttrWrapper::SaiAttrWrapper(const SaiAttrWrapper& other)
{
    init(other.m_objectType, *other.m_meta, other.m_attr);
}

SaiAttrWrapper& SaiAttrWrapper::operator=(const SaiAttrWrapper& other)
{
    init(other.m_objectType, *other.m_meta, other.m_attr);
    return *this;
}

SaiAttrWrapper::SaiAttrWrapper(SaiAttrWrapper&& other)
{
    swap(std::move(other));
}

SaiAttrWrapper& SaiAttrWrapper::operator=(SaiAttrWrapper&& other)
{
    swap(std::move(other));
    return *this;
}

bool SaiAttrWrapper::operator<(const SaiAttrWrapper& other) const
{
    return m_serializedAttr < other.m_serializedAttr;
}

const sai_attribute_t& SaiAttrWrapper::getSaiAttr() const
{
    return m_attr;
}

std::string SaiAttrWrapper::toString() const
{
    return m_serializedAttr;
}

sai_attr_id_t SaiAttrWrapper::getAttrId() const
{
    return m_attr.id;
}

void SaiAttrWrapper::swap(SaiAttrWrapper&& other)
{
    std::swap(m_objectType, other.m_objectType);
    std::swap(m_meta, other.m_meta);
    std::swap(m_attr, other.m_attr);
    std::swap(m_serializedAttr, other.m_serializedAttr);
}

void SaiAttrWrapper::init(
    sai_object_type_t objectType,
    const sai_attr_metadata_t& meta,
    const sai_attribute_t& attr)
{
    m_objectType = objectType;
    m_attr.id = attr.id;
    m_meta = &meta;

    m_serializedAttr = sai_serialize_attr_value(*m_meta, attr);

    // deserialize to actually preform a deep copy of attr
    // and attribute value's dynamically allocated lists.
    sai_deserialize_attr_value(m_serializedAttr, *m_meta, m_attr);
}
