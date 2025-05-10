#pragma once

#include <cstdbool>
#include <cstdint>

#include <unordered_map>
#include <unordered_set>
#include <string>

class BufferContainer
{
public:
    BufferContainer() = default;
    virtual ~BufferContainer() = default;

    std::unordered_map<std::string, std::string> fieldValueMap;
};

class BufferProfileConfig final : public BufferContainer
{
public:
    BufferProfileConfig() = default;
    ~BufferProfileConfig() = default;

    inline bool isTrimmingProhibited() const
    {
        return ((pgRefCount > 0) || (iBufProfListRefCount > 0) || (eBufProfListRefCount)) ? true : false;
    }

    std::uint64_t pgRefCount = 0;
    std::uint64_t iBufProfListRefCount = 0;
    std::uint64_t eBufProfListRefCount = 0;

    bool isTrimmingEligible = false;
};

class BufferPriorityGroupConfig final : public BufferContainer
{
public:
    BufferPriorityGroupConfig() = default;
    ~BufferPriorityGroupConfig() = default;

    struct {
        std::string value;
        bool is_set = false;
    } profile;
};

class IngressBufferProfileListConfig final : public BufferContainer
{
public:
    IngressBufferProfileListConfig() = default;
    ~IngressBufferProfileListConfig() = default;

    struct {
        std::unordered_set<std::string> value;
        bool is_set = false;
    } profile_list;
};

class EgressBufferProfileListConfig final : public BufferContainer
{
public:
    EgressBufferProfileListConfig() = default;
    ~EgressBufferProfileListConfig() = default;

    struct {
        std::unordered_set<std::string> value;
        bool is_set = false;
    } profile_list;
};
