// SPDX-License-Identifier: MIT
// IPv4 sockets for TCP and UDP over the console's network stack (IOS's, through libogc) or the
// host's: the same calls and the same results on both, so networking code runs in host tests.
//
// What the console's stack asks of a program, and this layer does for it:
//   - The network comes up in the background (DHCP can take seconds): wl_net_start returns at
//     once, wl_net_state / wl_net_wait say when it is up. Until then every socket call returns
//     WL_ERR_NET_DOWN. The host backend is always up.
//   - Addresses are WlNetAddr: IPv4 and port in host byte order, whatever the layout of the
//     system's own socket address (lwIP's has a length byte; Winsock's does not).
//   - Errors come back as results, not as errno: IOS's codes (libogc converts them to newlib's
//     errno numbers) and the host's become the same WL_ERR_* value. `wl_net_last_errno` has the
//     raw number for a log line.
//   - IOS has no getpeername and a small socket table (about 24); select is poll (wl_net_poll).
//
// Every call that takes a socket may be given a blocking or a non-blocking one
// (wl_net_set_nonblocking): a non-blocking call that cannot finish returns WL_ERR_AGAIN (connect:
// WL_ERR_NET_INPROGRESS). Counts are returned as a non-negative int32_t, failures as a negative
// WlResult. The functions are safe to call from several threads on different sockets.
#pragma once

#include "wl/base/base.h"

// ---- results (the platform block of base.h: WL_ERR_BASE_PLATFORM - n) --------------------------

#define WL_ERR_NET_DOWN (WL_ERR_BASE_PLATFORM - 1)        // the network is not up (yet, or failed)
#define WL_ERR_NET_UNREACH (WL_ERR_BASE_PLATFORM - 2)     // no route to the network or host
#define WL_ERR_NET_REFUSED (WL_ERR_BASE_PLATFORM - 3)     // connection refused
#define WL_ERR_NET_RESET (WL_ERR_BASE_PLATFORM - 4)       // connection reset or aborted by the peer
#define WL_ERR_NET_TIMEDOUT (WL_ERR_BASE_PLATFORM - 5)    // the connection or send timed out
#define WL_ERR_NET_NOTCONN (WL_ERR_BASE_PLATFORM - 6)     // the socket is not connected
#define WL_ERR_NET_ISCONN (WL_ERR_BASE_PLATFORM - 7)      // the socket is already connected
#define WL_ERR_NET_INPROGRESS (WL_ERR_BASE_PLATFORM - 8)  // a non-blocking connect has started
#define WL_ERR_NET_ALREADY (WL_ERR_BASE_PLATFORM - 9)     // a non-blocking connect is still pending
#define WL_ERR_NET_ADDRINUSE (WL_ERR_BASE_PLATFORM - 10)  // the local address or port is taken
#define WL_ERR_NET_ADDRNOTAVAIL (WL_ERR_BASE_PLATFORM - 11) // no such local address
#define WL_ERR_NET_MSGSIZE (WL_ERR_BASE_PLATFORM - 12)    // the datagram is larger than the stack takes
#define WL_ERR_NET_ACCESS (WL_ERR_BASE_PLATFORM - 13)     // not permitted (a broadcast without the option, ...)
#define WL_ERR_NET_NOTSOCK (WL_ERR_BASE_PLATFORM - 14)    // not an open socket

// Static name of a network result ("WL_ERR_NET_REFUSED"), or wl_result_name's for the others.
const char *wl_net_result_name(WlResult result);

// The raw errno-style number of the last failing call (newlib's numbering; the host's own numbers
// differ for a few). Any thread's: it is for log lines, decide on the returned result instead.
int wl_net_last_errno(void);

// ---- the network ------------------------------------------------------------------------------

typedef enum {
    WL_NET_IDLE = 0,  // wl_net_start has not been called (or wl_net_stop has)
    WL_NET_STARTING,  // coming up
    WL_NET_UP,
    WL_NET_FAILED     // it could not come up (no cable, no access point, no DHCP answer)
} WlNetState;

// Starts the network in the background and returns at once. Idempotent while starting or up; after
// a failure it tries again. Needs IOS to be running the network module: do it after
// wl_platform_fs_ios_init, which may reload IOS.
WlResult wl_net_start(void);
WlNetState wl_net_state(void);
// Waits until the network is up or has failed, or `timeout_ms` has passed (0: just look; any value
// in the range of uint32_t, UINT32_MAX for as long as it takes). WL_OK when up, WL_ERR_AGAIN on a
// timeout, WL_ERR_NET_DOWN when it failed, WL_ERR_STATE when it was never started.
WlResult wl_net_wait(uint32_t timeout_ms);
// Closes the network down; open sockets become invalid.
void wl_net_stop(void);

// ---- addresses --------------------------------------------------------------------------------

// An IPv4 address (0xAABBCCDD for AA.BB.CC.DD) and a port, both in host byte order.
typedef struct {
    uint32_t address;
    uint16_t port;
} WlNetAddr;

#define WL_NET_ADDR_ANY 0u
#define WL_NET_ADDR_BROADCAST 0xffffffffu
#define WL_NET_ADDR(a, b, c, d) \
    ((((uint32_t)(a) & 0xffu) << 24) | (((uint32_t)(b) & 0xffu) << 16) | (((uint32_t)(c) & 0xffu) << 8) | ((uint32_t)(d) & 0xffu))

// This machine's address on the network (0 when it is not up).
uint32_t wl_net_local_address(void);

// "192.168.0.7" to an address: four decimal fields, each 0..255, nothing else (no hex or octal
// forms, no shorter forms, no spaces). WL_ERR_FORMAT otherwise.
WlResult wl_net_parse_address(const char *text, uint32_t *out);
// An address as dotted decimal in `out` (at least 16 bytes: "255.255.255.255" and the NUL).
void wl_net_format_address(uint32_t address, char *out);
// A host name or dotted address to an address. A name is looked up with the system's resolver and
// blocks until the answer comes (seconds, on the console); WL_ERR_NOT_FOUND when there is none,
// WL_ERR_NET_DOWN when the network is not up. Not for the main loop.
WlResult wl_net_resolve(const char *host, uint32_t *out);

// ---- sockets ----------------------------------------------------------------------------------

typedef int32_t WlSocket;
#define WL_SOCKET_INVALID (-1)

typedef enum { WL_NET_UDP = 0, WL_NET_TCP } WlNetProto;

WlResult wl_net_socket(WlNetProto proto, WlSocket *out);
WlResult wl_net_close(WlSocket socket);
// What an open socket was made as (the stack has no way to ask: IOS's has no SO_TYPE; this layer
// keeps it for the first 64 socket numbers, which cover all the console can open). Sockets from
// wl_net_accept are TCP. WL_ERR_NET_NOTSOCK for one not opened here.
WlResult wl_net_socket_proto(WlSocket socket, WlNetProto *out);

// The address a socket is bound to (port 0 binds a free one: wl_net_local_addr says which).
WlResult wl_net_bind(WlSocket socket, const WlNetAddr *address);
WlResult wl_net_local_addr(WlSocket socket, WlNetAddr *out);
WlResult wl_net_connect(WlSocket socket, const WlNetAddr *address);
WlResult wl_net_listen(WlSocket socket, int32_t backlog);
// Takes a connection from a listening socket: the new socket (blocking, whatever the listener was)
// and the peer's address (`peer` may be NULL). WL_ERR_AGAIN on a non-blocking listener with none
// waiting. The console's stack has no getpeername: keep the address from here or from connect.
WlResult wl_net_accept(WlSocket socket, WlSocket *out, WlNetAddr *peer);

// TCP (or connected UDP): the bytes sent or received; recv returns 0 when the peer closed.
int32_t wl_net_send(WlSocket socket, const void *data, uint32_t size);
int32_t wl_net_recv(WlSocket socket, void *buffer, uint32_t size);
// UDP: one datagram to an address, or the next one from anyone (its sender in `from`, if not NULL).
// A datagram longer than `size` is cut to it.
int32_t wl_net_sendto(WlSocket socket, const void *data, uint32_t size, const WlNetAddr *to);
int32_t wl_net_recvfrom(WlSocket socket, void *buffer, uint32_t size, WlNetAddr *from);

typedef enum { WL_NET_SHUT_READ = 0, WL_NET_SHUT_WRITE, WL_NET_SHUT_BOTH } WlNetShutdown;
WlResult wl_net_shutdown(WlSocket socket, WlNetShutdown how);

WlResult wl_net_set_nonblocking(WlSocket socket, bool nonblocking);
// Bytes that can be read now: in a stream, all of them; of a datagram socket, the next datagram's.
// Not on the console: libogc's net_ioctl answers only FIONBIO, so this returns WL_ERR_UNSUPPORTED
// there (read with wl_net_recv / wl_net_recvfrom into a buffer of the largest size you accept).
WlResult wl_net_bytes_available(WlSocket socket, uint32_t *out);

typedef enum {
    WL_NET_OPT_NODELAY = 0, // TCP: send small writes at once; value 0 or 1
    WL_NET_OPT_BROADCAST,   // UDP: may send to the broadcast address; 0 or 1
    WL_NET_OPT_REUSEADDR,   // bind to a port that was lately in use; 0 or 1
    WL_NET_OPT_SNDBUF,      // bytes of send buffer
    WL_NET_OPT_RCVBUF       // bytes of receive buffer
} WlNetOption;
WlResult wl_net_set_option(WlSocket socket, WlNetOption option, int32_t value);
// How a non-blocking connect ended, once the socket polls writable: WL_OK in `out` when it is
// connected, otherwise the result of its failure (WL_ERR_NET_REFUSED, ...).
WlResult wl_net_connect_result(WlSocket socket, WlResult *out);

// ---- waiting for several sockets (select) -----------------------------------------------------

#define WL_NET_POLL_READ 0x01u  // in: data (or a connection to accept, or the peer's close) can be read
#define WL_NET_POLL_WRITE 0x02u // in: a send would not block (a connect has finished)
#define WL_NET_POLL_PRIORITY 0x04u // in: out-of-band data
#define WL_NET_POLL_ERROR 0x08u // out: the socket has an error (wl_net_connect_result says which)
#define WL_NET_POLL_HUP 0x10u   // out: the connection has been closed
#define WL_NET_POLL_MAX_SOCKETS 64u

typedef struct {
    WlSocket socket;
    uint32_t events;  // WL_NET_POLL_READ | WRITE | PRIORITY
    uint32_t revents; // set by the call; ERROR and HUP are reported whether asked for or not
} WlNetPoll;

// Waits until a socket in `items` is ready, `timeout_ms` has passed (0: just look) or, with
// WL_NET_TIMEOUT_FOREVER, for ever. Returns the number of items with a non-zero `revents`
// (0 on a timeout), or a negative result. At most WL_NET_POLL_MAX_SOCKETS items.
#define WL_NET_TIMEOUT_FOREVER (-1)
int32_t wl_net_poll(WlNetPoll *items, uint32_t count, int32_t timeout_ms);
