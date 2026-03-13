// includes -----------------------------------------------------------------------------------------------------------

#include <limits>

#include "orchagent/pbh/pbhcnt.h"

using namespace swss;

// PBH container ------------------------------------------------------------------------------------------------------

PbhContainer::PbhContainer(const std::string &key, const std::string &op) noexcept
{
    this->key = key;
    this->op = op;
}

std::uint64_t PbhContainer::getRefCount() const noexcept
{
    return this->refCount;
}

void PbhContainer::incrementRefCount() noexcept
{
    static const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();

    if (this->refCount < max)
    {
        this->refCount++;
    }
}

void PbhContainer::decrementRefCount() noexcept
{
    static const std::uint64_t min = std::numeric_limits<std::uint64_t>::min();

    if (this->refCount > min)
    {
        this->refCount--;
    }
}

void PbhContainer::clearRefCount() noexcept
{
    this->refCount = 0;
}

// PBH table ----------------------------------------------------------------------------------------------------------

PbhTable::PbhTable(const std::string &key, const std::string &op) noexcept :
    PbhContainer(key, op)
{

}

// PBH rule -----------------------------------------------------------------------------------------------------------

PbhRule::PbhRule(const std::string &key, const std::string &op) noexcept :
    PbhContainer(key, op)
{

}

// PBH hash -----------------------------------------------------------------------------------------------------------

PbhHash::PbhHash(const std::string &key, const std::string &op) noexcept :
    PbhContainer(key, op)
{

}

sai_object_id_t PbhHash::getOid() const noexcept
{
    return this->oid;
}

void PbhHash::setOid(sai_object_id_t oid) noexcept
{
    this->oid = oid;
}

// PBH hash field -----------------------------------------------------------------------------------------------------

PbhHashField::PbhHashField(const std::string &key, const std::string &op) noexcept :
    PbhContainer(key, op)
{

}

sai_object_id_t PbhHashField::getOid() const noexcept
{
    return this->oid;
}

void PbhHashField::setOid(sai_object_id_t oid) noexcept
{
    this->oid = oid;
}
