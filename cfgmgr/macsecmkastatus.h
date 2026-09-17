#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <table.h>

namespace swss
{

struct MKAParticipantStatus
{
    std::uint32_t participantIndex;
    std::string ckn;
    std::string mi;
    std::uint32_t mn;
    bool active;
    bool participant;
    bool retain;
    bool isPrincipal;
    bool isPrimary;
    std::uint32_t livePeers;
    std::uint32_t potentialPeers;
    bool isKeyServer;
    bool isElected;

    std::vector<FieldValueTuple> toFieldValues() const;
};

struct MKASessionStatus
{
    std::string kayStatus;
    bool authenticated;
    bool secured;
    bool failed;
    std::uint32_t actorPriority;
    std::uint32_t keyServerPriority;
    bool isKeyServer;
    std::uint32_t keysDistributed;
    std::uint32_t keysReceived;
    std::uint32_t mkaHelloTimeMs;
    std::string actorSci;
    std::string keyServerSci;
    std::vector<MKAParticipantStatus> participants;

    std::vector<FieldValueTuple> toFieldValues() const;
};

bool parseMKAStatus(
    const std::string &output,
    MKASessionStatus &status,
    std::string &error);

}
