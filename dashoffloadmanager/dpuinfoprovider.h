#pragma once

#include <string>
#include <vector>

struct DpuInfo
{
    uint32_t    dpuId;
    std::string mgmtAddr;
    std::string interfaces;
};

bool getDpuInfo(std::vector<DpuInfo> &info);
