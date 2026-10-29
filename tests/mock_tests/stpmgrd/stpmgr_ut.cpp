/*
 * Component tests for stpmgrd (StpMgr).
 *
 * stpmgrd's job is to translate CONFIG_DB changes into the raw IPC messages it
 * sends to stpd over a unix datagram socket (STPD_SOCK_NAME). These tests
 * exercise stpmgrd in isolation — no stpd, no orchagent — by standing up a
 * "fake stpd": a datagram socket bound at STPD_SOCK_NAME that captures exactly
 * what stpmgrd emits. Each StpMgr::doStp*Task handler gets its own test that
 * drives one config change and asserts the emitted message type + fields.
 *
 * This validates the *sender* half of the stpmgrd<->stpd IPC contract; the
 * *receiver* half (stpd parsing + APPL_DB output) is covered by a sibling
 * gtest in sonic-stp.
 *
 * MST port and member config:
 *   - DoStpPortTask_MstLinkTypeMapping      -> link_type value->enum mapping
 *                                              (link_type is a string; parsing
 *                                              it as an integer dropped the
 *                                              port's STP_PORT config in MST)
 *   - DoVlanMemUpdateTask_MstVlanPortList   -> STP_MST_VLAN_PORT_LIST_CONFIG on
 *                                              VLAN member updates in MST
 *   - every assertion checks STP_IPC_MSG.proto_mode == active mode (the
 *     l2ProtoEnabled-in-header fix)
 *
 * MST enable-time port enrolment (enrolMstVlanMembers):
 *   - EnrolPreexistingMstMembersOnEnable    -> members that pre-exist the MSTP
 *                                              enable are replayed to stpd
 *   - ReEnrolMstMembersAfterModeToggle      -> members re-enrol after an
 *                                              MSTP -> NONE -> MSTP toggle
 */
#include "stpmgr.h"
#include "gtest/gtest.h"
#include "mock_table.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <chrono>

namespace stpmgr_ut
{
    using namespace swss;
    using namespace std;

    // StpMgr subclass exposing a DEL injector. applyAndRun() replays only SETs
    // (via addExistingData), so a table DEL — e.g. an STP disable that drives
    // l2ProtoEnabled back to NONE — must be delivered through the consumer.
    class TestStpMgr : public StpMgr
    {
    public:
        using StpMgr::StpMgr;
        void deliverDel(const string &tableName, const string &key)
        {
            Consumer *c = dynamic_cast<Consumer *>(getExecutor(tableName));
            if (c)
                c->addToSync(KeyOpFieldsValuesTuple(key, DEL_COMMAND, vector<FieldValueTuple>()));
        }
        // One row's SET, and only that one: applyAndRun() replays the whole
        // table, which re-sends every other row too.
        void deliverSet(const string &tableName, const string &key, const vector<FieldValueTuple> &fvs)
        {
            Consumer *c = dynamic_cast<Consumer *>(getExecutor(tableName));
            if (c)
                c->addToSync(KeyOpFieldsValuesTuple(key, SET_COMMAND, fvs));
        }
    };

    class StpMgrTest : public ::testing::Test
    {
    protected:
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_state_db;
        shared_ptr<TestStpMgr> m_stpMgr;
        int m_fakeStpdFd = -1;  // datagram socket standing in for stpd

        void SetUp() override
        {
            ::testing_db::reset();
            m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db    = make_shared<DBConnector>("APPL_DB", 0);
            m_state_db  = make_shared<DBConnector>("STATE_DB", 0);

            // The IPC sockets live in a temp dir (see Makefile.am override);
            // /var/run is not bindable in the CI build container. Ensure the
            // parent dir exists before either socket is bound. 0700 (not 0777)
            // so a concurrent run by another user can't drop a stale socket in
            // it; check the result so a real failure surfaces here rather than
            // as the less-obvious bind() ASSERT below.
            string sockPath(STPD_SOCK_NAME);
            auto slash = sockPath.find_last_of('/');
            if (slash != string::npos)
            {
                string dir = sockPath.substr(0, slash);
                if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
                {
                    FAIL() << "mkdir(" << dir << "): " << strerror(errno);
                }
            }

            // Bind the fake stpd receiver BEFORE stpmgrd opens its socket, so
            // every sendMsgStpd() sendto(STPD_SOCK_NAME) lands here. unlink any
            // stale socket first; if bind still fails (e.g. EADDRINUSE from a
            // socket this user can't remove), the message names the path.
            unlink(STPD_SOCK_NAME);
            m_fakeStpdFd = socket(AF_UNIX, SOCK_DGRAM, 0);
            ASSERT_GE(m_fakeStpdFd, 0) << "fake stpd socket(): " << strerror(errno);

            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, STPD_SOCK_NAME, sizeof(addr.sun_path) - 1);
            ASSERT_EQ(bind(m_fakeStpdFd, (struct sockaddr *)&addr, sizeof(addr)), 0)
                << "fake stpd bind(" << STPD_SOCK_NAME << "): " << strerror(errno);

            // Fail fast instead of hanging if stpmgrd sends nothing.
            struct timeval tv = {2, 0};
            setsockopt(m_fakeStpdFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            vector<TableConnector> tables = {
                {m_config_db.get(), CFG_STP_GLOBAL_TABLE_NAME},
                {m_config_db.get(), CFG_STP_VLAN_TABLE_NAME},
                {m_config_db.get(), CFG_STP_VLAN_PORT_TABLE_NAME},
                {m_config_db.get(), CFG_STP_PORT_TABLE_NAME},
                {m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME},
                {m_state_db.get(),  STATE_VLAN_MEMBER_TABLE_NAME},
                {m_config_db.get(), "STP_MST"},
                {m_config_db.get(), "STP_MST_INST"},
                {m_config_db.get(), "STP_MST_PORT"},
            };
            m_stpMgr.reset(new TestStpMgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables));
            // Deterministic base MAC so STP_BRIDGE_CONFIG_MSG.base_mac_addr is assertable.
            m_stpMgr->macAddress = MacAddress("00:11:22:33:44:55");
            m_stpMgr->ipcInitStpd();

            // Mirror stpmgrd's init handshake: it reads the max instance count
            // from STATE_DB STP_TABLE|GLOBAL (published by stpd) before any VLAN
            // can be assigned an instance. Without this, max_stp_instances stays
            // 0 and PVST allocL2Instance() fails, silently dropping VLAN config.
            // Seed the entry so getStpMaxInstances() returns immediately instead
            // of polling for up to 60s.
            Table stateStp(m_state_db.get(), STATE_STP_TABLE_NAME);
            stateStp.set("GLOBAL", {{"max_stp_inst", "255"}});
            m_stpMgr->getStpMaxInstances();
        }

        void TearDown() override
        {
            if (m_fakeStpdFd >= 0)
                close(m_fakeStpdFd);
            unlink(STPD_SOCK_NAME);
            unlink(STPMGRD_SOCK_NAME);
        }

        // Receive one IPC datagram from stpmgrd. Returns the byte count, or -1
        // (with errno == EAGAIN) on timeout.
        ssize_t recvIpc(uint8_t *buf, size_t len)
        {
            return recv(m_fakeStpdFd, buf, len, 0);
        }

        // Assert stpmgrd emitted no further datagram than the ones already
        // consumed — catches an accidental double-send regression. Uses a
        // non-blocking recv so it returns immediately (no 2s SO_RCVTIMEO wait)
        // and expects EAGAIN/EWOULDBLOCK (nothing queued).
        void assertNoMoreIpc()
        {
            uint8_t buf[1024];
            ssize_t got = recv(m_fakeStpdFd, buf, sizeof(buf), MSG_DONTWAIT);
            EXPECT_EQ(got, (ssize_t)-1) << "unexpected extra IPC message (" << got << " bytes)";
            if (got == -1)
            {
                EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK) << "recv errno: " << strerror(errno);
            }
        }

        // Apply a config change to one table and drive the mgr.
        void applyAndRun(const string &tableName, const string &key,
                         const vector<FieldValueTuple> &fvs,
                         shared_ptr<DBConnector> db)
        {
            Table t(db.get(), tableName);
            t.set(key, fvs);
            m_stpMgr->addExistingData(&t);
            m_stpMgr->doTask();
        }

        // Drain n datagrams emitted by setup steps we don't assert on.
        void drainN(int n)
        {
            uint8_t buf[1024];
            for (int i = 0; i < n; i++)
            {
                ssize_t got = recvIpc(buf, sizeof(buf));
                ASSERT_GT(got, (ssize_t)offsetof(STP_IPC_MSG, data))
                    << "expected " << n << " setup message(s), missing #" << (i + 1);
            }
        }

        // Enable a protocol globally (sets l2ProtoEnabled and stpGlobalTask) and
        // drain the resulting STP_BRIDGE_CONFIG so later recvs see only the
        // message under test.
        void enableMode(const string &mode)
        {
            applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                        {{"mode", mode}, {"rootguard_timeout", "30"}}, m_config_db);
            uint8_t buf[512];
            ssize_t n = recvIpc(buf, sizeof(buf));
            ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
            ASSERT_EQ(reinterpret_cast<STP_IPC_MSG *>(buf)->msg_type, STP_BRIDGE_CONFIG);
        }
        void enableMst()  { enableMode("mst"); }
        void enablePvst() { enableMode("pvst"); }

        // Write one row and deliver only its SET, then drive the mgr.
        void setOneAndRun(const string &tableName, const string &key,
                          const vector<FieldValueTuple> &fvs, shared_ptr<DBConnector> db)
        {
            Table t(db.get(), tableName);
            t.set(key, fvs);
            m_stpMgr->deliverSet(tableName, key, fvs);
            m_stpMgr->doTask();
        }

        // Turn spanning tree off again, as a mode change does first.
        void disableMode()
        {
            m_stpMgr->deliverDel(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL");
            m_stpMgr->doTask();
            drainN(1);
        }

        STP_IPC_MSG *recvMsg(uint8_t *buf, size_t len)
        {
            ssize_t n = recvIpc(buf, len);
            if (n <= (ssize_t)offsetof(STP_IPC_MSG, data))
                return nullptr;
            return reinterpret_cast<STP_IPC_MSG *>(buf);
        }

        // One STP_PORT_CONFIG for this interface, read and discarded.
        void expectPortConfig(const char *ifname)
        {
            uint8_t buf[1024];
            ssize_t n = recvIpc(buf, sizeof(buf));
            ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no STP_PORT_CONFIG from stpmgrd";
            auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
            EXPECT_EQ(ipc->msg_type, STP_PORT_CONFIG);
            auto *msg = reinterpret_cast<STP_PORT_CONFIG_MSG *>(ipc->data);
            EXPECT_STREQ(msg->intf_name, ifname);
        }
    };

    // A disable must reach stpd under the mode being disabled: stpd routes
    // STP_BRIDGE_CONFIG by proto_mode and drops one that arrives as L2_NONE,
    // which would leave the protocol running with no mode configured.
    TEST_F(StpMgrTest, DoStpGlobalTask_DisableMstReachesStpd)
    {
        enableMst();
        m_stpMgr->deliverDel(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL");
        m_stpMgr->doTask();

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message for the disable";
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);
        EXPECT_EQ(reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data)->opcode, STP_DEL_COMMAND);
    }

    TEST_F(StpMgrTest, DoStpGlobalTask_DisablePvstReachesStpd)
    {
        enablePvst();
        m_stpMgr->deliverDel(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL");
        m_stpMgr->doTask();

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message for the disable";
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_PVSTP);
        EXPECT_EQ(reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data)->opcode, STP_DEL_COMMAND);
    }

    // CONFIG_DB `STP|GLOBAL` -> STP_BRIDGE_CONFIG_MSG. Covers doStpGlobalTask.
    TEST_F(StpMgrTest, DoStpGlobalTask_SetMstMode)
    {
        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "mst"}, {"rootguard_timeout", "30"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(ipc->msg_len, (unsigned int)sizeof(STP_BRIDGE_CONFIG_MSG));
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        auto *msg = reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->stp_mode, L2_MSTP);
        EXPECT_EQ(msg->rootguard_timeout, 30);
        const uint8_t expected_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        EXPECT_EQ(memcmp(msg->base_mac_addr, expected_mac, sizeof(expected_mac)), 0);

        assertNoMoreIpc();
    }

    // CONFIG_DB `STP_PORT` (MST) -> STP_PORT_CONFIG_MSG, verifying the link_type
    // value->enum mapping. link_type is a string (auto, shared,
    // point-to-point); parsing it as an integer rejected the row and dropped
    // the port's STP_PORT config, so this also asserts all three ports are
    // delivered.
    TEST_F(StpMgrTest, DoStpPortTask_MstLinkTypeMapping)
    {
        enableMst();

        // Three ports, one per link_type value. Set together so a single
        // doTask() emits one STP_PORT_CONFIG per port.
        Table cfgStpPort(m_config_db.get(), CFG_STP_PORT_TABLE_NAME);
        cfgStpPort.set("Ethernet0", {{"enabled", "true"}, {"edge_port", "true"}, {"link_type", "point-to-point"}});
        cfgStpPort.set("Ethernet4", {{"enabled", "true"}, {"edge_port", "true"}, {"link_type", "shared"}});
        cfgStpPort.set("Ethernet8", {{"enabled", "true"}, {"edge_port", "true"}, {"link_type", "auto"}});
        m_stpMgr->addExistingData(&cfgStpPort);
        m_stpMgr->doTask();

        // Collect the three emitted STP_PORT_CONFIG messages, keyed by ifname.
        map<string, LinkType> got;
        for (int i = 0; i < 3; i++)
        {
            uint8_t buf[1024];
            ssize_t n = recvIpc(buf, sizeof(buf));
            ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data))
                << "expected 3 STP_PORT_CONFIG messages, got " << i
                << " (regression: link_type parsing threw and dropped config)";
            auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
            ASSERT_EQ(ipc->msg_type, STP_PORT_CONFIG);
            EXPECT_EQ(ipc->proto_mode, L2_MSTP);
            auto *msg = reinterpret_cast<STP_PORT_CONFIG_MSG *>(ipc->data);
            got[msg->intf_name] = msg->link_type;
        }

        EXPECT_EQ(got["Ethernet0"], POINT_TO_POINT);
        EXPECT_EQ(got["Ethernet4"], SHARED);
        EXPECT_EQ(got["Ethernet8"], AUTO);

        assertNoMoreIpc();  // exactly 3 STP_PORT_CONFIG, no extra
    }

    // STATE_DB VLAN_MEMBER update (MST) -> STP_MST_VLAN_PORT_LIST_CONFIG.
    // VLAN membership reaches stpd in MST mode via STP_MST_VLAN_PORT_MAP.
    TEST_F(StpMgrTest, DoVlanMemUpdateTask_MstVlanPortList)
    {
        enableMst();

        // getAllVlanMem() reads the member list from STATE_DB VLAN_MEMBER and
        // its tagging mode from CONFIG_DB VLAN_MEMBER.
        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|Ethernet0", {{"tagging_mode", "untagged"}});

        applyAndRun(STATE_VLAN_MEMBER_TABLE_NAME, "Vlan10|Ethernet0",
                    {{"state", "ok"}}, m_state_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_VLAN_PORT_LIST_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        auto *msg = reinterpret_cast<STP_MST_VLAN_PORT_MAP *>(ipc->data);
        EXPECT_EQ(msg->vlan_id, 10);
        EXPECT_EQ(msg->port_count, 1);
        EXPECT_EQ(msg->stp_mode, L2_MSTP);
        EXPECT_EQ(msg->add, 1);
        ASSERT_GE(ipc->msg_len, (unsigned int)(sizeof(STP_MST_VLAN_PORT_MAP) + sizeof(PORT_LIST)));
        EXPECT_STREQ(msg->port_list[0].intf_name, "Ethernet0");
        EXPECT_EQ(msg->port_list[0].tagging_mode, UNTAGGED_MODE);

        // MST path continues after the single STP_MST_VLAN_PORT_LIST_CONFIG;
        // assert it did not also fall through to the PVST STP_VLAN_MEM_CONFIG.
        assertNoMoreIpc();
    }

    // Enable-time enrolment: a VLAN member that pre-exists the MSTP enable must
    // be replayed to stpd on enable. A mode enable does not re-fire the member's
    // STATE_VLAN_MEMBER event, so without enrolMstVlanMembers() the port never
    // enrols (previously required a manual VLAN-membership flap).
    TEST_F(StpMgrTest, EnrolPreexistingMstMembersOnEnable)
    {
        // Member exists (STATE_DB state + CONFIG_DB tagging mode) BEFORE STP is
        // enabled, and its event is consumed-and-dropped while l2Proto is NONE.
        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|Ethernet0", {{"tagging_mode", "untagged"}});
        applyAndRun(STATE_VLAN_MEMBER_TABLE_NAME, "Vlan10|Ethernet0",
                    {{"state", "ok"}}, m_state_db);
        assertNoMoreIpc();  // STP disabled -> nothing sent for the member

        // Enable MSTP: enableMst() consumes the STP_BRIDGE_CONFIG; the fix then
        // replays the pre-existing membership as STP_MST_VLAN_PORT_LIST_CONFIG.
        enableMst();

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data))
            << "no enrolment on MSTP enable: pre-existing VLAN member was not replayed";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_VLAN_PORT_LIST_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        auto *msg = reinterpret_cast<STP_MST_VLAN_PORT_MAP *>(ipc->data);
        EXPECT_EQ(msg->vlan_id, 10);
        EXPECT_EQ(msg->add, 1);
        ASSERT_EQ(msg->port_count, 1);
        EXPECT_STREQ(msg->port_list[0].intf_name, "Ethernet0");
        assertNoMoreIpc();
    }

    // MSTP -> NONE -> MSTP toggle: pre-existing members must re-enrol on the
    // re-enable. Their STATE_VLAN_MEMBER events were consumed on the first
    // enable; a disable/re-enable fires no new member events, so only the
    // enable-time replay recovers them.
    TEST_F(StpMgrTest, ReEnrolMstMembersAfterModeToggle)
    {
        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|Ethernet0", {{"tagging_mode", "untagged"}});
        applyAndRun(STATE_VLAN_MEMBER_TABLE_NAME, "Vlan10|Ethernet0",
                    {{"state", "ok"}}, m_state_db);

        // First enable enrols the member.
        enableMst();
        drainN(1);              // STP_MST_VLAN_PORT_LIST_CONFIG (enrol #1)
        assertNoMoreIpc();

        // Disable STP: DEL STP|GLOBAL drives l2ProtoEnabled back to NONE.
        m_stpMgr->deliverDel(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL");
        m_stpMgr->doTask();
        drainN(1);              // STP_BRIDGE_CONFIG (del)
        assertNoMoreIpc();

        // Re-enable: the member must enrol again (no member event re-fired).
        enableMst();

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data))
            << "no re-enrolment after MSTP -> NONE -> MSTP toggle";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_VLAN_PORT_LIST_CONFIG);
        auto *msg = reinterpret_cast<STP_MST_VLAN_PORT_MAP *>(ipc->data);
        EXPECT_EQ(msg->vlan_id, 10);
        EXPECT_EQ(msg->add, 1);
        ASSERT_EQ(msg->port_count, 1);
        EXPECT_STREQ(msg->port_list[0].intf_name, "Ethernet0");
        assertNoMoreIpc();
    }

    // CONFIG_DB `STP_MST|GLOBAL` -> STP_MST_GLOBAL_CONFIG_MSG. Covers doStpMstGlobalTask.
    TEST_F(StpMgrTest, DoStpMstGlobalTask_SetRegion)
    {
        enableMst();

        applyAndRun("STP_MST", "GLOBAL",
                    {{"name", "region1"}, {"revision", "5"}, {"forward_delay", "15"},
                     {"hello_time", "2"}, {"max_age", "20"}, {"max_hops", "20"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_GLOBAL_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        auto *msg = reinterpret_cast<STP_MST_GLOBAL_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_STREQ(msg->name, "region1");
        EXPECT_EQ(msg->revision_number, 5u);
        EXPECT_EQ((int)msg->forward_delay, 15);
        EXPECT_EQ((int)msg->hello_time, 2);
        EXPECT_EQ((int)msg->max_age, 20);
        EXPECT_EQ((int)msg->max_hops, 20);

        assertNoMoreIpc();
    }

    // CONFIG_DB `STP_MST_INST|<id>` -> STP_MST_INST_CONFIG_MSG.
    // Covers doStpMstInstTask (bridge_priority + parsed vlan). CONFIG_DB holds
    // the leaf-list as "vlan@", and the Table strips the "@" on read, so the
    // row is written here as the CLI writes it and read as stpmgrd sees it.
    // Gating is
    // stpGlobalTask + (stpPortTask || no STP_PORT); we configure no STP_PORT.
    TEST_F(StpMgrTest, DoStpMstInstTask_SetInstance)
    {
        enableMst();

        applyAndRun("STP_MST_INST", "1",
                    {{"bridge_priority", "8192"}, {"vlan@", "10,20,30"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_INST_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        auto *msg = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->mst_id, 1);
        EXPECT_EQ(msg->priority, 8192);
        ASSERT_EQ(msg->vlan_count, 3);
        EXPECT_EQ(msg->vlan_list[0].vlan_id, 10);
        EXPECT_EQ(msg->vlan_list[1].vlan_id, 20);
        EXPECT_EQ(msg->vlan_list[2].vlan_id, 30);

        assertNoMoreIpc();
    }

    // The YANG model defines the mapping as leaf-list "vlan", stored as
    // "vlan@" and read as "vlan". A row written by an older image carries
    // "vlan_list"; it must
    // still reach the daemon rather than arriving as an empty mapping.
    TEST_F(StpMgrTest, DoStpMstInstTask_AcceptsLegacyVlanListField)
    {
        enableMst();

        applyAndRun("STP_MST_INST", "1",
                    {{"bridge_priority", "8192"}, {"vlan_list", "10,20,30"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        auto *msg = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(ipc->data);
        ASSERT_EQ(msg->vlan_count, 3);
        EXPECT_EQ(msg->vlan_list[0].vlan_id, 10);
        EXPECT_EQ(msg->vlan_list[2].vlan_id, 30);

        assertNoMoreIpc();
    }

    // With both spellings present the YANG one wins, whichever order the
    // fields arrive in.
    TEST_F(StpMgrTest, DoStpMstInstTask_YangFieldWinsOverLegacy)
    {
        enableMst();

        applyAndRun("STP_MST_INST", "1",
                    {{"vlan_list", "10,20,30"}, {"vlan", "40"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        auto *msg = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(ipc->data);
        ASSERT_EQ(msg->vlan_count, 1);
        EXPECT_EQ(msg->vlan_list[0].vlan_id, 40);

        assertNoMoreIpc();
    }

    // A leaf-list element may itself be a range, which the parser already
    // supports; renaming the field must not change that.
    TEST_F(StpMgrTest, DoStpMstInstTask_VlanRangeStillExpands)
    {
        enableMst();

        applyAndRun("STP_MST_INST", "1",
                    {{"bridge_priority", "8192"}, {"vlan", "10,20-22"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        auto *msg = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(ipc->data);
        ASSERT_EQ(msg->vlan_count, 4);
        EXPECT_EQ(msg->vlan_list[0].vlan_id, 10);
        EXPECT_EQ(msg->vlan_list[1].vlan_id, 20);
        EXPECT_EQ(msg->vlan_list[3].vlan_id, 22);

        assertNoMoreIpc();
    }

    // CONFIG_DB `STP_MST_PORT|<id>|<intf>` -> STP_MST_INST_PORT_CONFIG_MSG.
    // Covers doStpMstInstPortTask, whose gate needs all of global + MST-inst +
    // port tasks to have run, so we drive each first.
    TEST_F(StpMgrTest, DoStpMstInstPortTask_SetPort)
    {
        enableMst();
        // stpPortTask := true
        applyAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0",
                    {{"enabled", "true"}, {"link_type", "auto"}}, m_config_db);
        drainN(1);  // STP_PORT_CONFIG
        // stpMstInstTask := true
        applyAndRun("STP_MST_INST", "1",
                    {{"bridge_priority", "8192"}, {"vlan", "10"}}, m_config_db);
        drainN(1);  // STP_MST_INST_CONFIG

        applyAndRun("STP_MST_PORT", "1|Ethernet0",
                    {{"path_cost", "200"}, {"priority", "112"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_INST_PORT_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        auto *msg = reinterpret_cast<STP_MST_INST_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->mst_id, 1);
        EXPECT_STREQ(msg->intf_name, "Ethernet0");
        EXPECT_EQ(msg->path_cost, 200);
        EXPECT_EQ(msg->priority, 112);

        assertNoMoreIpc();
    }

    // A per-instance row carries only the attribute the operator set, so the
    // other must travel as "unset". stpd reads 0 as an explicit value in MST
    // mode, and an explicit path cost of 0 both pins the instance to it and
    // sets the per-instance flag that stops the port-wide value applying.
    TEST_F(StpMgrTest, DoStpMstInstPortTask_PriorityOnlyRowLeavesPathCostUnset)
    {
        enableMst();
        applyAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0",
                    {{"enabled", "true"}, {"link_type", "auto"}}, m_config_db);
        drainN(1);
        applyAndRun("STP_MST_INST", "1",
                    {{"bridge_priority", "8192"}, {"vlan", "10"}}, m_config_db);
        drainN(1);

        applyAndRun("STP_MST_PORT", "1|Ethernet0", {{"priority", "112"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        auto *msg = reinterpret_cast<STP_MST_INST_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->priority, 112);
        EXPECT_EQ(msg->path_cost, STP_MST_PORT_ATTR_UNSET);

        assertNoMoreIpc();
    }

    // The mirror case: a path-cost-only row must leave priority unset.
    TEST_F(StpMgrTest, DoStpMstInstPortTask_PathCostOnlyRowLeavesPriorityUnset)
    {
        enableMst();
        applyAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0",
                    {{"enabled", "true"}, {"link_type", "auto"}}, m_config_db);
        drainN(1);
        applyAndRun("STP_MST_INST", "1",
                    {{"bridge_priority", "8192"}, {"vlan", "10"}}, m_config_db);
        drainN(1);

        applyAndRun("STP_MST_PORT", "1|Ethernet0", {{"path_cost", "200"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        auto *msg = reinterpret_cast<STP_MST_INST_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->path_cost, 200);
        EXPECT_EQ(msg->priority, STP_MST_PORT_ATTR_UNSET);

        assertNoMoreIpc();
    }

    // PVST uses a DIFFERENT convention and must not be "fixed" to match MST.
    // stpd guards the PVST path with `if (pmsg->path_cost)`, so 0 is what
    // means unset there; sending -1 would configure a path cost of -1.
    TEST_F(StpMgrTest, DoStpVlanPortTask_PriorityOnlyRowKeepsPathCostZero)
    {
        enablePvst();
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan100", {{"state", "ok"}});
        applyAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0",
                    {{"enabled", "true"}}, m_config_db);
        drainN(1);
        applyAndRun("STP_VLAN", "Vlan100", {{"enabled", "true"}}, m_config_db);
        drainN(1);

        applyAndRun("STP_VLAN_PORT", "Vlan100|Ethernet0", {{"priority", "112"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        auto *msg = reinterpret_cast<STP_VLAN_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->priority, 112);
        EXPECT_EQ(msg->path_cost, 0) << "PVST treats 0 as unset; -1 would configure a cost";

        assertNoMoreIpc();
    }

    // The hardware holds a fixed number of spanning tree instances. A VLAN
    // enabled beyond that is held with a STATE_DB row saying why, and takes
    // the next instance freed, instead of being dropped in silence.
    TEST_F(StpMgrTest, DoStpVlanTask_VlanBeyondTheLimitWaitsForAFreedInstance)
    {
        Table stateStp(m_state_db.get(), STATE_STP_TABLE_NAME);
        stateStp.set("GLOBAL", {{"max_stp_inst", "2"}});
        m_stpMgr->getStpMaxInstances();
        enablePvst();

        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        for (auto v : {"Vlan100", "Vlan200", "Vlan300"})
            stateVlan.set(v, {{"state", "ok"}});
        vector<FieldValueTuple> row = {{"enabled", "true"}, {"forward_delay", "15"}, {"hello_time", "2"},
                                       {"max_age", "20"}, {"priority", "32768"}};

        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan100", row, m_config_db);
        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan200", row, m_config_db);
        drainN(2);

        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan300", row, m_config_db);
        assertNoMoreIpc();

        Table stateStpVlan(m_state_db.get(), STATE_STP_VLAN_TABLE_NAME);
        vector<FieldValueTuple> held;
        ASSERT_TRUE(stateStpVlan.get("Vlan300", held));
        EXPECT_EQ(fvValue(held[0]), STP_INSTANCE_UNAVAILABLE);

        // Disabling VLAN 200 frees its instance; VLAN 300 takes it.
        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan200", {{"enabled", "false"}}, m_config_db);
        m_stpMgr->doTask();

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_VLAN_CONFIG) << "first message type " << ipc->msg_type << " len " << n;
        auto *del = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(del->opcode, STP_DEL_COMMAND);
        EXPECT_EQ(del->inst_id, 1);  // the delete names the instance, VLAN 200's was the second

        n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "VLAN 300 never got the freed instance";
        EXPECT_EQ(ipc->msg_type, STP_VLAN_CONFIG) << "second message type " << ipc->msg_type << " len " << n;
        auto *set = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(set->opcode, STP_SET_COMMAND);
        EXPECT_EQ(set->vlan_id, 300);
        EXPECT_EQ((int)set->newInstance, 1);
        assertNoMoreIpc();
        EXPECT_FALSE(stateStpVlan.get("Vlan300", held));
    }

    // Disabling a held VLAN only drops the hold.
    TEST_F(StpMgrTest, DoStpVlanTask_DisablingAHeldVlanDropsTheHold)
    {
        Table stateStp(m_state_db.get(), STATE_STP_TABLE_NAME);
        stateStp.set("GLOBAL", {{"max_stp_inst", "1"}});
        m_stpMgr->getStpMaxInstances();
        enablePvst();

        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan100", {{"state", "ok"}});
        stateVlan.set("Vlan200", {{"state", "ok"}});
        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan100", {{"enabled", "true"}}, m_config_db);
        drainN(1);
        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan200", {{"enabled", "true"}}, m_config_db);
        assertNoMoreIpc();

        Table stateStpVlan(m_state_db.get(), STATE_STP_VLAN_TABLE_NAME);
        vector<FieldValueTuple> held;
        ASSERT_TRUE(stateStpVlan.get("Vlan200", held));

        m_stpMgr->deliverDel(CFG_STP_VLAN_TABLE_NAME, "Vlan200");
        m_stpMgr->doTask();
        assertNoMoreIpc();
        EXPECT_FALSE(stateStpVlan.get("Vlan200", held));
    }

    // Same for MST: the CIST takes one instance, so with a limit of three
    // only two MSTIs fit, and the third waits for one to be deleted.
    TEST_F(StpMgrTest, DoStpMstInstTask_InstanceBeyondTheLimitWaitsForADeletedOne)
    {
        Table stateStp(m_state_db.get(), STATE_STP_TABLE_NAME);
        stateStp.set("GLOBAL", {{"max_stp_inst", "3"}});
        m_stpMgr->getStpMaxInstances();
        enableMst();

        setOneAndRun("STP_MST_INST", "1", {{"bridge_priority", "8192"}, {"vlan@", "10"}}, m_config_db);
        setOneAndRun("STP_MST_INST", "2", {{"bridge_priority", "8192"}, {"vlan@", "20"}}, m_config_db);
        drainN(2);

        setOneAndRun("STP_MST_INST", "3", {{"bridge_priority", "8192"}, {"vlan@", "30"}}, m_config_db);
        assertNoMoreIpc();

        Table stateMstInst(m_state_db.get(), STATE_STP_MST_INST_TABLE_NAME);
        vector<FieldValueTuple> held;
        ASSERT_TRUE(stateMstInst.get("3", held));
        EXPECT_EQ(fvValue(held[0]), STP_INSTANCE_UNAVAILABLE);

        m_stpMgr->deliverDel("STP_MST_INST", "2");
        m_stpMgr->doTask();
        m_stpMgr->doTask();

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
        auto *del = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(reinterpret_cast<STP_IPC_MSG *>(buf)->data);
        EXPECT_EQ(del->opcode, STP_DEL_COMMAND);
        EXPECT_EQ(del->mst_id, 2);

        n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "instance 3 never got the freed instance";
        auto *set = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(reinterpret_cast<STP_IPC_MSG *>(buf)->data);
        EXPECT_EQ(set->opcode, STP_SET_COMMAND);
        EXPECT_EQ(set->mst_id, 3);
        ASSERT_EQ(set->vlan_count, 1);
        EXPECT_EQ(set->vlan_list[0].vlan_id, 30);
        assertNoMoreIpc();
        EXPECT_FALSE(stateMstInst.get("3", held));
    }

    // The CIST row and an update to a known instance are never counted
    // again against the limit.
    TEST_F(StpMgrTest, DoStpMstInstTask_UpdatingAKnownInstanceIsNotCountedAgain)
    {
        Table stateStp(m_state_db.get(), STATE_STP_TABLE_NAME);
        stateStp.set("GLOBAL", {{"max_stp_inst", "2"}});
        m_stpMgr->getStpMaxInstances();
        enableMst();

        setOneAndRun("STP_MST_INST", "0", {{"bridge_priority", "32768"}}, m_config_db);
        setOneAndRun("STP_MST_INST", "1", {{"bridge_priority", "8192"}, {"vlan@", "10"}}, m_config_db);
        setOneAndRun("STP_MST_INST", "1", {{"bridge_priority", "4096"}, {"vlan@", "10,20"}}, m_config_db);
        drainN(3);
        assertNoMoreIpc();

        Table stateMstInst(m_state_db.get(), STATE_STP_MST_INST_TABLE_NAME);
        vector<FieldValueTuple> held;
        EXPECT_FALSE(stateMstInst.get("1", held));
    }

    // After a mode change the MST rows and the mode row land in CONFIG_DB
    // together, and the consumers run in no fixed order. The mode message
    // re-initialises the bridge in stpd, so region config that went out
    // before it was lost and the switch ran in a region of its own. The
    // region, instance and port tasks wait until the mode has gone out.
    TEST_F(StpMgrTest, DoStpMstGlobalTask_RegionWaitsForTheModeAfterAModeChange)
    {
        enableMst();
        disableMode();

        setOneAndRun("STP_MST", "GLOBAL", {{"name", "nh-region"}, {"revision", "1"}}, m_config_db);
        assertNoMoreIpc();

        enableMst();
        m_stpMgr->doTask();

        uint8_t buf[1024];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the region never went out after the mode";
        EXPECT_EQ(ipc->msg_type, STP_MST_GLOBAL_CONFIG);
        auto *msg = reinterpret_cast<STP_MST_GLOBAL_CONFIG_MSG *>(ipc->data);
        EXPECT_STREQ(msg->name, "nh-region");
        EXPECT_EQ(msg->revision_number, 1);
        assertNoMoreIpc();
    }

    TEST_F(StpMgrTest, DoStpMstInstTask_InstanceWaitsForTheModeAfterAModeChange)
    {
        enableMst();
        disableMode();

        setOneAndRun("STP_MST_INST", "1", {{"bridge_priority", "8192"}, {"vlan@", "10"}}, m_config_db);
        assertNoMoreIpc();

        enableMst();
        m_stpMgr->doTask();

        uint8_t buf[1024];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the instance never went out after the mode";
        EXPECT_EQ(ipc->msg_type, STP_MST_INST_CONFIG);
        auto *msg = reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->mst_id, 1);
        assertNoMoreIpc();
    }

    // stpd routes a port message by the mode in its header, so one sent with
    // no mode is dropped and the port never enrols.
    TEST_F(StpMgrTest, DoStpPortTask_PortWaitsForTheModeAfterAModeChange)
    {
        enableMst();
        disableMode();

        setOneAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0", {{"enabled", "true"}}, m_config_db);
        assertNoMoreIpc();

        enableMst();
        m_stpMgr->doTask();

        uint8_t buf[1024];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the port never went out after the mode";
        EXPECT_EQ(ipc->msg_type, STP_PORT_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);
        auto *msg = reinterpret_cast<STP_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_STREQ(msg->intf_name, "Ethernet0");
        assertNoMoreIpc();
    }

    // A mode edited in place on the STP|GLOBAL row. stpd routes the bridge
    // config by the mode in the envelope, so a set under the old mode keeps
    // the old protocol running: the old mode is taken down first, then the
    // new one goes out under its own mode.
    TEST_F(StpMgrTest, DoStpGlobalTask_ModeChangeTakesTheOldModeDownFirst)
    {
        enablePvst();

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "mst"}, {"rootguard_timeout", "30"}}, m_config_db);

        uint8_t buf[512];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "no delete for the old mode";
        EXPECT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_PVSTP);
        EXPECT_EQ(reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data)->opcode, STP_DEL_COMMAND);

        ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "no set for the new mode";
        EXPECT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);
        auto *msg = reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->stp_mode, L2_MSTP);
        assertNoMoreIpc();
    }

    TEST_F(StpMgrTest, DoStpGlobalTask_ModeChangeBackToPvstTakesMstDownFirst)
    {
        enableMst();

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "30"}}, m_config_db);

        uint8_t buf[512];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "no delete for the old mode";
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);
        EXPECT_EQ(reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data)->opcode, STP_DEL_COMMAND);

        ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "no set for the new mode";
        EXPECT_EQ(ipc->proto_mode, L2_PVSTP);
        EXPECT_EQ(reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data)->opcode, STP_SET_COMMAND);
        assertNoMoreIpc();
    }

    // Rewriting the row with the same mode is not a mode change.
    TEST_F(StpMgrTest, DoStpGlobalTask_SameModeRewriteIsNotAModeChange)
    {
        enableMst();

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "mst"}, {"rootguard_timeout", "40"}}, m_config_db);

        uint8_t buf[512];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr);
        EXPECT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);
        auto *msg = reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->rootguard_timeout, 40);
        assertNoMoreIpc();
    }

    // The PVST VLAN rows went out while MST was running and stpd dropped
    // them; after the change to PVST they go out again, under PVST.
    TEST_F(StpMgrTest, DoStpGlobalTask_ModeChangeReplaysThePvstVlans)
    {
        enableMst();
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan100", {{"state", "ok"}});
        setOneAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan100", {{"enabled", "true"}}, m_config_db);
        drainN(1);  // sent under MST, which stpd ignores

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "30"}}, m_config_db);
        drainN(2);  // the delete and the set

        uint8_t buf[1024];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the VLAN never went out again after the mode";
        EXPECT_EQ(ipc->msg_type, STP_VLAN_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_PVSTP);
        auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->vlan_id, 100);
        EXPECT_EQ((int)msg->newInstance, 1);
        assertNoMoreIpc();
    }

    // The region and instances went out under the first MST run and were
    // lost with it; after MST -> PVST -> MST they go out again, and the
    // port config with them.
    TEST_F(StpMgrTest, DoStpGlobalTask_ModeChangeReplaysTheMstConfig)
    {
        enableMst();
        setOneAndRun("STP_MST", "GLOBAL", {{"name", "nh-region"}, {"revision", "1"}}, m_config_db);
        drainN(1);
        setOneAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0", {{"enabled", "true"}}, m_config_db);
        drainN(1);
        setOneAndRun("STP_MST_INST", "1", {{"bridge_priority", "8192"}, {"vlan@", "10"}}, m_config_db);
        drainN(1);

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "30"}}, m_config_db);
        drainN(3);  // the delete, the set, and the port under PVST

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "mst"}, {"rootguard_timeout", "30"}}, m_config_db);
        drainN(2);  // the delete and the set

        uint8_t buf[1024];
        auto *ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the port never went out again after the mode";
        EXPECT_EQ(ipc->msg_type, STP_PORT_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_MSTP);

        ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the region never went out again after the mode";
        EXPECT_EQ(ipc->msg_type, STP_MST_GLOBAL_CONFIG);
        EXPECT_STREQ(reinterpret_cast<STP_MST_GLOBAL_CONFIG_MSG *>(ipc->data)->name, "nh-region");

        ipc = recvMsg(buf, sizeof(buf));
        ASSERT_NE(ipc, nullptr) << "the instance never went out again after the mode";
        EXPECT_EQ(ipc->msg_type, STP_MST_INST_CONFIG);
        EXPECT_EQ(reinterpret_cast<STP_MST_INST_CONFIG_MSG *>(ipc->data)->mst_id, 1);
        assertNoMoreIpc();
    }

    // CONFIG_DB `STP_VLAN|Vlan<id>` (PVST) -> STP_VLAN_CONFIG_MSG. Covers
    // doStpVlanTask: allocates an instance and emits per-VLAN bridge config.
    // Requires PVST mode and a ready VLAN in STATE_DB.
    TEST_F(StpMgrTest, DoStpVlanTask_SetVlan)
    {
        enablePvst();
        // isVlanStateOk() checks STATE_DB VLAN_TABLE for the VLAN.
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan100", {{"state", "ok"}});

        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan100",
                    {{"enabled", "true"}, {"forward_delay", "15"}, {"hello_time", "2"},
                     {"max_age", "20"}, {"priority", "32768"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_VLAN_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_PVSTP);

        auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->vlan_id, 100);
        EXPECT_EQ((int)msg->newInstance, 1);  // first time this VLAN is instanced
        EXPECT_EQ(msg->forward_delay, 15);
        EXPECT_EQ(msg->hello_time, 2);
        EXPECT_EQ(msg->max_age, 20);
        EXPECT_EQ(msg->priority, 32768);
        EXPECT_EQ(msg->count, 0);  // no VLAN members configured

        assertNoMoreIpc();
    }

    // A PVST VLAN row with no timers or priority (only `enabled` set) gets the
    // protocol defaults, not 0: a priority of 0 made the switch root everywhere.
    TEST_F(StpMgrTest, DoStpVlanTask_MissingFieldsUseDefaults)
    {
        enablePvst();
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan110", {{"state", "ok"}});

        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan110", {{"enabled", "true"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        ASSERT_EQ(ipc->msg_type, STP_VLAN_CONFIG);
        auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->vlan_id, 110);
        EXPECT_EQ(msg->forward_delay, 15);
        EXPECT_EQ(msg->hello_time, 2);
        EXPECT_EQ(msg->max_age, 20);
        EXPECT_EQ(msg->priority, 32768);

        assertNoMoreIpc();
    }

    // Fields a VLAN row leaves out come from STP|GLOBAL; fields it sets win.
    TEST_F(StpMgrTest, DoStpVlanTask_MissingFieldsFollowGlobal)
    {
        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "30"}, {"priority", "4096"},
                     {"forward_delay", "10"}}, m_config_db);
        drainN(1);  // STP_BRIDGE_CONFIG

        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan110", {{"state", "ok"}});
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan110",
                    {{"enabled", "true"}, {"hello_time", "3"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";
        auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(reinterpret_cast<STP_IPC_MSG *>(buf)->data);
        EXPECT_EQ(msg->priority, 4096);
        EXPECT_EQ(msg->forward_delay, 10);
        EXPECT_EQ(msg->hello_time, 3);
        EXPECT_EQ(msg->max_age, 20);

        assertNoMoreIpc();
    }

    // Changing the global priority reaches every PVST VLAN that has no priority
    // of its own; a VLAN with its own priority keeps it.
    TEST_F(StpMgrTest, DoStpGlobalTask_PriorityChangeReachesPvstVlans)
    {
        enablePvst();
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan110", {{"state", "ok"}});
        stateVlan.set("Vlan120", {{"state", "ok"}});
        Table cfgVlan(m_config_db.get(), CFG_STP_VLAN_TABLE_NAME);
        cfgVlan.set("Vlan120", {{"enabled", "true"}, {"priority", "8192"}});
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan110", {{"enabled", "true"}}, m_config_db);
        drainN(2);  // STP_VLAN_CONFIG for Vlan110 and Vlan120

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "30"}, {"priority", "4096"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
        ASSERT_EQ(reinterpret_cast<STP_IPC_MSG *>(buf)->msg_type, STP_BRIDGE_CONFIG);

        std::map<int, int> priorityByVlan;
        for (int i = 0; i < 2; i++)
        {
            n = recvIpc(buf, sizeof(buf));
            ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "missing VLAN refresh #" << (i + 1);
            auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
            ASSERT_EQ(ipc->msg_type, STP_VLAN_CONFIG);
            auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
            EXPECT_EQ((int)msg->newInstance, 0);
            priorityByVlan[msg->vlan_id] = msg->priority;
        }
        EXPECT_EQ(priorityByVlan[110], 4096);
        EXPECT_EQ(priorityByVlan[120], 8192);

        assertNoMoreIpc();
    }

    // A write to STP|GLOBAL that changes none of the fields the PVST VLANs
    // inherit (here rootguard_timeout) sends the bridge config and nothing
    // else; the VLANs are not re-sent.
    TEST_F(StpMgrTest, DoStpGlobalTask_UnrelatedChangeDoesNotRefreshPvstVlans)
    {
        enablePvst();
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan110", {{"state", "ok"}});
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan110", {{"enabled", "true"}}, m_config_db);
        drainN(1);  // STP_VLAN_CONFIG for Vlan110

        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "60"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        ASSERT_EQ(ipc->msg_type, STP_BRIDGE_CONFIG);
        EXPECT_EQ(reinterpret_cast<STP_BRIDGE_CONFIG_MSG *>(ipc->data)->rootguard_timeout, 60);
        assertNoMoreIpc();

        // The same row written again: still nothing for the VLANs.
        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "60"}}, m_config_db);
        n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
        ASSERT_EQ(reinterpret_cast<STP_IPC_MSG *>(buf)->msg_type, STP_BRIDGE_CONFIG);
        assertNoMoreIpc();
    }

    // A global field that is not a number (sonic-utilities can seed
    // priority=None) falls back to the protocol default instead of taking
    // every VLAN row down with it.
    TEST_F(StpMgrTest, DoStpVlanTask_MalformedGlobalFieldFallsBackToDefault)
    {
        applyAndRun(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL",
                    {{"mode", "pvst"}, {"rootguard_timeout", "30"}, {"priority", "None"}}, m_config_db);
        drainN(1);  // STP_BRIDGE_CONFIG
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan110", {{"state", "ok"}});

        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan110", {{"enabled", "true"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "the VLAN was dropped with the bad global field";
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        ASSERT_EQ(ipc->msg_type, STP_VLAN_CONFIG);
        auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->vlan_id, 110);
        EXPECT_EQ(msg->priority, STP_DFLT_BRIDGE_PRIORITY);
        assertNoMoreIpc();
    }

    // Malformed integer field must not crash stpmgrd. sonic-utilities can seed a
    // STP_VLAN row with forward_delay="None" (a PVST-only timer that is absent in
    // the STP|GLOBAL entry, so the getter returns None and it is serialized to the
    // string "None"). doStpVlanTask's stoi() then threw std::invalid_argument, the
    // exception propagated to main(), and stpmgrd exited 255 (autorestart off, so it
    // stayed down). The handler now logs and drops the offending entry, and keeps
    // processing later config. This test drives one unparseable entry and asserts
    // (a) nothing is emitted for it and (b) a subsequent valid VLAN still programs.
    TEST_F(StpMgrTest, DoStpVlanTask_MalformedIntFieldDroppedNoCrash)
    {
        enablePvst();
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan100", {{"state", "ok"}});
        stateVlan.set("Vlan200", {{"state", "ok"}});

        // Bad entry: forward_delay is the literal string "None".
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan100",
                    {{"enabled", "true"}, {"forward_delay", "None"}, {"hello_time", "2"},
                     {"max_age", "20"}, {"priority", "32768"}}, m_config_db);
        // Dropped, not emitted (and doTask() above did not throw).
        assertNoMoreIpc();

        // Daemon survived and is not wedged: a later valid VLAN still programs.
        // (Vlan100 is re-read too, and dropped again, so only Vlan200 is emitted.)
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan200",
                    {{"enabled", "true"}, {"forward_delay", "15"}, {"hello_time", "2"},
                     {"max_age", "20"}, {"priority", "32768"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data))
            << "daemon did not recover after dropping a malformed entry";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_VLAN_CONFIG);
        auto *msg = reinterpret_cast<STP_VLAN_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->vlan_id, 200);
        EXPECT_EQ(msg->forward_delay, 15);

        assertNoMoreIpc();
    }

    // CONFIG_DB `STP_VLAN_PORT|Vlan<id>|<intf>` (PVST) -> STP_VLAN_PORT_CONFIG_MSG.
    // Covers doStpVlanPortTask; its gate needs global + vlan + port tasks, and
    // the VLAN must already be mapped to an instance, so we drive those first.
    TEST_F(StpMgrTest, DoStpVlanPortTask_SetVlanPort)
    {
        enablePvst();
        // stpPortTask := true
        applyAndRun(CFG_STP_PORT_TABLE_NAME, "Ethernet0", {{"enabled", "true"}}, m_config_db);
        drainN(1);  // STP_PORT_CONFIG
        // stpVlanTask := true and Vlan100 mapped to an instance
        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan100", {{"state", "ok"}});
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan100",
                    {{"enabled", "true"}, {"forward_delay", "15"}, {"hello_time", "2"},
                     {"max_age", "20"}, {"priority", "32768"}}, m_config_db);
        drainN(1);  // STP_VLAN_CONFIG

        applyAndRun(CFG_STP_VLAN_PORT_TABLE_NAME, "Vlan100|Ethernet0",
                    {{"path_cost", "150"}, {"priority", "128"}}, m_config_db);

        uint8_t buf[512];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_VLAN_PORT_CONFIG);
        EXPECT_EQ(ipc->proto_mode, L2_PVSTP);

        auto *msg = reinterpret_cast<STP_VLAN_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
        EXPECT_EQ(msg->vlan_id, 100);
        EXPECT_STREQ(msg->intf_name, "Ethernet0");
        EXPECT_EQ(msg->path_cost, 150);
        EXPECT_EQ(msg->priority, 128);

        assertNoMoreIpc();
    }

    // A PortChannel drops out of spanning tree with its last member and has to
    // come back with the first one. The member add used to push only its
    // STP_PORT and STP_VLAN_PORT rows, never the VLAN membership itself, so
    // stpd never re-enrolled the LAG (and a LAG with no STP_PORT row got
    // nothing at all).
    TEST_F(StpMgrTest, DoLagMemUpdateTask_FirstMemberBackReenrolsLagVlansMst)
    {
        enableMst();

        Table stateLag(m_state_db.get(), STATE_LAG_TABLE_NAME);
        stateLag.set("PortChannel1", {{"state", "ok"}});
        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|PortChannel1", {{"tagging_mode", "tagged"}});
        Table stateVlanMember(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);
        stateVlanMember.set("Vlan10|PortChannel1", {{"state", "ok"}});

        auto expectLagInVlan10 = [&]() {
            uint8_t buf[1024];
            ssize_t n = recvIpc(buf, sizeof(buf));
            ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";
            auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
            EXPECT_EQ(ipc->msg_type, STP_MST_VLAN_PORT_LIST_CONFIG);
            auto *msg = reinterpret_cast<STP_MST_VLAN_PORT_MAP *>(ipc->data);
            EXPECT_EQ(msg->vlan_id, 10);
            EXPECT_EQ(msg->port_count, 1);
            EXPECT_EQ(msg->add, 1);
            EXPECT_STREQ(msg->port_list[0].intf_name, "PortChannel1");
            EXPECT_EQ(msg->port_list[0].tagging_mode, TAGGED_MODE);
        };

        // First member: the port's own config goes first, so stpd has the
        // PortChannel node before the membership arrives, then the VLAN.
        applyAndRun(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0",
                    {{"NULL", "NULL"}}, m_config_db);
        expectPortConfig("PortChannel1");
        expectLagInVlan10();
        assertNoMoreIpc();

        // Last member out, then back in: the LAG enrols again.
        m_stpMgr->deliverDel(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0");
        m_stpMgr->doTask();
        assertNoMoreIpc();

        applyAndRun(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0",
                    {{"NULL", "NULL"}}, m_config_db);
        expectPortConfig("PortChannel1");
        expectLagInVlan10();
        assertNoMoreIpc();
    }

    TEST_F(StpMgrTest, DoLagMemUpdateTask_FirstMemberBackReenrolsLagVlansPvst)
    {
        enablePvst();

        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan10", {{"state", "ok"}});
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan10",
                    {{"enabled", "true"}, {"forward_delay", "15"}, {"hello_time", "2"},
                     {"max_age", "20"}, {"priority", "32768"}}, m_config_db);
        drainN(1);  // STP_VLAN_CONFIG

        Table stateLag(m_state_db.get(), STATE_LAG_TABLE_NAME);
        stateLag.set("PortChannel1", {{"state", "ok"}});
        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|PortChannel1", {{"tagging_mode", "untagged"}});
        Table stateVlanMember(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);
        stateVlanMember.set("Vlan10|PortChannel1", {{"state", "ok"}});

        auto expectLagInVlan10 = [&]() {
            uint8_t buf[1024];
            ssize_t n = recvIpc(buf, sizeof(buf));
            ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";
            auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
            EXPECT_EQ(ipc->msg_type, STP_VLAN_MEM_CONFIG);
            EXPECT_EQ(ipc->proto_mode, L2_PVSTP);
            auto *msg = reinterpret_cast<STP_VLAN_MEM_CONFIG_MSG *>(ipc->data);
            EXPECT_EQ(msg->opcode, STP_SET_COMMAND);
            EXPECT_EQ(msg->vlan_id, 10);
            EXPECT_STREQ(msg->intf_name, "PortChannel1");
            EXPECT_EQ(msg->mode, UNTAGGED_MODE);
        };

        applyAndRun(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0",
                    {{"NULL", "NULL"}}, m_config_db);
        expectPortConfig("PortChannel1");
        expectLagInVlan10();
        assertNoMoreIpc();

        m_stpMgr->deliverDel(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0");
        m_stpMgr->doTask();
        assertNoMoreIpc();

        applyAndRun(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0",
                    {{"NULL", "NULL"}}, m_config_db);
        expectPortConfig("PortChannel1");
        expectLagInVlan10();
        assertNoMoreIpc();
    }

    // With STP_PORT and STP_VLAN_PORT rows, the member add replays all three
    // in the order stpd needs: the port, its membership, then the per-VLAN
    // cost and priority.
    TEST_F(StpMgrTest, DoLagMemUpdateTask_FirstMemberBackReplaysPortThenMembershipThenVlanPort)
    {
        enablePvst();

        Table stateVlan(m_state_db.get(), STATE_VLAN_TABLE_NAME);
        stateVlan.set("Vlan10", {{"state", "ok"}});
        applyAndRun(CFG_STP_VLAN_TABLE_NAME, "Vlan10",
                    {{"enabled", "true"}, {"forward_delay", "15"}, {"hello_time", "2"},
                     {"max_age", "20"}, {"priority", "32768"}}, m_config_db);
        drainN(1);  // STP_VLAN_CONFIG

        Table stateLag(m_state_db.get(), STATE_LAG_TABLE_NAME);
        stateLag.set("PortChannel1", {{"state", "ok"}});
        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|PortChannel1", {{"tagging_mode", "untagged"}});
        Table stateVlanMember(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);
        stateVlanMember.set("Vlan10|PortChannel1", {{"state", "ok"}});
        // Rows written straight to the tables, so nothing is sent for them here.
        Table cfgStpPort(m_config_db.get(), CFG_STP_PORT_TABLE_NAME);
        cfgStpPort.set("PortChannel1", {{"enabled", "true"}, {"bpdu_guard", "true"}});
        Table cfgStpVlanPort(m_config_db.get(), CFG_STP_VLAN_PORT_TABLE_NAME);
        cfgStpVlanPort.set("Vlan10|PortChannel1", {{"path_cost", "300"}, {"priority", "64"}});

        applyAndRun(CFG_LAG_MEMBER_TABLE_NAME, "PortChannel1|Ethernet0",
                    {{"NULL", "NULL"}}, m_config_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no STP_PORT_CONFIG from stpmgrd";
        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_PORT_CONFIG);
        auto *pmsg = reinterpret_cast<STP_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_STREQ(pmsg->intf_name, "PortChannel1");
        EXPECT_EQ(pmsg->enabled, 1);
        EXPECT_EQ(pmsg->bpdu_guard, 1);

        n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no membership message from stpmgrd";
        ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_VLAN_MEM_CONFIG);
        auto *mmsg = reinterpret_cast<STP_VLAN_MEM_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(mmsg->vlan_id, 10);
        EXPECT_STREQ(mmsg->intf_name, "PortChannel1");

        n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no STP_VLAN_PORT_CONFIG from stpmgrd";
        ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_VLAN_PORT_CONFIG);
        auto *vmsg = reinterpret_cast<STP_VLAN_PORT_CONFIG_MSG *>(ipc->data);
        EXPECT_EQ(vmsg->vlan_id, 10);
        EXPECT_STREQ(vmsg->intf_name, "PortChannel1");
        EXPECT_EQ(vmsg->path_cost, 300);
        EXPECT_EQ(vmsg->priority, 64);
        assertNoMoreIpc();
    }

    // vlanmgrd gives a priority_tagged member the PVID and sends it untagged,
    // so stpd has to see it as untagged too, or the kernel bridge loses the
    // PVID the first time the port blocks and unblocks.
    TEST_F(StpMgrTest, DoVlanMemUpdateTask_PriorityTaggedMemberIsUntagged)
    {
        enableMst();

        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|Ethernet0", {{"tagging_mode", "priority_tagged"}});

        applyAndRun(STATE_VLAN_MEMBER_TABLE_NAME, "Vlan10|Ethernet0",
                    {{"state", "ok"}}, m_state_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_VLAN_PORT_LIST_CONFIG);
        auto *msg = reinterpret_cast<STP_MST_VLAN_PORT_MAP *>(ipc->data);
        ASSERT_EQ(msg->port_count, 1);
        EXPECT_STREQ(msg->port_list[0].intf_name, "Ethernet0");
        EXPECT_EQ(msg->port_list[0].tagging_mode, UNTAGGED_MODE);
        assertNoMoreIpc();
    }

    // A member added with no tagging_mode is untagged in vlanmgrd, so stpd
    // has to get it as untagged instead of not at all.
    TEST_F(StpMgrTest, DoVlanMemUpdateTask_MemberWithoutTaggingModeIsUntagged)
    {
        enableMst();

        Table cfgVlanMember(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
        cfgVlanMember.set("Vlan10|Ethernet0", {{"NULL", "NULL"}});

        applyAndRun(STATE_VLAN_MEMBER_TABLE_NAME, "Vlan10|Ethernet0",
                    {{"state", "ok"}}, m_state_db);

        uint8_t buf[1024];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message received from stpmgrd";

        auto *ipc = reinterpret_cast<STP_IPC_MSG *>(buf);
        EXPECT_EQ(ipc->msg_type, STP_MST_VLAN_PORT_LIST_CONFIG);
        auto *msg = reinterpret_cast<STP_MST_VLAN_PORT_MAP *>(ipc->data);
        ASSERT_EQ(msg->port_count, 1);
        EXPECT_STREQ(msg->port_list[0].intf_name, "Ethernet0");
        EXPECT_EQ(msg->port_list[0].tagging_mode, UNTAGGED_MODE);
        assertNoMoreIpc();
    }

    // Bind a fresh fake stpd socket at STPD_SOCK_NAME and return its fd.
    static int bindFakeStpd()
    {
        unlink(STPD_SOCK_NAME);
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, STPD_SOCK_NAME, sizeof(addr.sun_path) - 1);
        if (fd >= 0 && bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        {
            close(fd);
            return -1;
        }
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return fd;
    }

    // stpd binds its socket a moment after it starts. A message sent before
    // that must be retried, not dropped.
    TEST_F(StpMgrTest, SendMsgStpd_RetriesUntilStpdBinds)
    {
        close(m_fakeStpdFd);
        m_fakeStpdFd = -1;
        unlink(STPD_SOCK_NAME);

        int lateFd = -1;
        std::thread late([&lateFd]() {
            usleep(300000);
            lateFd = bindFakeStpd();
        });

        STP_INIT_READY_MSG ready;
        memset(&ready, 0, sizeof(ready));
        ready.max_stp_instances = 255;
        int rc = m_stpMgr->sendMsgStpd(STP_INIT_READY, sizeof(ready), (void *)&ready);
        late.join();
        m_fakeStpdFd = lateFd;
        ASSERT_GE(m_fakeStpdFd, 0) << "late fake stpd bind failed";

        EXPECT_GT(rc, 0) << "the send was dropped instead of retried";
        uint8_t buf[256];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data)) << "no IPC message reached the late stpd";
        EXPECT_EQ(reinterpret_cast<STP_IPC_MSG *>(buf)->msg_type, STP_INIT_READY);
        assertNoMoreIpc();
    }

    // While stpd stays away the retry budget is spent once: the first send
    // retries for up to 2 s, the next one costs a single sendto, and a send
    // after stpd is back goes through at once.
    TEST_F(StpMgrTest, SendMsgStpd_SecondFailureCostsOneAttempt)
    {
        close(m_fakeStpdFd);
        m_fakeStpdFd = -1;
        unlink(STPD_SOCK_NAME);

        STP_INIT_READY_MSG ready;
        memset(&ready, 0, sizeof(ready));
        ready.max_stp_instances = 255;

        auto t0 = std::chrono::steady_clock::now();
        int rc = m_stpMgr->sendMsgStpd(STP_INIT_READY, sizeof(ready), (void *)&ready);
        auto first = std::chrono::steady_clock::now() - t0;
        EXPECT_EQ(rc, -1);
        EXPECT_GE(first, std::chrono::milliseconds(STPD_SEND_RETRIES * STPD_SEND_RETRY_US / 1000));

        t0 = std::chrono::steady_clock::now();
        rc = m_stpMgr->sendMsgStpd(STP_INIT_READY, sizeof(ready), (void *)&ready);
        auto second = std::chrono::steady_clock::now() - t0;
        EXPECT_EQ(rc, -1);
        EXPECT_LT(second, std::chrono::milliseconds(STPD_SEND_RETRY_US / 1000))
            << "a send inside the backoff window still retried";

        m_fakeStpdFd = bindFakeStpd();
        ASSERT_GE(m_fakeStpdFd, 0);
        rc = m_stpMgr->sendMsgStpd(STP_INIT_READY, sizeof(ready), (void *)&ready);
        EXPECT_GT(rc, 0) << "stpd is back but the send was not delivered";
        uint8_t buf[256];
        ssize_t n = recvIpc(buf, sizeof(buf));
        ASSERT_GT(n, (ssize_t)offsetof(STP_IPC_MSG, data));
        EXPECT_EQ(reinterpret_cast<STP_IPC_MSG *>(buf)->msg_type, STP_INIT_READY);
        assertNoMoreIpc();
    }

    // A socket file left behind by an earlier stpd does not count as ready;
    // only a bound socket does.
    TEST_F(StpMgrTest, WaitStpdReady_IgnoresStaleSocketFile)
    {
        close(m_fakeStpdFd);    // the path stays, nothing is bound to it
        m_fakeStpdFd = -1;
        EXPECT_FALSE(m_stpMgr->waitStpdReady(300));

        m_fakeStpdFd = bindFakeStpd();
        ASSERT_GE(m_fakeStpdFd, 0);
        EXPECT_TRUE(m_stpMgr->waitStpdReady(300));
    }
}
