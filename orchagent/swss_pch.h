#ifndef SWSS_ORCHAGENT_PCH_H
#define SWSS_ORCHAGENT_PCH_H

// C++ standard library (stable, never changes during development)
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <memory>
#include <utility>
#include <condition_variable>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <mutex>
#include <tuple>
#include <thread>
#include <deque>
#include <list>
#include <array>
#include <bitset>
#include <cassert>
#include <cstring>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <type_traits>

// SAI (118 sub-headers, 66K lines — the single heaviest include cost)
extern "C" {
#include <sai.h>
#include <saistatus.h>
}

// swsscommon (stable library headers, change only on package upgrades)
#include "dbconnector.h"
#include "table.h"
#include "consumertable.h"
#include "consumerstatetable.h"
#include "zmqconsumerstatetable.h"
#include "zmqserver.h"
#include "notificationconsumer.h"
#include "selectabletimer.h"
#include "macaddress.h"
#include "response_publisher.h"
#include "recorder.h"
#include "schema.h"
#include "producerstatetable.h"
#include "subscriberstatetable.h"
#include "converter.h"
#include "logger.h"
#include "tokenize.h"
#include "ipaddress.h"
#include "ipprefix.h"

#endif
