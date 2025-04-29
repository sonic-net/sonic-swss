#ifndef __BFDLINK__
#define __BFDLINK__

#include <arpa/inet.h>
#include <sys/socket.h>

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <exception>
#include <map>
#include <string.h>

#include "selectable.h"
#include "bfdd/bfddp_packet.h"
#include "dbconnector.h"
#include "producerstatetable.h"

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

namespace swss {

class BfdLink : public Selectable {
public:
    BfdLink(DBConnector *db, DBConnector *stateDb, unsigned short port = BFD_DATA_PLANE_DEFAULT_PORT, int debug = 0);
    virtual ~BfdLink();

    virtual std::string exec(const char* cmd);
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
    char *m_messageBuffer;

private:
    /* bfd table */
    ProducerStateTable m_bfdTable;
    Table m_bfdStateTable;

    unsigned int m_bufSize;
    char *m_sendBuffer;
    char m_printbuf[1024];
    unsigned int m_pos;
    std::map<std::string, struct bfddp_message> m_key2bfd;

    bool m_connected;
    bool m_server_up;
    int m_debug;
    int m_server_socket;
    int m_connection_socket;
};

}

#endif
