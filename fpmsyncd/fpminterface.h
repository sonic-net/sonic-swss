#pragma once

#include <swss/selectable.h>
#include <libnl3/netlink/netlink.h>

#include "fpmsyncd/fpm/fpm.h"

namespace swss
{

/**
 * @brief FPM zebra communication interface
 */
class FpmInterface : public Selectable
{
public:
    virtual ~FpmInterface() = default;

    /**
     * @brief Send netlink message through FPM socket
     * @param msg Netlink message
     * @return True on success, otherwise false is returned
     */
    virtual bool send(nlmsghdr* nl_hdr) = 0;
};

}
