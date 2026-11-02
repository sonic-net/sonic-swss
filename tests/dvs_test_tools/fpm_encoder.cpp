// Synthetic protocol fixture encoder; this is not an FRR producer integration.
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "fpmsyncd/fpm/fpm.h"

static uint32_t number(const std::string &text)
{
    char *end = nullptr;
    errno = 0;
    const auto value = strtoull(text.c_str(), &end, 0);
    if (errno || end == text.c_str() || *end || value > UINT32_MAX)
        throw std::runtime_error("invalid uint32 argument");
    return static_cast<uint32_t>(value);
}

static l2_nhg_member_msg member(uint32_t id, uint32_t kind,
                              const std::string &ip, const std::string &mac)
{
    l2_nhg_member_msg result{};
    if (kind > UINT8_MAX) throw std::runtime_error("invalid Member type");
    result.l2nm_nh_id = id;
    result.l2nm_type = static_cast<uint8_t>(kind);
    result.l2nm_family = ip.find(':') == std::string::npos ? AF_INET : AF_INET6;
    if (inet_pton(result.l2nm_family, ip.c_str(), &result.l2nm_remote_vtep) != 1)
        throw std::runtime_error("invalid IP address");
    unsigned octets[6];
    int consumed = 0;
    if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x%n", &octets[0], &octets[1],
               &octets[2], &octets[3], &octets[4], &octets[5], &consumed) != 6 ||
        static_cast<size_t>(consumed) != mac.size()) throw std::runtime_error("invalid MAC address");
    for (size_t i = 0; i < 6; ++i)
    {
        if (octets[i] > UINT8_MAX) throw std::runtime_error("invalid MAC octet");
        result.l2nm_router_mac[i] = static_cast<uint8_t>(octets[i]);
    }
    return result;
}

int main(int argc, char **argv)
{
    try
    {
        if (argc < 4) throw std::runtime_error("expected kind, id, add/del");
        const std::string kind = argv[1], operation = argv[3];
        if (operation != "add" && operation != "del") throw std::runtime_error("invalid operation");
        const bool deleting = operation == "del";
        const uint32_t id = number(argv[2]);
        uint16_t type = 0;
        std::vector<unsigned char> payload;
        auto append = [&](const void *data, size_t size) {
            const auto *bytes = static_cast<const unsigned char *>(data);
            payload.insert(payload.end(), bytes, bytes + size);
        };
        if (kind == "binding")
        {
            if (argc != 5 || id == 0 || id > INT32_MAX) throw std::runtime_error("invalid binding arguments");
            const uint32_t group = number(argv[4]);
            const evpn_backup_nhg_msg body{static_cast<int>(id), group};
            type = deleting ? RTM_FPM_DEL_EVPN_ES_BACKUP_NHG : RTM_FPM_ADD_EVPN_ES_BACKUP_NHG;
            append(&body, sizeof(body));
        }
        else if (kind == "member")
        {
            if (argc != 7) throw std::runtime_error("invalid Member arguments");
            const auto body = member(id, number(argv[4]), argv[5], argv[6]);
            type = deleting ? RTM_FPM_DEL_L2_NEXTHOP : RTM_FPM_ADD_L2_NEXTHOP;
            append(&body, sizeof(body));
        }
        else if (kind == "group")
        {
            std::vector<l2_nhg_member_msg> members;
            for (int i = 4; i < argc; ++i)
            {
                // ID@IP@MAC supplies complete records; bare IDs use fixture defaults.
                const std::string text = argv[i];
                const auto first = text.find('@'), second = first == std::string::npos ? first : text.find('@', first + 1);
                if (first != std::string::npos && second == std::string::npos)
                    throw std::runtime_error("expected ID@IP@MAC");
                members.push_back(member(number(text.substr(0, first)), 1,
                    first == std::string::npos ? "10.10.10.2" : text.substr(first + 1, second - first - 1),
                    first == std::string::npos ? "02:00:00:00:00:02" : text.substr(second + 1)));
            }
            std::sort(members.begin(), members.end(), [](const l2_nhg_member_msg &a, const l2_nhg_member_msg &b) {
                return a.l2nm_nh_id < b.l2nm_nh_id;
            });
            if (members.size() > L2_NHG_MAX_MEMBERS) throw std::runtime_error("too many Members");
            l2_nhg_group_msg body{};
            body.l2ng_nhg_id = id;
            body.l2ng_count = static_cast<uint32_t>(members.size());
            for (size_t i = 0; i < L2_NHG_ESI_LEN; ++i) body.l2ng_esi[i] = static_cast<uint8_t>(i);
            type = deleting ? RTM_FPM_DEL_L2_NEXTHOP_GROUP : RTM_FPM_ADD_L2_NEXTHOP_GROUP;
            append(&body, sizeof(body));
            for (const auto &entry : members) append(&entry, sizeof(entry));
        }
        else if (kind == "replay_done")
        {
            if (argc != 4) throw std::runtime_error("replay-done has no payload");
            type = RTM_FPM_EVPN_MH_REPLAY_DONE;
        }
        else throw std::runtime_error("unknown message kind");
        nlmsghdr header{};
        header.nlmsg_len = static_cast<uint32_t>(NLMSG_LENGTH(payload.size()));
        header.nlmsg_type = type;
        header.nlmsg_flags = NLM_F_CREATE | NLM_F_REQUEST;
        const size_t size = FPM_MSG_HDR_LEN + NLMSG_ALIGN(header.nlmsg_len);
        if (size > FPM_MAX_MSG_LEN) throw std::runtime_error("frame exceeds FPM limit");
        std::vector<unsigned char> output(size);
        const fpm_msg_hdr_t fpm{FPM_PROTO_VERSION, FPM_MSG_TYPE_NETLINK, htons(static_cast<uint16_t>(size))};
        memcpy(output.data(), &fpm, sizeof(fpm));
        memcpy(output.data() + sizeof(fpm), &header, sizeof(header));
        if (!payload.empty()) memcpy(output.data() + sizeof(fpm) + NLMSG_HDRLEN, payload.data(), payload.size());
        if (fwrite(output.data(), output.size(), 1, stdout) != 1) return 1;
        return 0;
    }
    catch (const std::exception &error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
