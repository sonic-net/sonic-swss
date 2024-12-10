#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#define private public
#include "dashpavalidationorch.h"
#include "dashvnetorch.h"
#undef private

#include "dash_api/pa_validation.pb.h"

#include <vector>
#include <algorithm>

using namespace swss;
using namespace std;

namespace dash_test {
    DEFINE_SAI_API_MOCK_DASH(pa_validation);
    DEFINE_SAI_GENERIC_API_MOCK(dash_vnet, vnet);
    DEFINE_SAI_API_MOCK_DASH(outbound_ca_to_pa);

    struct PaValidaionTestEntry
    {
        string vni;
        IpAddress address;

        inline bool operator==(const PaValidaionTestEntry &o) const
        {
            return vni == o.vni && address == o.address;
        }
    };

    using PaValidaionDb = vector<PaValidaionTestEntry>;

    struct PaValidaionConfig
    {
        string vni;
        vector<IpAddress> addresses;
    };

    struct DashPaValidationTest : public mock_orch_test::MockOrchTest
    {
        PaValidaionDb db;
        unique_ptr<DashVnetOrch> vnetorch;
        const vector<uint32_t> vnis = { 100, 200, 300 };

        bool addEntry(const PaValidaionTestEntry& entry)
        {
            auto exists = checkExists(entry);
            if (exists) {
                return false;
            }

            db.push_back(entry);
            return true;
        }

        bool removeEntry(const PaValidaionTestEntry& entry)
        {
            auto exists = find(begin(db), end(db), entry);
            if (exists == end(db)) {
                return false;
            }

            db.erase(exists);
            return true;
        }

        bool checkExists(const PaValidaionTestEntry& entry)
        {
            return find(begin(db), end(db), entry) != db.end();
        }

        PaValidaionTestEntry fromSai(const sai_pa_validation_entry_t& entry)
        {
            ip_addr_t addr;
            if (entry.sip.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
            {
                addr.family = AF_INET;
                addr.ip_addr.ipv4_addr = entry.sip.addr.ip4;
            } else {
                addr.family = AF_INET6;
                memcpy(addr.ip_addr.ipv6_addr, entry.sip.addr.ip6, sizeof(entry.sip.addr.ip6));
            }

            return PaValidaionTestEntry{to_string(entry.vnet_id), IpAddress(addr)};
        }

        void initVnetOrch()
        {
            vnetorch = make_unique<DashVnetOrch>(m_app_db.get(), vector<string>(), nullptr, nullptr);
            for (auto vni : vnis)
            {
                vnetorch->vni_vnet_oid_table_[vni] = vni;
            }
        }

        sai_status_t handleBulkCreate(const sai_pa_validation_entry_t *entries, uint32_t count, sai_status_t *object_statuses)
        {
            sai_status_t status = SAI_STATUS_SUCCESS;

            for (uint32_t i = 0; i < count; i++)
            {
                bool unique = addEntry(fromSai(entries[i]));
                object_statuses[i] = unique ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE;

                if (!unique)
                {
                    status = SAI_STATUS_FAILURE;
                }
            }

            return status;
        }

        sai_status_t handleBulkRemove(const sai_pa_validation_entry_t *entries, uint32_t count, sai_status_t *object_statuses)
        {
            sai_status_t status = SAI_STATUS_SUCCESS;

            for (uint32_t i = 0; i < count; i++)
            {
                bool removed = removeEntry(fromSai(entries[i]));
                object_statuses[i] = removed ? SAI_STATUS_SUCCESS : SAI_STATUS_FAILURE;

                if (!removed)
                {
                    status = SAI_STATUS_FAILURE;
                }
            }

            return status;
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(dash_pa_validation);
            MockSaiApis();

            db.clear();
            initVnetOrch();

            EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).WillRepeatedly(testing::Invoke(
                [this](uint32_t object_count, const sai_pa_validation_entry_t *pa_validation_entry, const uint32_t *attr_count,
                       const sai_attribute_t **attr_list, sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses) {
                    return handleBulkCreate(pa_validation_entry, object_count, object_statuses);
            }));

            EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries).WillRepeatedly(testing::Invoke(
                [this](uint32_t object_count, const sai_pa_validation_entry_t *pa_validation_entry,
                       sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses) {
                    return handleBulkRemove(pa_validation_entry, object_count, object_statuses);
            }));
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
        }

        dash::pa_validation::PaValidation toPb(const PaValidaionConfig& config)
        {
            dash::pa_validation::PaValidation pb;
            for (const auto& addr : config.addresses)
            {
                dash::types::IpAddress pbaddr;
                if (addr.isV4())
                {
                    pbaddr.set_ipv4(addr.getV4Addr());
                } else {
                    pbaddr.set_ipv6(string((const char*)addr.getV6Addr(), 16));
                }
                *pb.add_addresses() = pbaddr;
            }

            return pb;
        }

        unique_ptr<Consumer> makeConsumerWithConfig(const std::vector<PaValidaionConfig>& toCreate, const std::vector<PaValidaionConfig>& toRemove)
        {
            deque<KeyOpFieldsValuesTuple> tasks;

            for (const auto& entry : toCreate)
            {
                auto pb = toPb(entry);
                tasks.push_back({ entry.vni, SET_COMMAND, { {"pb", pb.SerializeAsString()} }});
            }

            for (const auto& entry : toRemove)
            {
                tasks.push_back({ entry.vni, DEL_COMMAND, { }});
            }

            auto consumer = unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(m_app_db.get(), APP_DASH_PA_VALIDATION_TABLE_NAME, 1, 1),
                                        nullptr, APP_DASH_PA_VALIDATION_TABLE_NAME));
            consumer->addToSync(tasks);

            return consumer;
        }

        void doConfig(DashPaValidationOrch &pa, const std::vector<PaValidaionConfig>& toCreate, const std::vector<PaValidaionConfig>& toRemove)
        {
            auto consumer = makeConsumerWithConfig(toCreate, toRemove);

            pa.doTask(*consumer);
        };

        void validateDb(const vector<PaValidaionTestEntry> &expected)
        {
            for (const auto& entry : expected)
            {
                ASSERT_TRUE(checkExists(entry)) << "Entry not found in SAI db";
            }

            ASSERT_EQ(expected.size(), db.size()) << "DB size is not expected";
        }

        uint64_t getCrmCounterIpV4() const
        {
            auto const &resourceMap = Portal::CrmOrchInternal::getResourceMap(gCrmOrch);
            return resourceMap.at(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION).countersMap.at("STATS").usedCounter;
        }

        uint64_t getCrmCounterIpV6() const
        {
            auto const &resourceMap = Portal::CrmOrchInternal::getResourceMap(gCrmOrch);
            return resourceMap.at(CrmResourceType::CRM_DASH_IPV6_PA_VALIDATION).countersMap.at("STATS").usedCounter;
        }
    };

    TEST_F(DashPaValidationTest, CreateRemove)
    {
        DashPaValidationOrch pa(m_app_db.get(), nullptr);
        pa.setVnetOrch(vnetorch.get());

        auto config = PaValidaionConfig {"100", {IpAddress("1.1.1.1"), IpAddress("1.1.1.2")}};
        doConfig(pa, { config }, {});

        validateDb({
            PaValidaionTestEntry { "100", IpAddress("1.1.1.1") },
            PaValidaionTestEntry { "100", IpAddress("1.1.1.2") }
        });

        config = PaValidaionConfig {"100", {}};

        doConfig(pa, { }, { config });
        validateDb({});
    }

    TEST_F(DashPaValidationTest, CreateAppend)
    {
        DashPaValidationOrch pa(m_app_db.get(), nullptr);
        pa.setVnetOrch(vnetorch.get());

        auto config = PaValidaionConfig {"100", {IpAddress("1.1.1.1"), IpAddress("1.1.1.2")}};
        doConfig(pa, { config }, {});

        config = PaValidaionConfig {"100", {IpAddress("1.1.1.3"), IpAddress("1.1.1.2")}};
        doConfig(pa, { config }, {});

        validateDb({
            PaValidaionTestEntry { "100", IpAddress("1.1.1.1") },
            PaValidaionTestEntry { "100", IpAddress("1.1.1.2") },
            PaValidaionTestEntry { "100", IpAddress("1.1.1.3") }
        });

        config = PaValidaionConfig {"100", {}};
        doConfig(pa, { }, { config });

        validateDb({
            PaValidaionTestEntry { "100", IpAddress("1.1.1.2") } // this entry is created twice
        });

        config = PaValidaionConfig {"100", {}};
        doConfig(pa, { }, { config });
        validateDb({});
    }

    TEST_F(DashPaValidationTest, CreateFromVnetOrch)
    {
        DashPaValidationOrch pa(m_app_db.get(), nullptr);
        pa.setVnetOrch(vnetorch.get());

        // Create from ZMQ
        auto config = PaValidaionConfig {"100", {IpAddress("1.1.1.1"), IpAddress("1.1.1.2")}};
        doConfig(pa, { config }, {});

        validateDb({
            PaValidaionTestEntry { "100", IpAddress("1.1.1.1") },
            PaValidaionTestEntry { "100", IpAddress("1.1.1.2") }
        });

        // Create the same entry from VnetOrch via PA validation Orch API
        auto vnet_orch_entries = PaValidationEntryList {
            PaValidationEntry {
                vnetorch->vni_vnet_oid_table_[100],
                100,
                IpAddress("1.1.1.1")
            }
        };
        pa.addPaValidationEntries(vnet_orch_entries);

        validateDb({
            PaValidaionTestEntry { "100", IpAddress("1.1.1.1") },
            PaValidaionTestEntry { "100", IpAddress("1.1.1.2") }
        });

        // Remove all the entries for 100, the one created by VnetOrch should still be active
        config = PaValidaionConfig {"100", {}};
        doConfig(pa, { }, { config });

        validateDb({
            PaValidaionTestEntry { "100", IpAddress("1.1.1.1") }
        });

        pa.removePaValidationEntries(vnet_orch_entries);
        validateDb({});
    }

    TEST_F(DashPaValidationTest, CRM)
    {
        DashPaValidationOrch pa(m_app_db.get(), nullptr);
        pa.setVnetOrch(vnetorch.get());

        auto config = PaValidaionConfig {"100", {IpAddress("1.1.1.1"), IpAddress("::1")}};
        doConfig(pa, { config }, {});

        auto crm_v4 = getCrmCounterIpV4();
        auto crm_v6 = getCrmCounterIpV6();
        ASSERT_EQ(crm_v4, 1);
        ASSERT_EQ(crm_v6, 1);

        // Apply the same entry again, the CRM should not change
        doConfig(pa, { config }, {});

        crm_v4 = getCrmCounterIpV4();
        crm_v6 = getCrmCounterIpV6();
        ASSERT_EQ(crm_v4, 1);
        ASSERT_EQ(crm_v6, 1);

        // add new addresses
        config = PaValidaionConfig {"100", {IpAddress("1.1.1.2"), IpAddress("::2")}};
        doConfig(pa, { config }, {});

        crm_v4 = getCrmCounterIpV4();
        crm_v6 = getCrmCounterIpV6();
        ASSERT_EQ(crm_v4, 2);
        ASSERT_EQ(crm_v6, 2);

        config = PaValidaionConfig {"100", {}};
        doConfig(pa, { }, { config });

        // there are still entries that are created twice
        crm_v4 = getCrmCounterIpV4();
        crm_v6 = getCrmCounterIpV6();
        ASSERT_EQ(crm_v4, 1);
        ASSERT_EQ(crm_v6, 1);

        config = PaValidaionConfig {"100", {}};
        doConfig(pa, { }, { config });

        // all removed now
        crm_v4 = getCrmCounterIpV4();
        crm_v6 = getCrmCounterIpV6();
        ASSERT_EQ(crm_v4, 0);
        ASSERT_EQ(crm_v6, 0);

        validateDb({});
    }
}
