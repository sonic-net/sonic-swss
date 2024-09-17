#pragma once

#include "logger.h"
#include <string>

#define DASH_MNG_LOG_ERROR(MSG, ...)  SWSS_LOG_ERROR("[%s] - " MSG, m_dpuLogKey.c_str(), ##__VA_ARGS__)
#define DASH_MNG_LOG_WARN(MSG, ...)   SWSS_LOG_WARN("[%s] - " MSG, m_dpuLogKey.c_str(), ##__VA_ARGS__)
#define DASH_MNG_LOG_NOTICE(MSG, ...) SWSS_LOG_NOTICE("[%s] - " MSG, m_dpuLogKey.c_str(), ##__VA_ARGS__)
#define DASH_MNG_LOG_INFO(MSG, ...)   SWSS_LOG_INFO("[%s] - " MSG, m_dpuLogKey.c_str(), ##__VA_ARGS__)
#define DASH_MNG_LOG_DEBUG(MSG, ...)  SWSS_LOG_DEBUG("[%s] - " MSG, m_dpuLogKey.c_str(), ##__VA_ARGS__)

inline std::string makeDpuLogKey(uint32_t dpuId)
{
    return "DPU" + std::to_string(dpuId);
}
