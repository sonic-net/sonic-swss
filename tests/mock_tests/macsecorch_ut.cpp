#include "ut_helper.h"
#include "mock_orchagent_main.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <new>

#define private public
#define protected public
#include "macsecorch.h"
#include "portsorch.h"
#undef protected
#undef private

extern sai_object_id_t gSwitchId;
extern sai_macsec_api_t *sai_macsec_api;
extern sai_acl_api_t    *sai_acl_api;

using namespace swss;

namespace macsecorch_test
{
    // ------------------------------------------------------------------
    // SAI call recorder
    // ------------------------------------------------------------------
    struct SaiSaCall
    {
        sai_object_id_t                 oid;          // assigned for create; ignored for remove
        std::vector<sai_attribute_t>    attrs;        // recorded attribute list (create only)
    };

    static std::vector<SaiSaCall>       g_created_sas;
    static std::vector<sai_object_id_t> g_removed_sas;
    static sai_object_id_t              g_next_sa_oid;
    static bool                         g_remove_should_fail;

    // Recorder for the egress ACL entry toggles MACsecOrch performs via
    // setMACsecFlowActive(). Either ACTION_MACSEC_FLOW enable=false or
    // ACTION_PACKET_ACTION (DROP) enable=true means frames on the port
    // stopped being steered into the MACsec flow.
    struct SaiAclCall
    {
        sai_object_id_t entry_id;
        sai_attribute_t attr;
    };
    static std::vector<SaiAclCall>      g_acl_calls;

    static sai_status_t fake_set_acl_entry_attribute(sai_object_id_t        entry_id,
                                                     const sai_attribute_t *attr)
    {
        g_acl_calls.push_back({ entry_id, *attr });
        return SAI_STATUS_SUCCESS;
    }

    static bool macsec_flow_was_disabled()
    {
        for (const auto &c : g_acl_calls)
        {
            if (c.attr.id == SAI_ACL_ENTRY_ATTR_ACTION_MACSEC_FLOW &&
                !c.attr.value.aclaction.enable)
                return true;
            if (c.attr.id == SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION &&
                c.attr.value.aclaction.enable)
                return true;
        }
        return false;
    }

    static void reset_sai_recorder()
    {
        g_created_sas.clear();
        g_removed_sas.clear();
        g_next_sa_oid        = 0x5c00000000010000ULL;   // mimic real OID prefix range
        g_remove_should_fail = false;
        g_acl_calls.clear();
    }

    static sai_status_t fake_create_macsec_sa(sai_object_id_t        *sa_id,
                                              sai_object_id_t         switch_id,
                                              uint32_t                attr_count,
                                              const sai_attribute_t  *attr_list)
    {
        SaiSaCall call;
        call.oid = ++g_next_sa_oid;
        call.attrs.assign(attr_list, attr_list + attr_count);
        *sa_id = call.oid;
        g_created_sas.push_back(std::move(call));
        return SAI_STATUS_SUCCESS;
    }

    // Attribute sets on an existing SA (next_pn / lowest_acceptable_pn)
    // are recorded and succeed; the vs SAI would reject the fake OIDs.
    static std::vector<sai_attribute_t> g_set_sa_attrs;
    static sai_status_t fake_set_macsec_sa_attribute(sai_object_id_t        sa_id,
                                                     const sai_attribute_t *attr)
    {
        g_set_sa_attrs.push_back(*attr);
        return SAI_STATUS_SUCCESS;
    }

    static sai_status_t fake_remove_macsec_sa(sai_object_id_t sa_id)
    {
        if (g_remove_should_fail)
            return SAI_STATUS_OBJECT_IN_USE;
        g_removed_sas.push_back(sa_id);
        return SAI_STATUS_SUCCESS;
    }

    // ------------------------------------------------------------------
    // Fixture: stand up a minimal MACsecOrch with the SAI MACsec API
    // pointed at our recorders, and pre-populate the per-port state so
    // that a "surviving SA from the prior MKA cycle" is visible to the
    // re-key paths under test.
    // ------------------------------------------------------------------
    class MacsecOrchStaleSakTest : public ::testing::Test
    {
    public:
        static constexpr const char    *kPortName       = "Ethernet0";
        // SCI hex strings as wpa_supplicant's macsec_sonic driver
        // writes them to APPL_DB. Numeric m_sci values are derived
        // via MACsecSCI::operator=(string) so the byte ordering
        // matches whatever the orch actually parses.
        static constexpr const char    *kIngressSciHex = "220eefebbc130001";
        static constexpr const char    *kEgressSciHex  = "e8e49d1111320001";
        static constexpr macsec_an_t    kAN            = 0;

        // The simulated "stale" hardware OID populated into m_sa_ids
        // before each test calls the re-key path.
        static constexpr sai_object_id_t kStaleSaOid =
            0x5c00000000005a5aULL;

    protected:
        void SetUp() override
        {
            // Set up SAI vslib so global gSwitchId resolves.
            std::map<std::string, std::string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "e8:e4:9d:11:13:20"        },
            };
            ASSERT_EQ(ut_helper::initSaiApi(profile), SAI_STATUS_SUCCESS);

            ASSERT_EQ(sai_api_query(SAI_API_MACSEC, (void **)&sai_macsec_api),
                      SAI_STATUS_SUCCESS);

            // Create the SAI switch so gSwitchId is valid.
            sai_attribute_t init = {};
            init.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            init.value.booldata = true;
            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &init),
                      SAI_STATUS_SUCCESS);

            ASSERT_NE(sai_macsec_api, nullptr);
            saved_create_macsec_sa = sai_macsec_api->create_macsec_sa;
            saved_remove_macsec_sa = sai_macsec_api->remove_macsec_sa;
            sai_macsec_api->create_macsec_sa = fake_create_macsec_sa;
            sai_macsec_api->remove_macsec_sa = fake_remove_macsec_sa;
            saved_set_macsec_sa_attribute = sai_macsec_api->set_macsec_sa_attribute;
            sai_macsec_api->set_macsec_sa_attribute = fake_set_macsec_sa_attribute;
            g_set_sa_attrs.clear();

            ASSERT_NE(sai_acl_api, nullptr);
            saved_set_acl_entry_attribute = sai_acl_api->set_acl_entry_attribute;
            sai_acl_api->set_acl_entry_attribute = fake_set_acl_entry_attribute;
            reset_sai_recorder();

            app_db   = std::make_shared<DBConnector>("APPL_DB",  0);
            state_db = std::make_shared<DBConnector>("STATE_DB", 0);

            // PortsOrch with just one port, Ethernet0
            fake_ports_orch_buf = std::malloc(sizeof(PortsOrch));
            ASSERT_NE(fake_ports_orch_buf, nullptr);
            std::memset(fake_ports_orch_buf, 0, sizeof(PortsOrch));
            auto *fake_ports =
                reinterpret_cast<PortsOrch *>(fake_ports_orch_buf);
            ::new (&fake_ports->m_portList)
                std::map<std::string, swss::Port>();
            swss::Port p;
            p.m_alias   = kPortName;
            p.m_port_id = 0x1000000000000abcULL;
            fake_ports->m_portList[kPortName] = p;
            saved_ports_orch = gPortsOrch;
            gPortsOrch = fake_ports;

            std::vector<std::string> tables = {
                APP_MACSEC_PORT_TABLE_NAME,
                APP_MACSEC_EGRESS_SC_TABLE_NAME,
                APP_MACSEC_INGRESS_SC_TABLE_NAME,
                APP_MACSEC_EGRESS_SA_TABLE_NAME,
                APP_MACSEC_INGRESS_SA_TABLE_NAME,
            };
            orch = std::make_shared<MACsecOrch>(app_db.get(),
                                                state_db.get(),
                                                tables,
                                                gPortsOrch);
        }

        void TearDown() override
        {
            orch->m_macsec_ports.clear();
            orch.reset();

            sai_macsec_api->create_macsec_sa = saved_create_macsec_sa;
            sai_macsec_api->remove_macsec_sa = saved_remove_macsec_sa;
            sai_macsec_api->set_macsec_sa_attribute = saved_set_macsec_sa_attribute;
            sai_acl_api->set_acl_entry_attribute = saved_set_acl_entry_attribute;

            if (fake_ports_orch_buf != nullptr)
            {
                auto *fake_ports =
                    reinterpret_cast<PortsOrch *>(fake_ports_orch_buf);
                fake_ports->m_portList.~map();
                std::free(fake_ports_orch_buf);
                fake_ports_orch_buf = nullptr;
            }
            gPortsOrch = saved_ports_orch;

            ASSERT_EQ(sai_switch_api->remove_switch(gSwitchId),
                      SAI_STATUS_SUCCESS);
            gSwitchId = SAI_NULL_OBJECT_ID;

            ASSERT_EQ(ut_helper::uninitSaiApi(), SAI_STATUS_SUCCESS);
        }

        // Construct a TaskArgs vector that looks like a full SA
        // payload from wpa_supplicant's macsec_sonic driver:
        //   sak / auth_key / lowest_acceptable_pn / ssci / salt
        // (egress: next_pn instead of lowest_acceptable_pn)
        // Caller picks the SAK bytes; the rest are arbitrary but
        // self-consistent so the create path inside MACsecOrch
        // succeeds.
        std::vector<FieldValueTuple>
        buildSaFvs(const std::string &sak_hex_32,
                   bool               include_active,
                   bool               active,
                   bool               egress)
        {
            std::vector<FieldValueTuple> fvs;
            if (include_active)
            {
                fvs.emplace_back("active", active ? "true" : "false");
            }
            fvs.emplace_back("sak", sak_hex_32);   // 16-byte AES-128 SAK
            fvs.emplace_back("auth_key", "00112233445566778899AABBCCDDEEFF");
            fvs.emplace_back("ssci", egress ? "2" : "1");
            fvs.emplace_back("salt", "000000000000000000000000");
            fvs.emplace_back(egress ? "next_pn" : "lowest_acceptable_pn", "1");
            return fvs;
        }

        static sai_uint64_t parseSciHex(const std::string &hex)
        {
            sai_uint64_t v = 0;
            uint8_t *bytes = reinterpret_cast<uint8_t *>(&v);
            for (size_t i = 0; i < sizeof(v) && (2 * i + 1) < hex.size(); ++i)
            {
                char buf[3] = { hex[2 * i], hex[2 * i + 1], '\0' };
                bytes[i] = static_cast<uint8_t>(std::strtoul(buf, nullptr, 16));
            }
            return v;
        }

        // Pre-populate m_macsec_ports so that the SC and AN under test
        // already exist with a "stale" SA OID -- the same shape orchagent
        // is in after macsec docker SIGKILL has surfaced the bug.
        void seedSurvivingSa(sai_macsec_direction_t direction,
                             sai_uint64_t           sci)
        {
            auto port = std::make_shared<MACsecOrch::MACsecPort>();
            port->m_cipher_suite = SAI_MACSEC_CIPHER_SUITE_GCM_AES_128;
            port->m_enable       = false;

            auto &scs = (direction == SAI_MACSEC_DIRECTION_EGRESS)
                        ? port->m_egress_scs
                        : port->m_ingress_scs;
            MACsecOrch::MACsecSC sc{};
            sc.m_sc_id        = 0x5b00000000000001ULL;
            sc.m_flow_id      = 0x5a00000000000001ULL;
            sc.m_encoding_an  = kAN;
            sc.m_sa_ids[kAN]  = kStaleSaOid;
            scs[sci] = sc;

            orch->m_macsec_ports[kPortName] = port;
        }

        std::shared_ptr<DBConnector>  app_db;
        std::shared_ptr<DBConnector>  state_db;
        std::shared_ptr<MACsecOrch>   orch;

        sai_status_t (*saved_create_macsec_sa)(sai_object_id_t*,
                                               sai_object_id_t,
                                               uint32_t,
                                               const sai_attribute_t*) = nullptr;
        sai_status_t (*saved_remove_macsec_sa)(sai_object_id_t)        = nullptr;
        sai_status_t (*saved_set_macsec_sa_attribute)(sai_object_id_t,
                                                      const sai_attribute_t*) = nullptr;
        sai_status_t (*saved_set_acl_entry_attribute)(sai_object_id_t,
                                                      const sai_attribute_t*) = nullptr;

        // Raw buffer holding a placement-new'd m_portList (see SetUp).
        void       *fake_ports_orch_buf = nullptr;
        PortsOrch  *saved_ports_orch    = nullptr;
    };

    constexpr const char    *MacsecOrchStaleSakTest::kPortName;
    constexpr const char    *MacsecOrchStaleSakTest::kIngressSciHex;
    constexpr const char    *MacsecOrchStaleSakTest::kEgressSciHex;
    constexpr macsec_an_t    MacsecOrchStaleSakTest::kAN;
    constexpr sai_object_id_t MacsecOrchStaleSakTest::kStaleSaOid;

    // A SAK as the macsec_sonic driver writes it: upper-case hex.
    static const std::string kNewSakHex =
        "C7AF571AC1A17C79DCD8C2F6E5DB1C18";

    // ------------------------------------------------------------------
    // Test 1: SET on an existing ingress SA without key material
    //
    // wpa_supplicant's enable_receive_sa() writes active=true only. On an
    // SA that already exists this is the second stage of a normal install
    // and must leave the SA in place: no SAI remove, no SAI create.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           taskUpdateIngressSA_existing_active_true_no_sak_leaves_sa_in_place)
    {
        const sai_uint64_t sci_num = parseSciHex(kIngressSciHex);
        seedSurvivingSa(SAI_MACSEC_DIRECTION_INGRESS, sci_num);

        // active=true ONLY, no key material -- stage-2 from
        // wpa_supplicant's enable_receive_sa().
        std::vector<FieldValueTuple> fvs = {
            { "active", "true" },
        };

        const std::string key = std::string(kPortName) + ":" +
                                kIngressSciHex + ":" + std::to_string(kAN);
        auto status = orch->taskUpdateIngressSA(key, fvs);
        EXPECT_EQ(status, task_success);

        // No SAI calls at all -- the surviving SA stays untouched.
        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);

        // And the orch's in-memory state still points at the stale OID.
        auto &port = orch->m_macsec_ports[kPortName];
        EXPECT_EQ(port->m_ingress_scs.at(sci_num).m_sa_ids.at(kAN),
                  kStaleSaOid);
    }

    // ------------------------------------------------------------------
    // Test 2: SET with key material on an existing ingress SA
    //
    // A SET carrying a SAK for an SA orchagent already holds is not a
    // re-key: a live SA is never destroyed on the strength of a payload
    // shape. SAI_MACSEC_SA_ATTR_SAK is create-only, so the SET is a no-op
    // in SAI. A supplicant that died leaves such entries behind, and
    // macsecmgrd clears them before the next session can write.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           createMACsecSA_existing_sa_with_sak_leaves_sa_in_place)
    {
        const sai_uint64_t sci_num = parseSciHex(kIngressSciHex);
        seedSurvivingSa(SAI_MACSEC_DIRECTION_INGRESS, sci_num);

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ true,
                              /*active*/         true,
                              /*egress*/         false);

        const std::string key = std::string(kPortName) + ":" +
                                kIngressSciHex + ":" + std::to_string(kAN);
        EXPECT_EQ(orch->taskUpdateIngressSA(key, fvs), task_success);

        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        auto &port = orch->m_macsec_ports[kPortName];
        EXPECT_EQ(port->m_ingress_scs.at(sci_num).m_sa_ids.at(kAN), kStaleSaOid);
    }

    // ------------------------------------------------------------------
    // Test 3: SET with key material on an existing egress SA
    //
    // Same rule on the egress side: only next_pn is applied, the SA and
    // its key stay as they are.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           taskUpdateEgressSA_existing_sa_with_sak_leaves_sa_in_place)
    {
        const sai_uint64_t sci_num = parseSciHex(kEgressSciHex);
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, sci_num);

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ false,
                              /*active*/         false,
                              /*egress*/         true);

        const std::string key = std::string(kPortName) + ":" +
                                kEgressSciHex + ":" + std::to_string(kAN);
        EXPECT_EQ(orch->taskUpdateEgressSA(key, fvs), task_success);

        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        auto &port = orch->m_macsec_ports[kPortName];
        EXPECT_EQ(port->m_egress_scs.at(sci_num).m_sa_ids.at(kAN), kStaleSaOid);
        ASSERT_EQ(g_set_sa_attrs.size(), 1u);
        EXPECT_EQ(g_set_sa_attrs[0].id, SAI_MACSEC_SA_ATTR_CONFIGURED_EGRESS_XPN);
        EXPECT_EQ(g_set_sa_attrs[0].value.u64, 1u);
    }

    // ------------------------------------------------------------------
    // Test 4: setEncodingAN is a no-op when encoding_an is unchanged
    //
    // A SET on MACSEC_EGRESS_SC_TABLE with the same encoding_an value
    // must not touch SAI at all (clean rekey fast-path guard).
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           setEncodingAN_noop_when_encoding_an_unchanged)
    {
        static constexpr sai_object_id_t kOid0 = 0x5c00000000002000ULL;

        MACsecOrch::MACsecSC sc{};
        sc.m_encoding_an = 0;
        sc.m_sa_ids[0]   = kOid0;

        MACsecOrch::TaskArgs attrs = { { "encoding_an", "0" } };

        bool result = orch->setEncodingAN(sc, attrs, SAI_MACSEC_DIRECTION_EGRESS);
        EXPECT_TRUE(result);

        // No SAI remove or create.
        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(0));
    }

    // ------------------------------------------------------------------
    // Test 5: setEncodingAN ingress direction is a no-op
    //
    // Ingress SCs don't carry encoding_an. The function refuses the call
    // (returns false) without touching SAI or sc.m_encoding_an; the caller
    // only invokes it for egress SCs.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           setEncodingAN_ingress_is_noop)
    {
        MACsecOrch::MACsecSC sc{};
        sc.m_encoding_an = 0;

        MACsecOrch::TaskArgs attrs = { { "encoding_an", "1" } };

        bool result = orch->setEncodingAN(sc, attrs, SAI_MACSEC_DIRECTION_INGRESS);
        EXPECT_FALSE(result);

        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(0));
    }

    // ------------------------------------------------------------------
    // Test 6: a normal MKA rekey must never leave the egress SC without
    // an SA (sonic-net/sonic-swss#4934).
    //
    // Real ordering, as driven by wpa_supplicant's macsec_sonic driver:
    //   1. create_transmit_sa(AN=1) at SAK distribution. Orchagent holds
    //      the SET in retry because encoding_an is still 0.
    //   2. enable_transmit_sa(AN=1) writes encoding_an=1 on the SC. At
    //      this instant AN=0 is the ONLY SA on the SC and is what the
    //      hardware encodes with.
    //   3. The deferred AN=1 SET is retried and the SA is created.
    //   4. ~3 s later wpa's CP RETIRE state deletes AN=0.
    //
    // Deleting the last SA of an SC flips the egress ACL entry from the
    // MACsec flow to PACKET_ACTION DROP (deleteMACsecSA ->
    // setMACsecFlowActive(false)), so the SC must not be emptied at
    // step 2. Nothing but wpa's own DEL at step 4 may remove AN=0.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           normal_rekey_never_empties_egress_sc_or_disables_flow)
    {
        static constexpr sai_object_id_t kAclEntryOid = 0x5900000000000001ULL;

        const sai_uint64_t sci = parseSciHex(kEgressSciHex);
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, sci);   // AN=0, encoding_an=0
        auto port = orch->m_macsec_ports[kPortName];
        port->m_enable = true;
        auto &sc = port->m_egress_scs[sci];
        sc.m_entry_id = kAclEntryOid;

        const std::string port_sci   = std::string(kPortName) + ":" + kEgressSciHex;
        const std::string port_sci_0 = port_sci + ":0";
        const std::string port_sci_1 = port_sci + ":1";
        const auto sa1_fvs = buildSaFvs(kNewSakHex, false, false, true);

        // Step 1: the new SA is deferred until the encoding AN moves.
        EXPECT_EQ(orch->taskUpdateEgressSA(port_sci_1, sa1_fvs), task_need_retry);
        EXPECT_EQ(g_created_sas.size(), 0u);

        // Step 2: encoding AN switches while AN=0 is the only SA.
        MACsecOrch::TaskArgs sc_attrs = { { "encoding_an", "1" } };
        EXPECT_TRUE(orch->setEncodingAN(sc, sc_attrs,
                                        SAI_MACSEC_DIRECTION_EGRESS));
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(1));
        EXPECT_EQ(g_removed_sas.size(), 0u)
            << "encoding SA removed before its replacement exists";
        EXPECT_EQ(sc.m_sa_ids.count(0), 1u) << "egress SC left with no SA";
        EXPECT_FALSE(macsec_flow_was_disabled())
            << "egress ACL entry flipped to DROP during rekey";

        // Step 3: the deferred SET lands; both SAs coexist.
        EXPECT_EQ(orch->taskUpdateEgressSA(port_sci_1, sa1_fvs), task_success);
        ASSERT_EQ(g_created_sas.size(), 1u);
        EXPECT_EQ(sc.m_sa_ids.size(), 2u);
        EXPECT_FALSE(macsec_flow_was_disabled());

        // Step 4: wpa retires the old key.
        EXPECT_EQ(orch->taskDeleteEgressSA(port_sci_0, {}), task_success);
        ASSERT_EQ(g_removed_sas.size(), 1u);
        EXPECT_EQ(g_removed_sas[0], kStaleSaOid);
        EXPECT_EQ(sc.m_sa_ids.size(), 1u);
        EXPECT_EQ(sc.m_sa_ids.count(1), 1u);
        EXPECT_FALSE(macsec_flow_was_disabled());
    }

    // ------------------------------------------------------------------
    // Test 7: setEncodingAN never removes an SA
    //
    // Whatever is installed on the SC when the encoding AN moves stays
    // installed. The previous AN is wpa_supplicant's to retire (CP RETIRE,
    // 3 s after the switch), and an SA leaked by a supplicant that died is
    // macsecmgrd's to clear before the next session starts. orchagent only
    // applies what APPL_DB tells it.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest, setEncodingAN_never_removes_sas)
    {
        static constexpr sai_object_id_t kCurrentOidAN1 = 0x5c00000000001001ULL;

        const sai_uint64_t sci = parseSciHex(kEgressSciHex);
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, sci);   // AN=0 = kStaleSaOid
        auto port = orch->m_macsec_ports[kPortName];
        port->m_enable = true;
        auto &sc = port->m_egress_scs[sci];
        sc.m_sa_ids[1]   = kCurrentOidAN1;
        sc.m_encoding_an = 1;

        MACsecOrch::TaskArgs attrs = { { "encoding_an", "2" } };
        EXPECT_TRUE(orch->setEncodingAN(sc, attrs, SAI_MACSEC_DIRECTION_EGRESS));

        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(2));
        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        ASSERT_EQ(sc.m_sa_ids.size(), 2u);
        EXPECT_EQ(sc.m_sa_ids.at(0), kStaleSaOid);
        EXPECT_EQ(sc.m_sa_ids.at(1), kCurrentOidAN1);
        EXPECT_FALSE(macsec_flow_was_disabled());
    }
}
