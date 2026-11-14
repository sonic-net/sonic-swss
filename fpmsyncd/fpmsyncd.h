#ifndef __FPMSYNCD__
#define __FPMSYNCD__

#include <time.h>
#include "selectabletimer.h"

// redispipeline has a maximum capacity of 50000 entries
#define ROUTE_SYNC_PPL_SIZE 50000

/*
 * Arm a timer with a new interval. A SelectableTimer keeps running after it
 * fires, and start() does nothing on a running timer, so stop it first.
 */
inline void restartTimer(swss::SelectableTimer &timer, const timespec &interval)
{
    timer.stop();
    timer.setInterval(interval);
    timer.start();
}

#endif
