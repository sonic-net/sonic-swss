#pragma once

#include <unordered_map>
#include <string>

#include "buffercontainer.h"

class BufferHelper final
{
public:
    BufferHelper() = default;
    ~BufferHelper() = default;

    void parseBufferConfig(BufferProfileConfig &cfg) const;
    void parseBufferConfig(BufferPriorityGroupConfig &cfg) const;
    void parseBufferConfig(IngressBufferProfileListConfig &cfg) const;
    void parseBufferConfig(EgressBufferProfileListConfig &cfg) const;

    template<typename T>
    void setBufferConfig(const std::string &key, const T &cfg);
    template<typename T>
    bool getBufferConfig(T &cfg, const std::string &key) const;

    void delBufferProfileConfig(const std::string &key);
    void delBufferPriorityGroupConfig(const std::string &key);
    void delIngressBufferProfileListConfig(const std::string &key);
    void delEgressBufferProfileListConfig(const std::string &key);

private:
    template<typename T>
    auto getBufferObjMap() const -> const std::unordered_map<std::string, T>&;
    template<typename T>
    auto getBufferObjMap() -> std::unordered_map<std::string, T>&;

    template<typename T>
    void setObjRef(const T &cfg);
    template<typename T>
    void delObjRef(const T &cfg);

    std::unordered_map<std::string, BufferProfileConfig> profMap;
    std::unordered_map<std::string, BufferPriorityGroupConfig> pgMap;
    std::unordered_map<std::string, IngressBufferProfileListConfig> iBufProfListMap;
    std::unordered_map<std::string, EgressBufferProfileListConfig> eBufProfListMap;
};
