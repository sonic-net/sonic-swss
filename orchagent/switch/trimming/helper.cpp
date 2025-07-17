// includes -----------------------------------------------------------------------------------------------------------

extern "C" {
#include <saiswitch.h>
}

#include <cstdbool>
#include <cstdint>

#include <exception>
#include <string>

#include <boost/algorithm/string.hpp>

#include <converter.h>
#include <logger.h>

#include "schema.h"
#include "helper.h"

using namespace swss;

// constants ----------------------------------------------------------------------------------------------------------

static const std::uint8_t minDscp = 0;
static const std::uint8_t maxDscp = 63;

// functions ----------------------------------------------------------------------------------------------------------

static inline std::uint8_t toUInt8(const std::string &str)
{
    return to_uint<std::uint8_t>(str);
}

static inline std::uint32_t toUInt32(const std::string &str)
{
    return to_uint<std::uint32_t>(str);
}

// helper -------------------------------------------------------------------------------------------------------------

bool SwitchTrimmingHelper::isSymDscpMode(const SwitchTrimming &cfg) const
{
    return cfg.dscp.mode.is_set && (cfg.dscp.mode.value == SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_DSCP_VALUE);
}

bool SwitchTrimmingHelper::isStaticQueueMode(const SwitchTrimming &cfg) const
{
    return cfg.queue.mode.is_set && (cfg.queue.mode.value == SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC);
}

const SwitchTrimming& SwitchTrimmingHelper::getConfig() const
{
    return cfg;
}

void SwitchTrimmingHelper::setConfig(const SwitchTrimming &value)
{
    cfg = value;
}

bool SwitchTrimmingHelper::parseTrimSize(SwitchTrimming &cfg, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        cfg.size.value = toUInt32(value);
        cfg.size.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool SwitchTrimmingHelper::parseTrimDscp(SwitchTrimming &cfg, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    if (boost::algorithm::to_lower_copy(value) == SWITCH_TRIMMING_DSCP_VALUE_FROM_TC)
    {
        cfg.dscp.mode.value = SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_FROM_TC;
        cfg.dscp.mode.is_set = true;
        return true;
    }

    try
    {
        cfg.dscp.value = toUInt8(value);
        cfg.dscp.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    if (!((minDscp <= cfg.dscp.value) && (cfg.dscp.value <= maxDscp)))
    {
        SWSS_LOG_ERROR(
            "Failed to parse field(%s): value(%s) is out of range: %u <= dscp <= %u",
            field.c_str(), value.c_str(), minDscp, maxDscp
        );
        return false;
    }

    cfg.dscp.mode.value = SAI_PACKET_TRIM_DSCP_RESOLUTION_MODE_DSCP_VALUE;
    cfg.dscp.mode.is_set = true;

    return true;
}

bool SwitchTrimmingHelper::parseTrimTc(SwitchTrimming &cfg, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        cfg.tc.value = toUInt8(value);
        cfg.tc.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool SwitchTrimmingHelper::parseTrimQueue(SwitchTrimming &cfg, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    if (boost::algorithm::to_lower_copy(value) == SWITCH_TRIMMING_QUEUE_INDEX_DYNAMIC)
    {
        cfg.queue.mode.value = SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_DYNAMIC;
        cfg.queue.mode.is_set = true;
        return true;
    }

    try
    {
        cfg.queue.index.value = toUInt8(value);
        cfg.queue.index.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    cfg.queue.mode.value = SAI_PACKET_TRIM_QUEUE_RESOLUTION_MODE_STATIC;
    cfg.queue.mode.is_set = true;

    return true;
}

bool SwitchTrimmingHelper::parseTrimConfig(SwitchTrimming &cfg) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : cfg.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == SWITCH_TRIMMING_SIZE)
        {
            if (!parseTrimSize(cfg, field, value))
            {
                return false;
            }
        }
        else if (field == SWITCH_TRIMMING_DSCP_VALUE)
        {
            if (!parseTrimDscp(cfg, field, value))
            {
                return false;
            }
        }
        else if (field == SWITCH_TRIMMING_TC_VALUE)
        {
            if (!parseTrimTc(cfg, field, value))
            {
                return false;
            }
        }
        else if (field == SWITCH_TRIMMING_QUEUE_INDEX)
        {
            if (!parseTrimQueue(cfg, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return validateTrimConfig(cfg);
}

bool SwitchTrimmingHelper::validateTrimConfig(SwitchTrimming &cfg) const
{
    SWSS_LOG_ENTER();

    auto cond = cfg.size.is_set || cfg.dscp.mode.is_set || cfg.tc.is_set || cfg.queue.mode.is_set;

    if (!cond)
    {
        SWSS_LOG_ERROR("Validation error: missing valid fields");
        return false;
    }

    return true;
}
