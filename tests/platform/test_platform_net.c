// SPDX-License-Identifier: MIT
// net.h on the host backend: the address text, the error mapping, and TCP and UDP over loopback
// (byte order and word size of the ppc-be build included). The on-console runner checks the Wii's
// backend the same way when the network is up (tests/console/console_runner.c).
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "wl/platform/net.h"
#include "wl/platform/net_internal.h"
#include "wl/platform/platform.h"

// ---- addresses ---------------------------------------------------------------------------------

TEST address_text_round_trips(void) {
    uint32_t address = 0;
    char text[16];

    ASSERT_EQ(WL_OK, wl_net_parse_address("192.168.0.7", &address));
    ASSERT_EQ(WL_NET_ADDR(192, 168, 0, 7), address);
    ASSERT_EQ(0xc0a80007u, address);
    wl_net_format_address(address, text);
    ASSERT_STR_EQ("192.168.0.7", text);
    wl_net_format_address(WL_NET_ADDR_BROADCAST, text);
    ASSERT_STR_EQ("255.255.255.255", text);
    wl_net_format_address(0, text);
    ASSERT_STR_EQ("0.0.0.0", text);
    ASSERT_EQ(WL_OK, wl_net_parse_address("0.0.0.0", &address));
    ASSERT_EQ(0u, address);
    ASSERT_EQ(WL_OK, wl_net_parse_address("255.255.255.255", &address));
    ASSERT_EQ(0xffffffffu, address);
    PASS();
}

TEST address_text_refuses_what_is_not_dotted_decimal(void) {
    static const char *const bad[] = {"",         "1.2.3",       "1.2.3.4.5", "1.2.3.256", "1.2.3.-4", "1..2.3",
                                      "1.2.3.4 ", " 1.2.3.4",    "a.b.c.d",   "0x7f.0.0.1", "127.1",   "01.2.3.4",
                                      "1.2.3.04", "1.2.3.1000", "1.2.3.4x",  "1.2.3.",    ".1.2.3"};
    uint32_t address = 99;

    for (size_t i = 0; i < WL_ARRAY_COUNT(bad); i++) {
        ASSERT_EQ_FMT(WL_ERR_FORMAT, wl_net_parse_address(bad[i], &address), "%d");
        ASSERT_EQ(99u, address); // untouched on failure
    }
    ASSERT_EQ(WL_ERR_ARG, wl_net_parse_address(NULL, &address));
    ASSERT_EQ(WL_ERR_ARG, wl_net_parse_address("1.2.3.4", NULL));
    PASS();
}

TEST resolve_takes_dotted_addresses_and_names(void) {
    uint32_t address = 0;

    ASSERT_EQ(WL_OK, wl_net_resolve("10.1.2.3", &address));
    ASSERT_EQ(WL_NET_ADDR(10, 1, 2, 3), address);
#if defined(__powerpc__)
    ASSERT_EQ(WL_ERR_UNSUPPORTED, wl_net_resolve("localhost", &address)); // (the static test build)
#else
    ASSERT_EQ(WL_OK, wl_net_resolve("localhost", &address));
    ASSERT_EQ(WL_NET_ADDR(127, 0, 0, 1), address);
#endif
    ASSERT_EQ(WL_ERR_ARG, wl_net_resolve(NULL, &address));
    // (a name nobody answers for is WL_ERR_NOT_FOUND, but asking needs a resolver: not tested here)
    PASS();
}

// ---- errors ------------------------------------------------------------------------------------

TEST errno_values_map_to_results(void) {
    ASSERT_EQ(WL_ERR_AGAIN, wl_net_result_from_errno(EAGAIN));
    ASSERT_EQ(WL_ERR_AGAIN, wl_net_result_from_errno(EWOULDBLOCK));
    ASSERT_EQ(WL_ERR_AGAIN, wl_net_result_from_errno(EINTR));
    ASSERT_EQ(WL_ERR_NET_DOWN, wl_net_result_from_errno(ENETDOWN));
    ASSERT_EQ(WL_ERR_NET_UNREACH, wl_net_result_from_errno(EHOSTUNREACH));
    ASSERT_EQ(WL_ERR_NET_REFUSED, wl_net_result_from_errno(ECONNREFUSED));
    ASSERT_EQ(WL_ERR_NET_RESET, wl_net_result_from_errno(ECONNRESET));
    ASSERT_EQ(WL_ERR_NET_RESET, wl_net_result_from_errno(EPIPE));
    ASSERT_EQ(WL_ERR_NET_TIMEDOUT, wl_net_result_from_errno(ETIMEDOUT));
    ASSERT_EQ(WL_ERR_NET_NOTCONN, wl_net_result_from_errno(ENOTCONN));
    ASSERT_EQ(WL_ERR_NET_ISCONN, wl_net_result_from_errno(EISCONN));
    ASSERT_EQ(WL_ERR_NET_INPROGRESS, wl_net_result_from_errno(EINPROGRESS));
    ASSERT_EQ(WL_ERR_NET_ALREADY, wl_net_result_from_errno(EALREADY));
    ASSERT_EQ(WL_ERR_NET_ADDRINUSE, wl_net_result_from_errno(EADDRINUSE));
    ASSERT_EQ(WL_ERR_NET_ADDRNOTAVAIL, wl_net_result_from_errno(EADDRNOTAVAIL));
    ASSERT_EQ(WL_ERR_NET_MSGSIZE, wl_net_result_from_errno(EMSGSIZE));
    ASSERT_EQ(WL_ERR_NET_ACCESS, wl_net_result_from_errno(EACCES));
    ASSERT_EQ(WL_ERR_NET_NOTSOCK, wl_net_result_from_errno(ENOTSOCK));
    ASSERT_EQ(WL_ERR_NET_NOTSOCK, wl_net_result_from_errno(EBADF));
    ASSERT_EQ(WL_ERR_ARG, wl_net_result_from_errno(EINVAL));
    ASSERT_EQ(WL_ERR_NOMEM, wl_net_result_from_errno(EMFILE));
    ASSERT_EQ(WL_ERR_FAILED, wl_net_result_from_errno(EIO));
    ASSERT_EQ(EIO, wl_net_last_errno());
    PASS();
}

TEST every_network_result_has_a_name(void) {
    for (WlResult r = WL_ERR_NET_DOWN; r >= WL_ERR_NET_NOTSOCK; r--) {
        ASSERT(strncmp(wl_net_result_name(r), "WL_ERR_NET_", 11) == 0);
    }
    ASSERT_STR_EQ("WL_ERR_AGAIN", wl_net_result_name(WL_ERR_AGAIN));
    ASSERT_STR_EQ("unknown", wl_net_result_name(-12345));
    PASS();
}

TEST the_host_network_is_up(void) {
    ASSERT_EQ(WL_OK, wl_net_start());
    ASSERT_EQ(WL_NET_UP, wl_net_state());
    ASSERT_EQ(WL_OK, wl_net_wait(0));
    ASSERT(wl_net_local_address() != 0u);
    PASS();
}

// ---- UDP ---------------------------------------------------------------------------------------

static WlNetAddr loopback(uint16_t port) {
    WlNetAddr address = {WL_NET_ADDR(127, 0, 0, 1), port};
    return address;
}

// A socket bound to a free loopback port, and the port.
static WlResult bound(WlNetProto proto, WlSocket *socket, uint16_t *port) {
    WlNetAddr address = loopback(0), actual;
    WlResult r = wl_net_socket(proto, socket);

    if (r != WL_OK) {
        return r;
    }
    r = wl_net_bind(*socket, &address);
    if (r == WL_OK) {
        r = wl_net_local_addr(*socket, &actual);
    }
    *port = actual.port;
    return r;
}

TEST udp_datagrams_carry_their_senders(void) {
    WlSocket a, b;
    uint16_t port_a, port_b;
    uint8_t out[8] = {1, 2, 3, 4, 5, 6, 7, 8}, in[16];
    WlNetAddr from, to;

    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &a, &port_a));
    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &b, &port_b));
    ASSERT(port_a != 0 && port_b != 0 && port_a != port_b);
    to = loopback(port_b);
    ASSERT_EQ(8, wl_net_sendto(a, out, 8, &to));
    ASSERT_EQ_FMT(WL_OK, wl_net_set_nonblocking(b, true), "%d");
    // (loopback delivers at once; a short wait keeps the test from racing it)
    WlNetPoll poll_b = {b, WL_NET_POLL_READ, 0};
    ASSERT_EQ(1, wl_net_poll(&poll_b, 1, 1000));
    ASSERT(poll_b.revents & WL_NET_POLL_READ);
    uint32_t available = 0;
    ASSERT_EQ(WL_OK, wl_net_bytes_available(b, &available));
    ASSERT_EQ(8u, available);
    ASSERT_EQ(8, wl_net_recvfrom(b, in, sizeof in, &from));
    ASSERT_MEM_EQ(out, in, 8);
    ASSERT_EQ(WL_NET_ADDR(127, 0, 0, 1), from.address);
    ASSERT_EQ(port_a, from.port);
    // nothing more: a non-blocking receive says to try later
    ASSERT_EQ(WL_ERR_AGAIN, wl_net_recvfrom(b, in, sizeof in, &from));
    ASSERT_EQ(0, wl_net_poll(&poll_b, 1, 0));
    ASSERT_EQ(0u, poll_b.revents);
    // a datagram longer than the buffer is cut, and the sender may be left out
    uint8_t big[32];
    memset(big, 0x5a, sizeof big);
    ASSERT_EQ(32, wl_net_sendto(a, big, 32, &to));
    ASSERT_EQ(1, wl_net_poll(&poll_b, 1, 1000));
    ASSERT_EQ(4, wl_net_recvfrom(b, in, 4, NULL));
    ASSERT_EQ(WL_OK, wl_net_close(a));
    ASSERT_EQ(WL_OK, wl_net_close(b));
    // a closed socket is not a socket
    ASSERT_EQ(WL_ERR_NET_NOTSOCK, wl_net_sendto(a, out, 8, &to));
    PASS();
}

TEST sockets_know_what_they_were_made_as(void) {
    WlSocket udp, tcp, listener, client, server;
    uint16_t port;
    WlNetProto proto = WL_NET_UDP;
    WlNetAddr to;

    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_UDP, &udp));
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &tcp));
    ASSERT_EQ(WL_OK, wl_net_socket_proto(udp, &proto));
    ASSERT_EQ(WL_NET_UDP, proto);
    ASSERT_EQ(WL_OK, wl_net_socket_proto(tcp, &proto));
    ASSERT_EQ(WL_NET_TCP, proto);
    ASSERT_EQ(WL_ERR_ARG, wl_net_socket_proto(tcp, NULL));
    ASSERT_EQ(WL_ERR_NET_NOTSOCK, wl_net_socket_proto(WL_SOCKET_INVALID, &proto));
    ASSERT_EQ(WL_ERR_NET_NOTSOCK, wl_net_socket_proto(5000, &proto));
    ASSERT_EQ(WL_OK, wl_net_close(udp));
    ASSERT_EQ(WL_ERR_NET_NOTSOCK, wl_net_socket_proto(udp, &proto));
    ASSERT_EQ(WL_OK, wl_net_close(tcp));
    // an accepted socket is a TCP one
    ASSERT_EQ(WL_OK, bound(WL_NET_TCP, &listener, &port));
    ASSERT_EQ(WL_OK, wl_net_listen(listener, 1));
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &client));
    to = loopback(port);
    ASSERT_EQ(WL_OK, wl_net_connect(client, &to));
    ASSERT_EQ(WL_OK, wl_net_accept(listener, &server, NULL));
    ASSERT_EQ(WL_OK, wl_net_socket_proto(server, &proto));
    ASSERT_EQ(WL_NET_TCP, proto);
    ASSERT_EQ(WL_OK, wl_net_close(server));
    ASSERT_EQ(WL_OK, wl_net_close(client));
    ASSERT_EQ(WL_OK, wl_net_close(listener));
    PASS();
}

TEST udp_zero_length_and_bad_arguments(void) {
    WlSocket s;
    uint16_t port;
    uint8_t byte = 0;
    WlNetAddr to = loopback(9);

    ASSERT_EQ(WL_ERR_ARG, wl_net_socket((WlNetProto)7, &s));
    ASSERT_EQ(WL_ERR_ARG, wl_net_socket(WL_NET_UDP, NULL));
    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &s, &port));
    ASSERT_EQ(WL_ERR_ARG, wl_net_sendto(s, &byte, 1, NULL));
    ASSERT_EQ(WL_ERR_ARG, wl_net_bind(s, NULL));
    ASSERT_EQ(WL_ERR_ARG, wl_net_local_addr(s, NULL));
    ASSERT_EQ(WL_ERR_ARG, wl_net_shutdown(s, (WlNetShutdown)9));
    ASSERT_EQ(WL_ERR_ARG, wl_net_set_option(s, (WlNetOption)99, 1));
    ASSERT_EQ(0, wl_net_sendto(s, &byte, 0, &to));
    // (a second socket cannot take the first's port)
    WlNetAddr again = loopback(port);
    WlSocket t;
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_UDP, &t));
    ASSERT_EQ(WL_ERR_NET_ADDRINUSE, wl_net_bind(t, &again));
    ASSERT_EQ(WL_OK, wl_net_close(t));
    ASSERT_EQ(WL_OK, wl_net_close(s));
    PASS();
}

TEST broadcast_needs_its_option(void) {
    WlSocket s;
    uint16_t port;
    uint8_t byte = 1;
    WlNetAddr to = {WL_NET_ADDR_BROADCAST, 9};

    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &s, &port));
    ASSERT_EQ(WL_OK, wl_net_set_option(s, WL_NET_OPT_BROADCAST, 1));
    ASSERT_EQ(WL_OK, wl_net_set_option(s, WL_NET_OPT_REUSEADDR, 1));
    ASSERT_EQ(WL_OK, wl_net_set_option(s, WL_NET_OPT_RCVBUF, 65536));
    ASSERT_EQ(WL_OK, wl_net_set_option(s, WL_NET_OPT_SNDBUF, 65536));
    // (a machine with no route for a broadcast answers unreachable; one with it sends the byte)
    int32_t sent = wl_net_sendto(s, &byte, 1, &to);
    ASSERT(sent == 1 || sent == WL_ERR_NET_UNREACH || sent == WL_ERR_NET_DOWN || sent == WL_ERR_NET_ACCESS);
    ASSERT_EQ(WL_OK, wl_net_close(s));
    PASS();
}

// ---- TCP ---------------------------------------------------------------------------------------

TEST tcp_connect_accept_and_stream(void) {
    WlSocket listener, client, server;
    uint16_t port;
    WlNetAddr peer, to;
    uint8_t out[1000], in[1000];

    for (size_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i * 7u + 3u);
    }
    ASSERT_EQ(WL_OK, bound(WL_NET_TCP, &listener, &port));
    ASSERT_EQ(WL_OK, wl_net_listen(listener, 4));
    ASSERT_EQ(WL_OK, wl_net_set_nonblocking(listener, true));
    ASSERT_EQ(WL_ERR_AGAIN, wl_net_accept(listener, &server, &peer)); // none waiting
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &client));
    ASSERT_EQ(WL_OK, wl_net_set_option(client, WL_NET_OPT_NODELAY, 1));
    to = loopback(port);
    ASSERT_EQ(WL_OK, wl_net_connect(client, &to));
    WlNetPoll waiting = {listener, WL_NET_POLL_READ, 0};
    ASSERT_EQ(1, wl_net_poll(&waiting, 1, 1000));
    ASSERT_EQ(WL_OK, wl_net_accept(listener, &server, &peer));
    ASSERT_EQ(WL_NET_ADDR(127, 0, 0, 1), peer.address);
    WlNetAddr client_address;
    ASSERT_EQ(WL_OK, wl_net_local_addr(client, &client_address));
    ASSERT_EQ(client_address.port, peer.port);
    ASSERT_EQ(1000, wl_net_send(client, out, sizeof out));
    size_t got = 0;
    while (got < sizeof in) {
        int32_t n = wl_net_recv(server, in + got, (uint32_t)(sizeof in - got));
        ASSERT(n > 0);
        got += (size_t)n;
    }
    ASSERT_MEM_EQ(out, in, sizeof out);
    // the peer shuts its sending side: the next read is the end of the stream, a poll says hung up
    // or readable, and the other direction still works
    ASSERT_EQ(WL_OK, wl_net_shutdown(client, WL_NET_SHUT_WRITE));
    ASSERT_EQ(0, wl_net_recv(server, in, sizeof in));
    ASSERT_EQ(5, wl_net_send(server, "hello", 5));
    ASSERT_EQ(5, wl_net_recv(client, in, sizeof in));
    ASSERT_MEM_EQ("hello", in, 5);
    ASSERT_EQ(WL_OK, wl_net_close(client));
    ASSERT_EQ(WL_OK, wl_net_close(server));
    ASSERT_EQ(WL_OK, wl_net_close(listener));
    PASS();
}

TEST tcp_nonblocking_connect_completes_and_refusal_is_reported(void) {
    WlSocket listener, client;
    uint16_t port;
    WlNetAddr to;
    WlResult outcome = -1;

    ASSERT_EQ(WL_OK, bound(WL_NET_TCP, &listener, &port));
    ASSERT_EQ(WL_OK, wl_net_listen(listener, 1));
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &client));
    ASSERT_EQ(WL_OK, wl_net_set_nonblocking(client, true));
    to = loopback(port);
    WlResult started = wl_net_connect(client, &to);
    ASSERT(started == WL_OK || started == WL_ERR_NET_INPROGRESS);
    WlNetPoll writable = {client, WL_NET_POLL_WRITE, 0};
    ASSERT_EQ(1, wl_net_poll(&writable, 1, 1000));
    ASSERT(writable.revents & WL_NET_POLL_WRITE);
    ASSERT_EQ(WL_OK, wl_net_connect_result(client, &outcome));
    ASSERT_EQ(WL_OK, outcome);
    ASSERT_EQ(WL_OK, wl_net_close(client));
    ASSERT_EQ(WL_OK, wl_net_close(listener));

    // the same port with nobody listening: refused, either at once or through the connect result
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &client));
    ASSERT_EQ(WL_OK, wl_net_set_nonblocking(client, true));
    started = wl_net_connect(client, &to);
    if (started == WL_ERR_NET_INPROGRESS) {
        writable.socket = client;
        writable.revents = 0;
        ASSERT_EQ(1, wl_net_poll(&writable, 1, 1000));
        ASSERT_EQ(WL_OK, wl_net_connect_result(client, &outcome));
        started = outcome;
    }
    ASSERT_EQ(WL_ERR_NET_REFUSED, started);
    ASSERT_EQ(ECONNREFUSED, wl_net_last_errno());
    ASSERT_EQ(WL_OK, wl_net_close(client));
    PASS();
}

// The way IOS answers (libogc's idiom, which the Wii backend's connect result rests on): connecting
// again says how a non-blocking connect went, pending, connected or failed.
TEST tcp_connecting_again_says_how_a_nonblocking_connect_went(void) {
    WlSocket listener, client;
    uint16_t port;
    WlNetAddr to;

    ASSERT_EQ(WL_OK, bound(WL_NET_TCP, &listener, &port));
    ASSERT_EQ(WL_OK, wl_net_listen(listener, 1));
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &client));
    ASSERT_EQ(WL_OK, wl_net_set_nonblocking(client, true));
    to = loopback(port);
    WlResult r = wl_net_connect(client, &to);
    ASSERT(r == WL_OK || r == WL_ERR_NET_INPROGRESS);
    for (int i = 0; i < 100 && r != WL_OK && r != WL_ERR_NET_ISCONN; i++) {
        ASSERT(r == WL_ERR_NET_INPROGRESS || r == WL_ERR_NET_ALREADY || r == WL_ERR_AGAIN);
        WlNetPoll writable = {client, WL_NET_POLL_WRITE, 0};
        (void)wl_net_poll(&writable, 1, 20);
        r = wl_net_connect(client, &to);
    }
    ASSERT(r == WL_OK || r == WL_ERR_NET_ISCONN);
    ASSERT_EQ(WL_OK, wl_net_close(client));
    ASSERT_EQ(WL_OK, wl_net_close(listener));

    // nobody listening: the repeat says refused (or the first connect did)
    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &client));
    ASSERT_EQ(WL_OK, wl_net_set_nonblocking(client, true));
    r = wl_net_connect(client, &to);
    for (int i = 0; i < 100 && (r == WL_ERR_NET_INPROGRESS || r == WL_ERR_NET_ALREADY); i++) {
        WlNetPoll writable = {client, WL_NET_POLL_WRITE, 0};
        (void)wl_net_poll(&writable, 1, 20);
        r = wl_net_connect(client, &to);
    }
    ASSERT_EQ(WL_ERR_NET_REFUSED, r);
    ASSERT_EQ(WL_OK, wl_net_close(client));
    PASS();
}

TEST tcp_on_an_unconnected_socket_says_so(void) {
    WlSocket s;
    uint8_t byte = 0;

    ASSERT_EQ(WL_OK, wl_net_socket(WL_NET_TCP, &s));
    ASSERT_EQ(WL_ERR_NET_NOTCONN, wl_net_recv(s, &byte, 1));
    ASSERT_EQ(WL_ERR_NET_NOTCONN, wl_net_shutdown(s, WL_NET_SHUT_BOTH));
    int32_t sent = wl_net_send(s, &byte, 1);
    ASSERT(sent == WL_ERR_NET_NOTCONN || sent == WL_ERR_NET_RESET); // (EPIPE on Linux)
    ASSERT_EQ(WL_OK, wl_net_close(s));
    PASS();
}

// ---- poll --------------------------------------------------------------------------------------

TEST poll_reports_only_the_ready_sockets_and_times_out(void) {
    WlSocket a, b, c;
    uint16_t pa, pb, pc;
    WlNetAddr to;
    uint8_t byte = 9;
    WlNetPoll items[3];

    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &a, &pa));
    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &b, &pb));
    ASSERT_EQ(WL_OK, bound(WL_NET_UDP, &c, &pc));
    to = loopback(pb);
    ASSERT_EQ(1, wl_net_sendto(a, &byte, 1, &to));
    items[0] = (WlNetPoll){a, WL_NET_POLL_READ, 99};
    items[1] = (WlNetPoll){b, WL_NET_POLL_READ, 99};
    items[2] = (WlNetPoll){c, WL_NET_POLL_READ | WL_NET_POLL_WRITE, 99};
    // wait for the datagram, then: b readable, c writable only, a neither
    WlNetPoll first = items[1];
    ASSERT_EQ(1, wl_net_poll(&first, 1, 1000));
    ASSERT_EQ(2, wl_net_poll(items, 3, 0));
    ASSERT_EQ(0u, items[0].revents);
    ASSERT_EQ(WL_NET_POLL_READ, items[1].revents);
    ASSERT_EQ(WL_NET_POLL_WRITE, items[2].revents);
    // a timeout takes about as long as asked
    uint8_t in;
    ASSERT_EQ(1, wl_net_recvfrom(b, &in, 1, NULL));
    items[2].events = WL_NET_POLL_READ;
    uint64_t start = wl_platform_time_us();
    ASSERT_EQ(0, wl_net_poll(items, 3, 50));
    uint64_t took = wl_platform_time_us() - start;
    ASSERT(took >= 40000u && took < 2000000u);
    // limits
    ASSERT_EQ(0, wl_net_poll(NULL, 0, 0));
    ASSERT_EQ(WL_ERR_ARG, wl_net_poll(NULL, 1, 0));
    WlNetPoll many[WL_NET_POLL_MAX_SOCKETS + 1];
    memset(many, 0, sizeof many);
    ASSERT_EQ(WL_ERR_LIMIT, wl_net_poll(many, WL_NET_POLL_MAX_SOCKETS + 1, 0));
    ASSERT_EQ(WL_OK, wl_net_close(a));
    ASSERT_EQ(WL_OK, wl_net_close(b));
    ASSERT_EQ(WL_OK, wl_net_close(c));
    PASS();
}

SUITE(net_suite) {
    RUN_TEST(address_text_round_trips);
    RUN_TEST(address_text_refuses_what_is_not_dotted_decimal);
    RUN_TEST(resolve_takes_dotted_addresses_and_names);
    RUN_TEST(errno_values_map_to_results);
    RUN_TEST(every_network_result_has_a_name);
    RUN_TEST(the_host_network_is_up);
    RUN_TEST(udp_datagrams_carry_their_senders);
    RUN_TEST(sockets_know_what_they_were_made_as);
    RUN_TEST(udp_zero_length_and_bad_arguments);
    RUN_TEST(broadcast_needs_its_option);
    RUN_TEST(tcp_connect_accept_and_stream);
    RUN_TEST(tcp_nonblocking_connect_completes_and_refusal_is_reported);
    RUN_TEST(tcp_connecting_again_says_how_a_nonblocking_connect_went);
    RUN_TEST(tcp_on_an_unconnected_socket_says_so);
    RUN_TEST(poll_reports_only_the_ready_sockets_and_times_out);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(net_suite);
    GREATEST_MAIN_END();
}
