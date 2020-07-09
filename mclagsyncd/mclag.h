/* Copyright(c) 2016-2019 Nephos.
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along with
* this program; if not, see <http://www.gnu.org/licenses/>.
*
* The full GNU General Public License is included in this distribution in
* the file called "COPYING".
*
*  Maintainer: Jim Jiang from nephos
*/

#ifndef _MCLAG_H
#define _MCLAG_H
#define MCLAG_DEFAULT_IP   0x7f000006

enum MCLAG_FDB_OP_TYPE {
    MCLAG_FDB_OPER_ADD = 1,
    MCLAG_FDB_OPER_DEL = 2,
};

enum MCLAG_FDB_TYPE {
    MCLAG_FDB_TYPE_STATIC  = 1,
    MCLAG_FDB_TYPE_DYNAMIC = 2,
    MCLAG_FDB_TYPE_DYNAMIC_LOCAL = 3, /* Used while sending MAC to Syncd to program with aging enabled. */
};

enum MCLAG_DOMAIN_CFG_OP_TYPE {
    MCLAG_CFG_OPER_NONE       = 0,  //NOP
    MCLAG_CFG_OPER_ADD        = 1,  //Add domain
    MCLAG_CFG_OPER_DEL        = 2,  //Delete domain
    MCLAG_CFG_OPER_UPDATE     = 3,   //update domain
    MCLAG_CFG_OPER_ATTR_DEL   = 4   //Attribute del 
};


enum MCLAG_DOMAIN_CFG_ATTR_BMAP_FLAGS {
    MCLAG_CFG_ATTR_NONE                  = 0x0,  
    MCLAG_CFG_ATTR_SRC_ADDR              = 0x1,  
    MCLAG_CFG_ATTR_PEER_ADDR             = 0x2,  
    MCLAG_CFG_ATTR_PEER_LINK             = 0x4,   
    MCLAG_CFG_ATTR_KEEPALIVE_INTERVAL    = 0x8,   
    MCLAG_CFG_ATTR_SESSION_TIMEOUT       = 0x10   
};

/*
 * default port for mclag connections
 */
#define MCLAG_DEFAULT_PORT 2626

/*
 * Largest message that can be sent to or received from the MCLAG.
 */
#define MCLAG_MAX_MSG_LEN 4096
#define MCLAG_MAX_SEND_MSG_LEN 4096

typedef struct mclag_msg_hdr_t_ {
    /*
     * Protocol version.
     */
    uint8_t version;

    /*
     * Type of message, see below.
     */
    uint8_t msg_type;

    /*
     * Length of entire message, including the header.
     */
    uint16_t msg_len;
}mclag_msg_hdr_t;

#define MCLAG_PROTO_VERSION 1
#define MCLAG_MSG_HDR_LEN (sizeof (mclag_msg_hdr_t))

/*syncd send msg type to iccpd*/
typedef enum mclag_syncd_msg_type_e_ {
    MCLAG_SYNCD_MSG_TYPE_NONE = 0,
    MCLAG_SYNCD_MSG_TYPE_FDB_OPERATION = 1,
    MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_DOMAIN = 2,
    MCLAG_SYNCD_MSG_TYPE_CFG_MCLAG_IFACE  = 3,
    MCLAG_SYNCD_MSG_TYPE_VLAN_MBR_UPDATES  = 4
}mclag_syncd_msg_type_e;

/*iccpd send msg type to syncd*/
typedef enum mclag_msg_type_e_ {
    MCLAG_MSG_TYPE_NONE = 0,
    MCLAG_MSG_TYPE_PORT_ISOLATE = 1,
    MCLAG_MSG_TYPE_PORT_MAC_LEARN_MODE = 2,
    MCLAG_MSG_TYPE_FLUSH_FDB = 3,
    MCLAG_MSG_TYPE_SET_INTF_MAC = 4,
    MCLAG_MSG_TYPE_SET_FDB = 5,
    MCLAG_MSG_TYPE_SET_TRAFFIC_DIST_ENABLE = 6,
    MCLAG_MSG_TYPE_SET_TRAFFIC_DIST_DISABLE = 7,
    MCLAG_MSG_TYPE_SET_ICCP_STATE           = 8,
    MCLAG_MSG_TYPE_SET_ICCP_ROLE            = 9,
    MCLAG_MSG_TYPE_SET_ICCP_SYSTEM_ID       = 10,
    MCLAG_MSG_TYPE_DEL_ICCP_INFO            = 11,
    MCLAG_MSG_TYPE_SET_REMOTE_IF_STATE      = 12,
    MCLAG_MSG_TYPE_DEL_REMOTE_IF_INFO       = 13,
    MCLAG_MSG_TYPE_SET_PEER_LINK_ISOLATION  = 14,
    MCLAG_MSG_TYPE_SET_ICCP_PEER_SYSTEM_ID  = 15,
    MCLAG_MSG_TYPE_FLUSH_FDB_BY_PORT = 20
}mclag_msg_type_e;

typedef struct mclag_sub_option_hdr_t_ {    
    uint8_t op_type;

    /*
     * Length of option value, not including the header.
     */
    uint16_t op_len;
    uint8_t data[];
}mclag_sub_option_hdr_t;

#define MCLAG_SUB_OPTION_HDR_LEN (sizeof (mclag_sub_option_hdr_t))

typedef enum mclag_sub_option_type_e_ {
    MCLAG_SUB_OPTION_TYPE_NONE = 0,
    MCLAG_SUB_OPTION_TYPE_ISOLATE_SRC = 1,
    MCLAG_SUB_OPTION_TYPE_ISOLATE_DST = 2,
    MCLAG_SUB_OPTION_TYPE_MAC_LEARN_ENABLE = 3,
    MCLAG_SUB_OPTION_TYPE_MAC_LEARN_DISABLE = 4,
    MCLAG_SUB_OPTION_TYPE_SET_MAC_SRC = 5,
    MCLAG_SUB_OPTION_TYPE_SET_MAC_DST = 6,
    MCLAG_SUB_OPTION_TYPE_MCLAG_INTF_NAME = 7,
    MCLAG_SUB_OPTION_TYPE_MCLAG_ID        = 8,
    MCLAG_SUB_OPTION_TYPE_ICCP_ROLE       = 9,
    MCLAG_SUB_OPTION_TYPE_SYSTEM_ID       = 10,
    MCLAG_SUB_OPTION_TYPE_OPER_STATUS     = 11,
    MCLAG_SUB_OPTION_TYPE_ISOLATION_STATE = 12,
    MCLAG_SUB_OPTION_TYPE_PEER_SYSTEM_ID  = 13
} mclag_sub_option_type_e;

static inline size_t
mclag_msg_len (const mclag_msg_hdr_t *hdr)
{
    return hdr->msg_len;
}

/*
 * mclag_msg_data_len
 */
static inline size_t
mclag_msg_data_len (const mclag_msg_hdr_t *hdr)
{
  return (mclag_msg_len (hdr) - MCLAG_MSG_HDR_LEN);
}

/*
 * mclag_msg_hdr_ok
 *
 * Returns TRUE if a message header looks well-formed.
 */
static inline int
mclag_msg_hdr_ok (const mclag_msg_hdr_t *hdr)
{
  size_t msg_len;

  if (hdr->msg_type == MCLAG_MSG_TYPE_NONE)
    return 0;

  msg_len = mclag_msg_len (hdr);

  if (msg_len < MCLAG_MSG_HDR_LEN || msg_len > MCLAG_MAX_MSG_LEN)
    return 0;

  return 1;
}

/*
 * mclag_msg_ok
 *
 * Returns TRUE if a message looks well-formed.
 *
 * @param len The length in bytes from 'hdr' to the end of the buffer.
 */
static inline int
mclag_msg_ok (const mclag_msg_hdr_t *hdr, size_t len)
{
  if (len < MCLAG_MSG_HDR_LEN)
    return 0;

  if (!mclag_msg_hdr_ok (hdr))
    return 0;

  if (mclag_msg_len (hdr) > len)
    return 0;

  return 1;
}


#endif

