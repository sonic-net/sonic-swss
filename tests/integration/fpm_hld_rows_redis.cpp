// Actual Redis/ProducerStateTable durability and schema check. No SAI/DVS.
#include "fpmsyncd/routesync.h"
#include "fpmsyncd/fpm/fpm.h"
#include "lib/fpm_protection_ids.h"
#include "dbconnector.h"
#include "consumerstatetable.h"
#include <arpa/inet.h>
#include <cassert>
#include <iostream>
#include <memory>

using namespace swss;

void member(RouteSync &sync, RedisPipeline &pipeline, uint32_t id)
{
    struct { nlmsghdr h; l2_nhg_member_msg body; } frame{};
    frame.h.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP;
    frame.h.nlmsg_len = NLMSG_LENGTH(sizeof(frame.body));
    frame.body.l2nm_nh_id = id; frame.body.l2nm_type = 1; frame.body.l2nm_family = AF_INET;
    assert(inet_pton(AF_INET, "10.0.0.3", &frame.body.l2nm_remote_vtep) == 1);
    frame.body.l2nm_router_mac[0] = 2; frame.body.l2nm_router_mac[5] = 3;
    sync.onMsgRaw(&frame.h); pipeline.flush();
}
void group(RouteSync &sync, RedisPipeline &pipeline, uint32_t id, uint32_t nh = 0)
{
    struct { nlmsghdr h; uint32_t id, count, member; } frame{};
    frame.h.nlmsg_type = RTM_FPM_ADD_L2_NEXTHOP_GROUP;
    frame.h.nlmsg_len = NLMSG_LENGTH(nh ? 12 : 8);
    frame.id = id; frame.count = nh ? 1 : 0; frame.member = nh;
    sync.onMsgRaw(&frame.h); pipeline.flush();
}
std::map<std::string,std::string> read(Table &table, const std::string &key)
{
    std::vector<FieldValueTuple> fields;
    if (!table.get(key, fields))
    {
        std::cerr << "Missing native Redis object " << key << "\n"; assert(false);
    }
    return {fields.begin(), fields.end()};
}
int main(int argc, char **argv)
{
    assert(argc == 2); SonicDBConfig::initialize(argv[1]);
    DBConnector db{"APPL_DB", 0}; RedisPipeline pipeline{&db};
    Table objects{&db, APP_L2_NEXTHOP_GROUP_TABLE_NAME}, ids{&db, FPM_PROTECTION_ID_TABLE};
    Table pending{&db, "_" + std::string(APP_L2_NEXTHOP_GROUP_TABLE_NAME)};
    ConsumerStateTable consumer{&db, APP_L2_NEXTHOP_GROUP_TABLE_NAME};
    auto drain = [&] {
        std::deque<KeyOpFieldsValuesTuple> rows;
        do { rows.clear(); consumer.pops(rows); } while (!rows.empty());
    };
    objects.set("200", {{"remote_vtep", "10.0.0.1"}});
    objects.set("201", {{"nexthop_group", "200"}});
    const auto kernelMember = read(objects, "200"), kernelGroup = read(objects, "201");
    auto sync = std::make_unique<RouteSync>(&pipeline);
    member(*sync, pipeline, 0x40001001u); group(*sync, pipeline, 0x80001001u, 0x40001001u);
    group(*sync, pipeline, UINT32_MAX, 0x40001002u); // Orphan: Member never arrived.
    group(*sync, pipeline, 0x80001002u); // Empty: no Members to infer type from.
    assert(read(pending, "1073745921").at("type") == "l3");
    group(*sync, pipeline, 0x40001001u); // Wrong-kind SET before consumer POP.
    assert(read(pending, "1073745921").at("type") == "l3");
    drain();
    assert((read(objects, "1073745921") == std::map<std::string,std::string>{
        {"type", "l3"}, {"remote_vtep", "10.0.0.3"}, {"router_mac", "02:00:00:00:00:03"}}));
    assert((read(objects, "2147487745") == std::map<std::string,std::string>{{"nexthop_group", "1073745921"}}));
    for (const auto *id : {"1073745921", "2147487745", "4294967295", "2147487746"})
        assert(hasFpmProtectionId(ids, id));
    // Exact-key collision rejection never registers a kernel key.
    member(*sync, pipeline, 200); group(*sync, pipeline, 201);
    assert(read(objects, "200") == kernelMember && read(objects, "201") == kernelGroup);
    assert(!hasFpmProtectionId(ids, "200") && !hasFpmProtectionId(ids, "201"));
    ProducerStateTable kernelPending{&pipeline, APP_L2_NEXTHOP_GROUP_TABLE_NAME, true};
    kernelPending.set("300", {{"remote_vtep", "10.0.0.8"}});
    kernelPending.set("301", {{"nexthop_group", "300"}}); pipeline.flush();
    member(*sync, pipeline, 300); group(*sync, pipeline, 301);
    assert((read(pending, "300") == std::map<std::string,std::string>{{"remote_vtep", "10.0.0.8"}}));
    assert(!hasFpmProtectionId(ids, "300") && !hasFpmProtectionId(ids, "301"));
    group(*sync, pipeline, 0x80001003u); // Owned pending SET at process crash.
    ids.set("1234", {{"NULL","NULL"}}); // Crash after indexing, before SET.
    sync.reset(); sync = std::make_unique<RouteSync>(&pipeline);
    sync->resetFpmProtectionState(db); pipeline.flush();
    std::vector<FieldValueTuple> fields;
    for (const auto *id : {"1073745921", "2147487745", "4294967295", "2147487746", "2147487747", "1234"})
    {
        assert(!objects.get(id, fields)); assert(!pending.get(id, fields)); assert(!ids.get(id, fields));
    }
    assert(read(objects, "200") == kernelMember && read(objects, "201") == kernelGroup);
    drain(); // Own DELs cannot resurrect SETs; kernel pending data still applies.
    assert((read(objects, "300") == std::map<std::string,std::string>{{"remote_vtep", "10.0.0.8"}}));
    assert((read(objects, "301") == std::map<std::string,std::string>{{"nexthop_group", "300"}}));
    group(*sync, pipeline, 0x80001001u, 0x40001001u); drain(); // Same-ID replay succeeds.
    assert(read(objects, "2147487745").at("nexthop_group") == "1073745921");
    sync->resetFpmProtectionState(db); pipeline.flush();
    std::vector<std::string> keys; ids.getKeys(keys); assert(keys.empty());
    std::cout << "Native Redis HLD rows / accepted-ID restart cleanup PASS\n";
}
