#include "common/selectable.h"
#include "common/select.h"
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>

using namespace std;

namespace swss {

void Select::addSelectable(Selectable *c)
{
    m_objects.push_back(c);
}

void Select::removeSelectable(Selectable *c)
{
    for (auto i = m_objects.begin(); i != m_objects.end(); i++)
        if (*i == c)
        {
            m_objects.erase(i);
            return;
        }
}

void Select::addFd(int fd)
{
    m_fds.push_back(fd);
}

int Select::select(Selectable **c, int *fd, unsigned int timeout)
{
    struct timeval t = {0, (suseconds_t)(timeout)*1000};
    struct timeval *pTimeout = NULL;
    fd_set fs;
    int err;

    FD_ZERO(&fs);
    *c = NULL;
    *fd = 0;
    if (timeout != std::numeric_limits<unsigned int>::max())
        pTimeout = &t;

    /* Checking caching from reader */
    for (Selectable *i : m_objects)
    {
        err = i->readCache();
        if (err == Selectable::ERROR)
            return Select::ERROR;
        else if (err == Selectable::DATA) {
            *c = i;
            return Select::OBJECT;
        }
        /* else, timeout = no data */

        i->addFd(&fs);
    }

    for (int fd : m_fds)
    {
        FD_SET(fd, &fs);
    }

    err = ::select(FD_SETSIZE, &fs, NULL, NULL, pTimeout);
    if (err < 0)
        return Select::ERROR;
    if (err == 0)
        return Select::TIMEOUT;

    /* Check other consumer-table */
    for (Selectable *i : m_objects)
        if (i->isMe(&fs))
        {
            i->readMe();
            *c = i;
            return Select::OBJECT;
        }

    /* Check other FDs */
    for (int f : m_fds)
        if (FD_ISSET(f, &fs))
        {
            *fd = f;
            return Select::FD;
        }

    /* Shouldn't reach here */
    return Select::ERROR;
}

};
