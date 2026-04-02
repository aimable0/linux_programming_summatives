/*
 * protocol.h
 * ==========================================================================
 * Shared message protocol for the Digital Library Reservation Platform.
 *
 * WIRE FORMAT
 * -----------
 * Every message on the TCP byte stream has the structure:
 *
 *    +--------+----------+------------------+
 *    | type   | length   | payload          |
 *    | 1 byte | 4 bytes  | 'length' bytes   |
 *    |        | big-end  |                  |
 *    +--------+----------+------------------+
 *
 * Using a length-prefix framing scheme (as opposed to delimiter-based)
 * guarantees that arbitrary binary payloads are supported and that a
 * single recv() call never ambiguously spans two logical messages.
 * The 5-byte header is minimal overhead for this application.
 *
 * PAYLOAD ENCODING
 * ----------------
 * All payloads are UTF-8 text (null-terminated in the struct, but the
 * null byte is NOT included in the transmitted 'length' field).
 *
 * Book list payload (MSG_BOOK_LIST):
 *   Line 1:  "<count>\n"
 *   Lines 2+: "<index>|<title>|<status>\n"
 *             status is either "available" or "reserved:<LIB_ID>"
 *
 * Reservation request payload (MSG_RESERVE_REQ):
 *   "<book_index>\n"    (1-based integer as ASCII)
 *
 * All other payloads are human-readable status strings.
 * ==========================================================================
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

/* ── Network constants ────────────────────────────────────────────────────── */
#define SERVER_PORT        8080
#define SERVER_BACKLOG     10
#define MAX_CLIENTS        5

/* ── Sizing constants ─────────────────────────────────────────────────────── */
#define MAX_BOOKS          8
#define MAX_LIB_ID_LEN     16      /* e.g. "LIB001\0"                        */
#define MAX_TITLE_LEN      80
#define MAX_PAYLOAD_LEN    4096    /* large enough for full book list          */
#define LOG_FILENAME       "server.log"
#define BOOKS_FILENAME     "books.dat"

/* ── Session timeout ──────────────────────────────────────────────────────── */
#define CLIENT_TIMEOUT_SEC 120     /* idle client disconnected after 2 min    */

/* ── Message type enumeration ────────────────────────────────────────────────
 * Using an enum (rather than bare #define integers) gives the compiler
 * type information and makes switch() exhaustiveness warnings possible.
 * ────────────────────────────────────────────────────────────────────────── */
typedef enum {
    MSG_AUTH_REQ     = 0x01,  /* client → server : "LIB_ID\n"               */
    MSG_AUTH_OK      = 0x02,  /* server → client : welcome text              */
    MSG_AUTH_FAIL    = 0x03,  /* server → client : rejection reason          */
    MSG_BOOK_LIST    = 0x04,  /* server → client : serialised catalogue      */
    MSG_RESERVE_REQ  = 0x05,  /* client → server : "book_index\n"            */
    MSG_RESERVE_OK   = 0x06,  /* server → client : confirmation text         */
    MSG_RESERVE_FAIL = 0x07,  /* server → client : rejection reason          */
    MSG_DISCONNECT   = 0x08,  /* either direction: clean session termination  */
    MSG_SERVER_FULL  = 0x09,  /* server → client : capacity exceeded         */
    MSG_SERVER_INFO  = 0x0A,  /* server → client : informational text        */
} MsgType;

/* ── In-memory message representation ────────────────────────────────────── */
typedef struct {
    uint8_t  type;
    uint32_t length;                    /* payload byte count, host order     */
    char     payload[MAX_PAYLOAD_LEN];  /* null-terminated in memory only     */
} Message;


/* ==========================================================================
 * send_all / recv_all
 * --------------------------------------------------------------------------
 * TCP is a STREAM protocol — a single send() may transmit fewer bytes than
 * requested, and a single recv() may return fewer bytes than available.
 * These helpers loop until the exact requested count is transferred.
 *
 * Return value:
 *   send_all:  (ssize_t)n on success,  -1 on error
 *   recv_all:   n on success,  0 if peer closed,  -1 on error
 * ========================================================================== */
static inline ssize_t send_all(int fd, const void *buf, size_t n)
{
    size_t      sent = 0;
    const char *ptr  = (const char *)buf;
    while (sent < n) {
        ssize_t s = send(fd, ptr + sent, n - sent, MSG_NOSIGNAL);
        if (s < 0) return -1;
        sent += (size_t)s;
    }
    return (ssize_t)sent;
}

static inline ssize_t recv_all(int fd, void *buf, size_t n)
{
    size_t  got = 0;
    char   *ptr = (char *)buf;
    while (got < n) {
        ssize_t r = recv(fd, ptr + got, n - got, 0);
        if (r == 0) return 0;   /* peer disconnected cleanly                 */
        if (r <  0) return -1;  /* error (check errno: EAGAIN = timeout)     */
        got += (size_t)r;
    }
    return (ssize_t)got;
}


/* ==========================================================================
 * send_msg / recv_msg
 * --------------------------------------------------------------------------
 * Frame a message onto / off the TCP stream.
 *
 * send_msg  return: 0 = success, -1 = error
 * recv_msg  return: 1 = success, 0 = disconnected, -1 = error/timeout
 * ========================================================================== */
static inline int send_msg(int fd, MsgType type, const char *payload)
{
    uint8_t  t       = (uint8_t)type;
    uint32_t len     = payload ? (uint32_t)strlen(payload) : 0;
    uint32_t net_len = htonl(len);

    if (send_all(fd, &t,       1) < 0) return -1;
    if (send_all(fd, &net_len, 4) < 0) return -1;
    if (len > 0 && send_all(fd, payload, len) < 0) return -1;
    return 0;
}

static inline int recv_msg(int fd, Message *msg)
{
    uint8_t  t;
    uint32_t net_len;

    ssize_t r = recv_all(fd, &t, 1);
    if (r <= 0) return (int)r;

    r = recv_all(fd, &net_len, 4);
    if (r <= 0) return (int)r;

    msg->type   = t;
    msg->length = ntohl(net_len);

    /* Guard against oversized payloads (protocol error or malicious client) */
    if (msg->length >= MAX_PAYLOAD_LEN) return -1;

    if (msg->length > 0) {
        r = recv_all(fd, msg->payload, msg->length);
        if (r <= 0) return (int)r;
    }
    msg->payload[msg->length] = '\0';   /* ensure null-terminated             */
    return 1;
}

#endif /* PROTOCOL_H */

