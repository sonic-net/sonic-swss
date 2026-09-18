#include "macsecmkastatus.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

using namespace std;
using namespace swss;

namespace
{

const set<string> SESSION_FIELDS = {
    "PAE KaY status",
    "Authenticated",
    "Secured",
    "Failed",
    "Actor Priority",
    "Key Server Priority",
    "Is Key Server",
    "Number of Keys Distributed",
    "Number of Keys Received",
    "MKA Hello Time",
    "actor_sci",
    "key_server_sci",
};

const set<string> PARTICIPANT_FIELDS = {
    "participant_idx",
    "ckn",
    "mi",
    "mn",
    "active",
    "retain",
    "is_principal",
    "is_primary",
    "live_peers",
    "potential_peers",
    "is_key_server",
    "is_elected",
};

bool parseBoolean(const string &value, bool &result)
{
    if (value == "Yes")
    {
        result = true;
        return true;
    }
    if (value == "No")
    {
        result = false;
        return true;
    }
    return false;
}

bool parseUint32(const string &value, uint32_t &result)
{
    if (value.empty() ||
        !all_of(value.begin(), value.end(), [](unsigned char c) { return isdigit(c); }))
    {
        return false;
    }

    try
    {
        const auto parsed = stoull(value);
        if (parsed > numeric_limits<uint32_t>::max())
        {
            return false;
        }
        result = static_cast<uint32_t>(parsed);
        return true;
    }
    catch (const exception &)
    {
        return false;
    }
}

bool normalizeHex(const string &value, string &normalized, size_t exactLength = 0)
{
    if (value.empty() || (value.size() % 2) != 0 ||
        (exactLength != 0 && value.size() != exactLength) ||
        !all_of(value.begin(), value.end(), [](unsigned char c) { return isxdigit(c); }))
    {
        return false;
    }

    normalized = value;
    transform(normalized.begin(), normalized.end(), normalized.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return true;
}

bool normalizeCkn(const string &value, string &normalized)
{
    return value.size() <= 128 && normalizeHex(value, normalized);
}

bool normalizeSci(const string &value, string &normalized)
{
    if (normalizeHex(value, normalized, 16))
    {
        return true;
    }

    const auto separator = value.find('@');
    if (separator == string::npos || value.find('@', separator + 1) != string::npos)
    {
        return false;
    }

    string mac;
    for (const auto c : value.substr(0, separator))
    {
        if (c == ':' || c == '-' || c == '.')
        {
            continue;
        }
        mac.push_back(c);
    }

    string normalizedMac;
    uint32_t port = 0;
    if (!normalizeHex(mac, normalizedMac, 12) ||
        !parseUint32(value.substr(separator + 1), port) ||
        port > numeric_limits<uint16_t>::max())
    {
        return false;
    }

    ostringstream stream;
    stream << normalizedMac << hex << nouppercase << setw(4) << setfill('0') << port;
    normalized = stream.str();
    return true;
}

bool requireFields(
    const map<string, string> &values,
    const set<string> &required,
    const string &scope,
    string &error)
{
    for (const auto &field : required)
    {
        if (values.find(field) == values.end())
        {
            error = scope + " is missing field '" + field + "'";
            return false;
        }
    }
    return true;
}

bool parseParticipant(
    const map<string, string> &values,
    MKAParticipantStatus &participant,
    string &error)
{
    if (!requireFields(values, PARTICIPANT_FIELDS, "participant", error))
    {
        return false;
    }

    if (!parseUint32(values.at("participant_idx"), participant.participantIndex) ||
        !normalizeCkn(values.at("ckn"), participant.ckn) ||
        !normalizeHex(values.at("mi"), participant.mi, 24) ||
        !parseUint32(values.at("mn"), participant.mn) ||
        !parseBoolean(values.at("active"), participant.active) ||
        !parseBoolean(values.at("retain"), participant.retain) ||
        !parseBoolean(values.at("is_principal"), participant.isPrincipal) ||
        !parseBoolean(values.at("is_primary"), participant.isPrimary) ||
        !parseUint32(values.at("live_peers"), participant.livePeers) ||
        !parseUint32(values.at("potential_peers"), participant.potentialPeers) ||
        !parseBoolean(values.at("is_key_server"), participant.isKeyServer) ||
        !parseBoolean(values.at("is_elected"), participant.isElected))
    {
        error = "participant contains a malformed known field";
        return false;
    }

    return true;
}

}

vector<FieldValueTuple> MKAParticipantStatus::toFieldValues() const
{
    return {
        {"participant_index", to_string(participantIndex)},
        {"mi", mi},
        {"mn", to_string(mn)},
        {"active", active ? "true" : "false"},
        {"retain", retain ? "true" : "false"},
        {"is_principal", isPrincipal ? "true" : "false"},
        {"is_primary", isPrimary ? "true" : "false"},
        {"live_peers", to_string(livePeers)},
        {"potential_peers", to_string(potentialPeers)},
        {"is_key_server", isKeyServer ? "true" : "false"},
        {"is_elected", isElected ? "true" : "false"},
    };
}

vector<FieldValueTuple> MKASessionStatus::toFieldValues() const
{
    return {
        {"kay_status", kayStatus},
        {"authenticated", authenticated ? "true" : "false"},
        {"secured", secured ? "true" : "false"},
        {"failed", failed ? "true" : "false"},
        {"actor_sci", actorSci},
        {"key_server_sci", keyServerSci},
        {"actor_priority", to_string(actorPriority)},
        {"key_server_priority", to_string(keyServerPriority)},
        {"is_key_server", isKeyServer ? "true" : "false"},
        {"keys_distributed", to_string(keysDistributed)},
        {"keys_received", to_string(keysReceived)},
        {"mka_hello_time_ms", to_string(mkaHelloTimeMs)},
    };
}

bool swss::parseMKAStatus(
    const string &output,
    MKASessionStatus &status,
    string &error)
{
    map<string, string> sessionValues;
    vector<map<string, string>> participantValues;
    map<string, string> *currentParticipant = nullptr;
    istringstream stream(output);
    string line;

    while (getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == string::npos)
        {
            error = "status contains a line without a field separator";
            return false;
        }

        const string field = line.substr(0, separator);
        const string value = line.substr(separator + 1);
        if (field == "participant_idx")
        {
            participantValues.emplace_back();
            currentParticipant = &participantValues.back();
        }

        auto &target = currentParticipant == nullptr ? sessionValues : *currentParticipant;
        const auto &knownFields = currentParticipant == nullptr ? SESSION_FIELDS : PARTICIPANT_FIELDS;
        if (knownFields.find(field) == knownFields.end())
        {
            continue;
        }
        if (!target.emplace(field, value).second)
        {
            error = "status contains duplicate field '" + field + "'";
            return false;
        }
    }

    if (!requireFields(sessionValues, SESSION_FIELDS, "session", error))
    {
        return false;
    }

    if (sessionValues.at("PAE KaY status") == "Active")
    {
        status.kayStatus = "active";
    }
    else if (sessionValues.at("PAE KaY status") == "Not-Active")
    {
        status.kayStatus = "not-active";
    }
    else
    {
        error = "session contains an invalid KaY status";
        return false;
    }

    if (!parseBoolean(sessionValues.at("Authenticated"), status.authenticated) ||
        !parseBoolean(sessionValues.at("Secured"), status.secured) ||
        !parseBoolean(sessionValues.at("Failed"), status.failed) ||
        !parseUint32(sessionValues.at("Actor Priority"), status.actorPriority) ||
        !parseUint32(sessionValues.at("Key Server Priority"), status.keyServerPriority) ||
        !parseBoolean(sessionValues.at("Is Key Server"), status.isKeyServer) ||
        !parseUint32(sessionValues.at("Number of Keys Distributed"), status.keysDistributed) ||
        !parseUint32(sessionValues.at("Number of Keys Received"), status.keysReceived) ||
        !parseUint32(sessionValues.at("MKA Hello Time"), status.mkaHelloTimeMs) ||
        !normalizeSci(sessionValues.at("actor_sci"), status.actorSci) ||
        !normalizeSci(sessionValues.at("key_server_sci"), status.keyServerSci))
    {
        error = "session contains a malformed known field";
        return false;
    }

    status.participants.clear();
    set<string> ckns;
    size_t principals = 0;
    size_t primaries = 0;
    for (const auto &values : participantValues)
    {
        MKAParticipantStatus participant;
        if (!parseParticipant(values, participant, error))
        {
            return false;
        }
        if (!ckns.insert(participant.ckn).second)
        {
            error = "status contains duplicate participant CKN";
            return false;
        }
        principals += participant.isPrincipal ? 1 : 0;
        primaries += participant.isPrimary ? 1 : 0;
        status.participants.push_back(participant);
    }

    if (principals > 1 || primaries > 1)
    {
        error = "status contains conflicting participant ownership or roles";
        return false;
    }

    return true;
}
