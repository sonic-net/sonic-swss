// includes -----------------------------------------------------------------------------------------------------------

#include "portcnt.h"

// Port container -----------------------------------------------------------------------------------------------------

PortContainer::PortContainer(const std::string &key, const std::string &op) noexcept
{
    this->key = key;
    this->op = op;
}

// Port config --------------------------------------------------------------------------------------------------------

PortConfig::PortConfig(const std::string &key, const std::string &op) noexcept :
    PortContainer(key, op)
{

}
