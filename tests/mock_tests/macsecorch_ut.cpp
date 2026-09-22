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

    static void reset_sai_recorder()
    {
        g_created_sas.clear();
        g_removed_sas.clear();
        g_next_sa_oid        = 0x5c00000000010000ULL;   // mimic real OID prefix range
        g_remove_should_fail = false;
    }

    // Helper: pull the SAK bytes out of a recorded SAI attribute list.
    // Returns true if found, fills `out`.
    static bool extract_sai_sak(const std::vector<sai_attribute_t> &attrs,
                                sai_macsec_sak_t                   &out)
    {
        for (const auto &a : attrs)
        {
            if (a.id == SAI_MACSEC_SA_ATTR_SAK)
            {
                memcpy(out, a.value.macsecsak, sizeof(sai_macsec_sak_t));
                return true;
            }
        }
        return false;
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

        // Raw buffer holding a placement-new'd m_portList (see SetUp).
        void       *fake_ports_orch_buf = nullptr;
        PortsOrch  *saved_ports_orch    = nullptr;
    };

    constexpr const char    *MacsecOrchStaleSakTest::kPortName;
    constexpr const char    *MacsecOrchStaleSakTest::kIngressSciHex;
    constexpr const char    *MacsecOrchStaleSakTest::kEgressSciHex;
    constexpr macsec_an_t    MacsecOrchStaleSakTest::kAN;
    constexpr sai_object_id_t MacsecOrchStaleSakTest::kStaleSaOid;

    // Convenience: the macsec_sonic driver writes SAKs as upper-case
    // hex strings; pick two distinguishable values for "old" vs "new".
    static const std::string kOldSakHex =
        "B9A68A8F3B02F02BC0DFEFD738BBECB1";   // pre-restart
    static const std::string kNewSakHex =
        "C7AF571AC1A17C79DCD8C2F6E5DB1C18";   // post-restart MKA re-key

    // ------------------------------------------------------------------
    // Test 1: taskUpdateIngressSA(active=false + SAK)
    //
    // A surviving ingress SA exists in orchagent state.
    // The post-restart wpa_supplicant fires its stage-1 SET
    // (active=false + full key material). The fix must delete the
    // surviving SA AND recreate from THIS sa_attr, not wait for stage-2.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           taskUpdateIngressSA_active_false_with_sak_deletes_stale_and_recreates)
    {
        seedSurvivingSa(SAI_MACSEC_DIRECTION_INGRESS, parseSciHex(kIngressSciHex));

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ true,
                              /*active*/         false,
                              /*egress*/         false);

        const std::string key = std::string(kPortName) + ":" +
                                kIngressSciHex + ":" + std::to_string(kAN);
        auto status = orch->taskUpdateIngressSA(key, fvs);
        EXPECT_EQ(status, task_success);

        // Old SA must have been removed.
        ASSERT_EQ(g_removed_sas.size(), 1u);
        EXPECT_EQ(g_removed_sas[0], kStaleSaOid);

        // New SA must have been created and carry the NEW SAK.
        ASSERT_EQ(g_created_sas.size(), 1u);
        sai_macsec_sak_t saw{};
        ASSERT_TRUE(extract_sai_sak(g_created_sas[0].attrs, saw));
        // AES-128 SAK occupies the lower 16 bytes; upper 16 should be 0.
        // Re-stringify to compare against kNewSakHex.
        char buf[33];
        for (int i = 0; i < 16; ++i)
            snprintf(buf + 2 * i, 3, "%02X", saw[16 + i]);
        buf[32] = '\0';
        EXPECT_STREQ(buf, kNewSakHex.c_str());
    }

    // ------------------------------------------------------------------
    // Test 2: createMACsecSA early-exit re-key
    //
    // A SET arrives with active=true and a fresh SAK on an SA that
    // already exists. The legacy code returned task_success without 
    // touching SAI; the fix delete+recreates.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           createMACsecSA_existing_with_sak_deletes_stale_and_recreates)
    {
        seedSurvivingSa(SAI_MACSEC_DIRECTION_INGRESS, parseSciHex(kIngressSciHex));

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ true,
                              /*active*/         true,
                              /*egress*/         false);

        const std::string key = std::string(kPortName) + ":" +
                                kIngressSciHex + ":" + std::to_string(kAN);
        // taskUpdateIngressSA's active=true branch funnels through
        // createMACsecSA; this exercises the createMACsecSA early-exit
        // fix.
        auto status = orch->taskUpdateIngressSA(key, fvs);
        EXPECT_EQ(status, task_success);

        ASSERT_EQ(g_removed_sas.size(), 1u);
        EXPECT_EQ(g_removed_sas[0], kStaleSaOid);

        ASSERT_EQ(g_created_sas.size(), 1u);
        sai_macsec_sak_t saw{};
        ASSERT_TRUE(extract_sai_sak(g_created_sas[0].attrs, saw));
        char buf[33];
        for (int i = 0; i < 16; ++i)
            snprintf(buf + 2 * i, 3, "%02X", saw[16 + i]);
        buf[32] = '\0';
        EXPECT_STREQ(buf, kNewSakHex.c_str());
    }

    // ------------------------------------------------------------------
    // Test 3: taskUpdateEgressSA SA exists + SAK in fvs
    //
    // Egress flow: wpa_supplicant calls create_transmit_sa which
    // writes MACSEC_EGRESS_SA_TABLE with sak/salt/ssci/auth_key/next_pn
    // (no `active` field). On a surviving SA the legacy code only
    // updated next_pn and silently dropped the new SAK; the fix
    // delete+recreates.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           taskUpdateEgressSA_existing_with_sak_deletes_stale_and_recreates)
    {
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, parseSciHex(kEgressSciHex));

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ false,
                              /*active*/         false,
                              /*egress*/         true);

        const std::string key = std::string(kPortName) + ":" +
                                kEgressSciHex + ":" + std::to_string(kAN);
        auto status = orch->taskUpdateEgressSA(key, fvs);
        EXPECT_EQ(status, task_success);

        ASSERT_EQ(g_removed_sas.size(), 1u);
        EXPECT_EQ(g_removed_sas[0], kStaleSaOid);

        ASSERT_EQ(g_created_sas.size(), 1u);
        sai_macsec_sak_t saw{};
        ASSERT_TRUE(extract_sai_sak(g_created_sas[0].attrs, saw));
        char buf[33];
        for (int i = 0; i < 16; ++i)
            snprintf(buf + 2 * i, 3, "%02X", saw[16 + i]);
        buf[32] = '\0';
        EXPECT_STREQ(buf, kNewSakHex.c_str());
    }

    // ------------------------------------------------------------------
    // Test 4: legacy fast-path -- SET on existing SA without SAK
    //
    // Counterpart to the three re-key tests: when wpa_supplicant
    // sends stage-2 (active=true with no SAK) on an SA that already
    // exists, the SA must be LEFT IN PLACE -- no remove, no create.
    // This guards against the fix over-firing on the no-SAK path.
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
    // Test 5: taskUpdateIngressSA delete-failure propagation
    //
    // When SAI remove_macsec_sa fails during the ingress re-key path,
    // the error must be returned to the caller (not silently swallowed)
    // and no create must follow.  Covers the `del_status != task_success`
    // branch added in taskUpdateIngressSA.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           taskUpdateIngressSA_delete_failure_propagates_error)
    {
        seedSurvivingSa(SAI_MACSEC_DIRECTION_INGRESS, parseSciHex(kIngressSciHex));

        g_remove_should_fail = true;

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ true,
                              /*active*/         false,
                              /*egress*/         false);

        const std::string key = std::string(kPortName) + ":" +
                                kIngressSciHex + ":" + std::to_string(kAN);
        auto status = orch->taskUpdateIngressSA(key, fvs);
        EXPECT_NE(status, task_success);

        // No SA must have been created (the re-key aborted after the
        // failed delete).
        EXPECT_EQ(g_created_sas.size(), 0u);
    }

    // ------------------------------------------------------------------
    // Test 6: taskUpdateEgressSA delete-failure propagation
    //
    // Same invariant as Test 5 but for the egress re-key path.
    // Covers the `del_status != task_success` branch in
    // taskUpdateEgressSA.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           taskUpdateEgressSA_delete_failure_propagates_error)
    {
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, parseSciHex(kEgressSciHex));

        g_remove_should_fail = true;

        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ false,
                              /*active*/         false,
                              /*egress*/         true);

        const std::string key = std::string(kPortName) + ":" +
                                kEgressSciHex + ":" + std::to_string(kAN);
        auto status = orch->taskUpdateEgressSA(key, fvs);
        EXPECT_NE(status, task_success);

        EXPECT_EQ(g_created_sas.size(), 0u);
    }

    // ------------------------------------------------------------------
    // Test 7: createMACsecSA delete-failure propagation
    //
    // When the defense-in-depth path in createMACsecSA detects an
    // existing SA with a new SAK but the preceding delete fails, the
    // error must be returned and no recursive create must follow.
    // Covers the `del_status != task_success` branch in createMACsecSA.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           createMACsecSA_delete_failure_propagates_error)
    {
        seedSurvivingSa(SAI_MACSEC_DIRECTION_INGRESS, parseSciHex(kIngressSciHex));

        g_remove_should_fail = true;

        // active=true funnels through createMACsecSA, exercising the
        // early-exit re-key block (ctx.get_macsec_sa() != nullptr &&
        // SAK present).
        auto fvs = buildSaFvs(kNewSakHex,
                              /*include_active*/ true,
                              /*active*/         true,
                              /*egress*/         false);

        const std::string key = std::string(kPortName) + ":" +
                                kIngressSciHex + ":" + std::to_string(kAN);
        auto status = orch->taskUpdateIngressSA(key, fvs);
        EXPECT_NE(status, task_success);

        EXPECT_EQ(g_created_sas.size(), 0u);
    }

    // ------------------------------------------------------------------
    // Test 8: setEncodingAN sweeps stale SA when encoding_an changes
    //
    // Covers the dirty-restart AN-boundary case (NOS-7806, SC side):
    // an egress SC has two SAs installed (AN=0 from the prior MKA
    // cycle that survived the SIGKILL, AN=1 just negotiated by the
    // fresh wpa).  When taskUpdateEgressSC writes encoding_an=1,
    // setEncodingAN must remove the AN=0 SA from SAI and leave AN=1
    // intact.
    //
    // The SC is seeded into m_macsec_ports so that the full cleanup
    // path (deleteMACsecSA(port_sci_an, direction)) can look it up.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           setEncodingAN_sweeps_stale_sa_on_encoding_an_change)
    {
        static constexpr sai_object_id_t kCurrentOidAN1 = 0x5c00000000001001ULL;

        // Seed the SC with AN=0 (kStaleSaOid) into m_macsec_ports.
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, parseSciHex(kEgressSciHex));

        // Add AN=1 directly to the seeded SC.
        auto &sc = orch->m_macsec_ports[kPortName]
                       ->m_egress_scs[parseSciHex(kEgressSciHex)];
        sc.m_sa_ids[1] = kCurrentOidAN1;

        const std::string port_sci =
            std::string(kPortName) + ":" + kEgressSciHex;

        // port_sci_an for AN=0 — used to pre-seed and verify cleanup of
        // COUNTERS_DB (uses ':' separator) and STATE_DB (uses '|' separator).
        const std::string port_sci_an_0 =
            std::string(kPortName) + ":" + kEgressSciHex + ":" + std::to_string(kAN);
        const std::string state_key_0 =
            std::string(kPortName) + "|" + kEgressSciHex + "|" + std::to_string(kAN);

        // Plant fake COUNTERS_DB and STATE_DB entries for AN=0, simulating
        // what installCounter / createMACsecSA would have written.
        orch->m_macsec_counters_map.hset("", port_sci_an_0,
                                         sai_serialize_object_id(kStaleSaOid));
        orch->m_state_macsec_egress_sa.hset(state_key_0, "state", "ok");

        MACsecOrch::TaskArgs attrs = { { "encoding_an", "1" } };

        bool result = orch->setEncodingAN(sc, attrs,
                                          SAI_MACSEC_DIRECTION_EGRESS,
                                          port_sci);
        EXPECT_TRUE(result);

        // AN=0 stale SA must have been removed from SAI.
        ASSERT_EQ(g_removed_sas.size(), 1u);
        EXPECT_EQ(g_removed_sas[0], kStaleSaOid);

        // AN=1 SA must remain in sc.m_sa_ids; no new SA created.
        ASSERT_EQ(sc.m_sa_ids.size(), 1u);
        EXPECT_EQ(sc.m_sa_ids.at(1), kCurrentOidAN1);
        EXPECT_EQ(g_created_sas.size(), 0u);

        // encoding_an updated to reflect the new session.
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(1));

        // COUNTERS_DB name->OID mapping for AN=0 must be gone.
        std::string counters_val;
        EXPECT_FALSE(orch->m_macsec_counters_map.hget("", port_sci_an_0,
                                                       counters_val));

        // STATE_DB entry for AN=0 must be gone.
        std::string state_val;
        EXPECT_FALSE(orch->m_state_macsec_egress_sa.hget(state_key_0, "state",
                                                          state_val));
    }

    // ------------------------------------------------------------------
    // Test 9: setEncodingAN is a no-op when encoding_an is unchanged
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

        // port_sci is not consulted because encoding_an is unchanged.
        bool result = orch->setEncodingAN(sc, attrs, SAI_MACSEC_DIRECTION_EGRESS,
                                          std::string(kPortName) + ":" + kEgressSciHex);
        EXPECT_TRUE(result);

        // No SAI remove or create.
        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(0));
    }

    // ------------------------------------------------------------------
    // Test 10: setEncodingAN delete failure is best-effort
    //
    // If SAI remove_macsec_sa fails while sweeping a stale AN, the
    // function must still return true and erase the entry from
    // sc.m_sa_ids.  The failure is logged but not fatal: the SC-level
    // encoding_an update is more important than a leaked OID.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           setEncodingAN_delete_failure_is_best_effort)
    {
        static constexpr sai_object_id_t kCurrentOidAN1 = 0x5c00000000003001ULL;

        // Seed the SC with AN=0 (kStaleSaOid) into m_macsec_ports.
        seedSurvivingSa(SAI_MACSEC_DIRECTION_EGRESS, parseSciHex(kEgressSciHex));

        auto &sc = orch->m_macsec_ports[kPortName]
                       ->m_egress_scs[parseSciHex(kEgressSciHex)];
        sc.m_sa_ids[1] = kCurrentOidAN1;

        g_remove_should_fail = true;

        const std::string port_sci =
            std::string(kPortName) + ":" + kEgressSciHex;
        MACsecOrch::TaskArgs attrs = { { "encoding_an", "1" } };

        bool result = orch->setEncodingAN(sc, attrs,
                                          SAI_MACSEC_DIRECTION_EGRESS,
                                          port_sci);

        // Function must succeed despite the SAI failure.
        EXPECT_TRUE(result);

        // The stale AN=0 entry must be evicted from sc.m_sa_ids even
        // though the SAI remove failed (best-effort sweep).
        ASSERT_EQ(sc.m_sa_ids.size(), 1u);
        EXPECT_EQ(sc.m_sa_ids.at(1), kCurrentOidAN1);

        // encoding_an updated.
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(1));
    }

    // ------------------------------------------------------------------
    // Test 11: setEncodingAN ingress direction is a no-op
    //
    // Ingress SCs don't carry encoding_an. The function must return
    // true immediately without touching SAI or sc.m_encoding_an.
    // ------------------------------------------------------------------
    TEST_F(MacsecOrchStaleSakTest,
           setEncodingAN_ingress_is_noop)
    {
        MACsecOrch::MACsecSC sc{};
        sc.m_encoding_an = 0;

        MACsecOrch::TaskArgs attrs = { { "encoding_an", "1" } };

        // port_sci is not consulted for ingress (returns false immediately).
        bool result = orch->setEncodingAN(sc, attrs, SAI_MACSEC_DIRECTION_INGRESS,
                                          std::string(kPortName) + ":" + kIngressSciHex);
        EXPECT_FALSE(result);

        EXPECT_EQ(g_removed_sas.size(), 0u);
        EXPECT_EQ(g_created_sas.size(), 0u);
        EXPECT_EQ(sc.m_encoding_an, static_cast<macsec_an_t>(0));
    }
}
