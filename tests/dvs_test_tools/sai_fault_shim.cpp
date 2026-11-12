// DVS-only client-side SAI interposer. Never linked into production orchagent.
// Target selected bridge ports, NHG members and tunnel RMAC SETs.
// Other objects and API calls pass through.
extern "C" {
#include <sai.h>
}
#include <dlfcn.h>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace {
sai_bridge_api_t original{}, wrapped{};
sai_next_hop_group_api_t originalGroup{}, wrappedGroup{};
sai_switch_api_t originalSwitch{}, wrappedSwitch{};
sai_tunnel_api_t originalTunnel{}, wrappedTunnel{};
sai_fdb_api_t originalFdb{}, wrappedFdb{};
std::mutex apiMutex, traceMutex;
struct Rule {
    sai_object_id_t target = 0, group = 0, member = 0, tunnel = 0;
    std::string fail, mismatch;
    int32_t state = -1;
    sai_status_t status = SAI_STATUS_FAILURE;
    bool rejects(const std::string &stage) const {
        return ("," + fail + ",").find("," + stage + ",") != std::string::npos;
    }
};
Rule rule() {
    Rule result;
    const char *path = std::getenv("BPP_DVS_SAI_FAULT_FILE");
    if (!path) return result;
    std::ifstream input(path);
    std::map<std::string,std::string> fields;
    std::string line;
    while (std::getline(input,line)) {
        auto split = line.find('=');
        if (split != std::string::npos) fields[line.substr(0,split)] = line.substr(split+1);
    }
    try {
        if (fields.count("target")) result.target = std::stoull(fields.at("target"),nullptr,0);
        if (fields.count("group")) result.group = std::stoull(fields.at("group"),nullptr,0);
        if (fields.count("member")) result.member = std::stoull(fields.at("member"),nullptr,0);
        if (fields.count("tunnel")) result.tunnel = std::stoull(fields.at("tunnel"),nullptr,0);
        result.fail = fields["fail"]; result.mismatch = fields["mismatch"];
        if (fields.count("state")) result.state = static_cast<int32_t>(std::stol(fields.at("state")));
        if (fields["status"] == "unsupported") result.status = SAI_STATUS_NOT_SUPPORTED;
        else if (fields["status"] == "unimplemented") result.status = SAI_STATUS_NOT_IMPLEMENTED;
    } catch (...) { result = {}; }
    return result;
}
uint64_t now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void trace(const char *op, sai_object_id_t oid, sai_attr_id_t attr, int64_t value,
           sai_status_t status, int32_t observed=-1, int32_t programmed=-1, const char *mac="") {
    const char *path = std::getenv("BPP_DVS_SAI_TRACE_FILE");
    if (!path) return;
    std::lock_guard<std::mutex> lock(traceMutex);
    std::ofstream output(path,std::ios::app);
    output << "{\"time_ms\":" << now() << ",\"operation\":\"" << op
           << "\",\"oid\":" << oid << ",\"attr\":" << attr << ",\"value\":" << value
           << ",\"status\":" << status << ",\"observed\":" << observed
           << ",\"programmed\":" << programmed << ",\"mac\":\"" << mac << "\"}\n";
}
sai_status_t set(sai_object_id_t oid, const sai_attribute_t *attr) {
    auto fault = rule();
    const bool target = fault.target && fault.target == oid;
    const bool admin = attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE;
    const bool group = attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
    const bool clear = group && attr->value.oid == SAI_NULL_OBJECT_ID;
    const bool mode = attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE;
    const bool reject = target && ((admin && fault.rejects("admin")) ||
        (clear && fault.rejects("clear")) || (group && !clear && fault.rejects("group")) ||
        (mode && fault.rejects("mode")));
    auto status = reject ? fault.status : original.set_bridge_port_attribute(oid,attr);
    const bool adminState = attr->id == SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
    if (target && (admin || group || mode || adminState))
        trace("set",oid,attr->id,group ? static_cast<int64_t>(attr->value.oid) : adminState ? attr->value.booldata : attr->value.s32,status);
    return status;
}
sai_status_t get(sai_object_id_t oid, uint32_t count, sai_attribute_t *attrs) {
    auto fault = rule();
    const bool target = fault.target && fault.target == oid;
    auto status = target && (fault.rejects("get") || (fault.rejects("programming_get") && count == 4) ||
        (fault.rejects("clear_get") && count == 2)) ?
        fault.status : original.get_bridge_port_attribute(oid,count,attrs);
    int32_t observed=-1, programmed=-1;
    if (status == SAI_STATUS_SUCCESS) {
        for (uint32_t i=0;i<count;++i) {
            // Report a client-visible attribute mismatch; never seed ASIC_DB.
            if (target && count == 4) {
                if (fault.mismatch == "group" && attrs[i].id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID)
                    attrs[i].value.oid = SAI_NULL_OBJECT_ID;
                if (fault.mismatch == "mode" && attrs[i].id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE)
                    attrs[i].value.s32 = SAI_BRIDGE_PORT_PROTECTION_MODE_HARDWARE;
                if (fault.mismatch == "admin" && attrs[i].id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE)
                    attrs[i].value.s32 = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
            }
            if (attrs[i].id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE) {
                if (target && count == 4 && fault.state >= 0) attrs[i].value.s32 = fault.state;
                observed = attrs[i].value.s32;
            }
            if (attrs[i].id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE)
                programmed = attrs[i].value.s32;
        }
    }
    if (target) trace("get",oid,0,count,status,observed,programmed);
    return status;
}
sai_status_t removeBridge(sai_object_id_t oid) {
    auto status = original.remove_bridge_port(oid);
    if (rule().target == oid) trace("bridge_remove",oid,0,0,status);
    return status;
}
sai_status_t removeGroup(sai_object_id_t oid) {
    auto status = originalGroup.remove_next_hop_group(oid);
    if (rule().group == oid) trace("group_remove",oid,0,0,status);
    return status;
}
sai_status_t removeGroupMember(sai_object_id_t oid) {
    const auto fault = rule();
    // Read the actual owner solely for test tracing; no ASIC/STATE_DB seeding.
    sai_attribute_t owner{}; owner.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    const auto lookup = originalGroup.get_next_hop_group_member_attribute(oid,1,&owner);
    const bool target = fault.member == oid ||
        (fault.group && lookup == SAI_STATUS_SUCCESS && owner.value.oid == fault.group);
    const auto status = fault.member == oid && fault.rejects("member_remove") ? fault.status :
        originalGroup.remove_next_hop_group_member(oid);
    if (target) trace("member_remove",oid,owner.id,
        lookup == SAI_STATUS_SUCCESS ? static_cast<int64_t>(owner.value.oid) : 0,status);
    return status;
}
sai_status_t setTunnel(sai_object_id_t oid, const sai_attribute_t *attr) {
    const auto fault = rule();
    const bool target = fault.tunnel && fault.tunnel == oid && attr->id == SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC;
    const auto status = target && fault.rejects("rmac") ? fault.status : originalTunnel.set_tunnel_attribute(oid,attr);
    if (target) {
        const auto &m = attr->value.mac; char mac[18];
        std::snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",static_cast<unsigned>(m[0]),
            static_cast<unsigned>(m[1]),static_cast<unsigned>(m[2]),static_cast<unsigned>(m[3]),
            static_cast<unsigned>(m[4]),static_cast<unsigned>(m[5]));
        trace("rmac_set",oid,attr->id,0,status,-1,-1,mac);
    }
    return status;
}
sai_status_t flushFdb(sai_object_id_t oid, uint32_t count, const sai_attribute_t *attrs) {
    sai_object_id_t port=0, vlan=0;
    for (uint32_t i=0;i<count;++i) {
        if (attrs[i].id==SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID) port=attrs[i].value.oid;
        if (attrs[i].id==SAI_FDB_FLUSH_ATTR_BV_ID) vlan=attrs[i].value.oid;
    }
    const auto status=originalFdb.flush_fdb_entries(oid,count,attrs);
    if (port && rule().target==port) trace("fdb_flush",port,SAI_FDB_FLUSH_ATTR_BV_ID,static_cast<int64_t>(vlan),status);
    return status;
}
sai_status_t getSwitch(sai_object_id_t oid, uint32_t count, sai_attribute_t *attrs) {
    auto status = originalSwitch.get_switch_attribute(oid,count,attrs);
    if (status == SAI_STATUS_SUCCESS) {
        for (uint32_t i=0;i<count;++i) {
            if (attrs[i].id != SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC) continue;
            auto &m = attrs[i].value.mac; char mac[18];
            std::snprintf(mac,sizeof(mac),"%02x:%02x:%02x:%02x:%02x:%02x",static_cast<unsigned>(m[0]),
                static_cast<unsigned>(m[1]),static_cast<unsigned>(m[2]),static_cast<unsigned>(m[3]),
                static_cast<unsigned>(m[4]),static_cast<unsigned>(m[5]));
            trace("default_mac",oid,attrs[i].id,0,status,-1,-1,mac);
        }
    }
    return status;
}

}
extern "C" sai_status_t sai_api_query(sai_api_t api, void **methods) {
    using Query = sai_status_t (*)(sai_api_t,void **);
    static auto query = reinterpret_cast<Query>(dlsym(RTLD_NEXT,"sai_api_query"));
    if (!query) return SAI_STATUS_FAILURE;
    auto status = query(api,methods);
    if (status == SAI_STATUS_SUCCESS && api == SAI_API_BRIDGE) {
        std::lock_guard<std::mutex> lock(apiMutex);
        original = *static_cast<sai_bridge_api_t *>(*methods);
        wrapped = original; wrapped.set_bridge_port_attribute = set; wrapped.get_bridge_port_attribute = get; wrapped.remove_bridge_port = removeBridge;
        *methods = &wrapped;
    }
    if (status == SAI_STATUS_SUCCESS && api == SAI_API_NEXT_HOP_GROUP) {
        std::lock_guard<std::mutex> lock(apiMutex);
        originalGroup = *static_cast<sai_next_hop_group_api_t *>(*methods);
        wrappedGroup = originalGroup; wrappedGroup.remove_next_hop_group = removeGroup;
        wrappedGroup.remove_next_hop_group_member = removeGroupMember; *methods = &wrappedGroup;
    }
    if (status == SAI_STATUS_SUCCESS && api == SAI_API_SWITCH) {
        std::lock_guard<std::mutex> lock(apiMutex);
        originalSwitch = *static_cast<sai_switch_api_t *>(*methods);
        wrappedSwitch = originalSwitch; wrappedSwitch.get_switch_attribute = getSwitch; *methods = &wrappedSwitch;
    }
    if (status == SAI_STATUS_SUCCESS && api == SAI_API_TUNNEL) {
        std::lock_guard<std::mutex> lock(apiMutex);
        originalTunnel = *static_cast<sai_tunnel_api_t *>(*methods);
        wrappedTunnel = originalTunnel; wrappedTunnel.set_tunnel_attribute = setTunnel; *methods = &wrappedTunnel;
    }
    if (status == SAI_STATUS_SUCCESS && api == SAI_API_FDB) {
        std::lock_guard<std::mutex> lock(apiMutex);
        originalFdb = *static_cast<sai_fdb_api_t *>(*methods);
        wrappedFdb = originalFdb; wrappedFdb.flush_fdb_entries = flushFdb; *methods = &wrappedFdb;
    }
    return status;
}
