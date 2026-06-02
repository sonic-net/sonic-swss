#ifndef __BFDLINK__
#define __BFDLINK__

#include <arpa/inet.h>
#include <sys/socket.h>

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <exception>
#include <map>
#include <memory>
#include <string.h>

#include "selectable.h"
#include "bfdd/bfddp_packet.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "table.h"

// if name in bfddp_session struct: char ifname[64];
#define IFNAME_LEN 64
#define BFD_MAX_MSG_LEN 256

typedef bfddp_message_header bfd_msg_hdr_t;

#define BFD_MSG_HDR_LEN (sizeof (bfd_msg_hdr_t))


/*
 * bfd_msg_len
 */
static inline size_t
bfd_msg_len (const bfd_msg_hdr_t *hdr)
{
  //return ntohs (hdr->msg_len);
  return ntohs (hdr->length);
}

/*
 * bfd_msg_ok
 *
 * Returns TRUE if a message looks well-formed.
 *
 * @param len The length in bytes from 'hdr' to the end of the buffer.
 */
static inline int
bfd_msg_ok (const bfd_msg_hdr_t *hdr)
{
  size_t msg_len;

  if ((ntohs(hdr->type) != DP_ADD_SESSION) && (ntohs(hdr->type) != DP_DELETE_SESSION) && (ntohs(hdr->type) != DP_REQUEST_SESSION_COUNTERS))
  {
      return 0;
  }

  msg_len = bfd_msg_len (hdr);
  if (msg_len < BFD_MSG_HDR_LEN || msg_len > BFD_MAX_MSG_LEN)
    return 0;

  return 1;
}

using namespace std;

/* Forward declaration of the gtest fixture class at global scope so the
 * friend declaration below in `swss::BfdLink` resolves to ::BfdSyncdTest
 * (defined in tests/mock_tests/bfdsyncd/test_bfdlink.cpp) rather than to
 * an implicit forward decl scoped to namespace swss. */
class BfdSyncdTest;

namespace swss {

class BfdLink : public Selectable {
public:
    BfdLink(DBConnector *db, DBConnector *stateDb, unsigned short port = BFD_DATA_PLANE_DEFAULT_PORT, int debug = 0);
    virtual ~BfdLink();

    virtual std::string get_intf_mac(const char* intf);
    virtual bool sendmsg(uint16_t msglen);

    /* Wait for connection (blocking) */
    void accept();

    int getFd() override;
    uint64_t readData() override;
    void bfdDebugMessage(struct bfddp_message *bm);
    void handleBfdDpMessage(size_t start);
    void hexdump(void *ptr, int buflen);

    /* readMe throws BfdConnectionClosedException when connection is lost */
    class BfdConnectionClosedException : public std::exception
    {
    };
    bool handleBfdStateUpdate(std::string key, const std::vector<swss::FieldValueTuple> &fvs);
    void bfdStateUpdate(std::string key);

private:
    /* The unit-test fixture writes raw wire bytes into m_messageBuffer
     * directly so it can drive handleBfdDpMessage() without a real TCP
     * peer. The friendship is the standard swss pattern for granting
     * gtest fixtures access to private members; access is funnelled
     * through a single helper method on BfdSyncdTest rather than
     * touching m_messageBuffer all over the test body. */
    friend class ::BfdSyncdTest;

    /* Reception buffer for inbound BFD DP messages from FRR's bfdd.
     * Allocated in the ctor (m_bufSize bytes), filled by readData() via
     * ::read() on the TCP socket, parsed by handleBfdDpMessage().
     * Declared first to keep init-list order stable when other private
     * members are added later. */
    char *m_messageBuffer;

    /* bfd table */
    ProducerStateTable m_bfdTable;
    Table m_bfdStateTable;

    unsigned int m_bufSize;
    char *m_sendBuffer;
    char m_printbuf[1024];
    unsigned int m_pos;
    std::map<std::string, struct bfddp_message> m_key2bfd;

    /* Reverse lookup from bfdd's local discriminator (host byte order)
     * to the APP_BFD_SESSION_TABLE key (colon-separated form, e.g.
     * "default:10.0.0.5"). Populated on DP_ADD_SESSION, removed on
     * DP_DELETE_SESSION. Used by DP_REQUEST_SESSION_COUNTERS to map
     * the requested lid back to the COUNTERS_BFD_SESSION_NAME_MAP key
     * BfdOrch publishes for the same session. m_key2bfd already exists
     * but is keyed on the pipe-separated STATE_DB form, which is the
     * wrong shape for the counters name-map. */
    std::map<uint32_t, std::string> m_lid2appkey;

    /* COUNTERS_DB plumbing for DP_REQUEST_SESSION_COUNTERS. BfdOrch's
     * BFD_SESSION_STAT_COUNTER FLEX_COUNTER group polls SAI BFD-session
     * stats every 10s into COUNTERS:<sai_oid>; COUNTERS_BFD_SESSION_NAME_MAP
     * maps APP_DB session keys to that SAI OID. Synchronous read on
     * demand — no SUBSCRIBE needed because bfdd polls us, not the other
     * way around. unique_ptr because COUNTERS_DB is a runtime concern
     * (the BFD_SESSION_STAT counter group may or may not be enabled by
     * the operator); deferring construction lets us tolerate Redis
     * not being reachable for the counters DB without breaking the
     * primary APP/STATE_DB paths. */
    std::unique_ptr<DBConnector> m_countersDb;
    std::unique_ptr<Table> m_bfdSessionNameMap;
    std::unique_ptr<Table> m_bfdCountersTable;

    bool m_connected;
    bool m_server_up;
    int m_debug;
    int m_server_socket;
    int m_connection_socket;
};

}

#endif
