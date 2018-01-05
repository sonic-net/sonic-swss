#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <exception>

#include "sai.h"
#include "macaddress.h"
#include "orch.h"
#include "request_parser.h"
#include "vrforch.h"

VRFOrch::VRFOrch(DBConnector *db, const std::string& tableName) : Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

void VRFOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    VRFRequest request;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        try
        {
            request.parse(it->second);
            it = consumer.m_toSync.erase(it);
        }
        catch (const std::invalid_argument& e)
        {
            SWSS_LOG_ERROR("Parse error: %s", e.what());
            ++it;
        }
        catch (const std::runtime_error& e)
        {
            SWSS_LOG_ERROR("Parse error: %s", e.what());
            ++it;
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_ERROR("Exception was catched in request parser: %s", e.what());
            ++it;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Unknown exception was catched in request parser");
            ++it;
        }

        request.clear();
    }
}


bool VRFOrch::AddVRF(const VRFRequest& request)
{
    SWSS_LOG_ENTER();
    return true;
}

bool VRFOrch::DeleteVRF(const VRFRequest& request)
{
    SWSS_LOG_ENTER();
    return true;
}
