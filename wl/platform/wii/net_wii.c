// SPDX-License-Identifier: MIT
// net.h over libogc's network (network.h, which IOS's network module serves). libogc's calls return
// the value, or a negative errno in newlib's numbering (its soc_common.o converts IOS's codes),
// which net_internal.h maps to the WL_ERR_* results. Only this directory may include <ogc/*.h>.
#include <errno.h>
#include <network.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>

#include "wl/platform/net_internal.h"
#include "wl/platform/platform.h"

static atomic_int g_state; // WlNetState

static s32 on_started(s32 result, void *user) {
    (void)user;
    atomic_store(&g_state, result >= 0 ? WL_NET_UP : WL_NET_FAILED);
    return 0;
}

WlResult wl_net_start(void) {
    int expected = WL_NET_IDLE;

    // (from idle, or again after a failure; starting and up are left alone)
    if (!atomic_compare_exchange_strong(&g_state, &expected, WL_NET_STARTING)) {
        if (expected != WL_NET_FAILED || !atomic_compare_exchange_strong(&g_state, &expected, WL_NET_STARTING)) {
            return WL_OK;
        }
    }
    if (net_init_async(on_started, NULL) < 0) {
        atomic_store(&g_state, WL_NET_FAILED);
        return WL_ERR_FAILED;
    }
    return WL_OK;
}

WlNetState wl_net_state(void) {
    return (WlNetState)atomic_load(&g_state);
}

WlResult wl_net_wait(uint32_t timeout_ms) {
    uint64_t start = wl_platform_time_us();

    for (;;) {
        switch (wl_net_state()) {
        case WL_NET_UP: return WL_OK;
        case WL_NET_FAILED: return WL_ERR_NET_DOWN;
        case WL_NET_IDLE: return WL_ERR_STATE;
        default: break;
        }
        if (timeout_ms != UINT32_MAX && wl_platform_time_us() - start >= (uint64_t)timeout_ms * 1000u) {
            return WL_ERR_AGAIN;
        }
        wl_platform_thread_sleep_ms(2);
    }
}

void wl_net_stop(void) {
    if (atomic_exchange(&g_state, WL_NET_IDLE) != WL_NET_IDLE) {
        net_deinit();
    }
}

uint32_t wl_net_local_address(void) {
    // (IOS answers with the address's four bytes in order, which is the big-endian word)
    return wl_net_state() == WL_NET_UP ? (uint32_t)net_gethostip() : 0u;
}

static bool down(void) {
    if (wl_net_state() == WL_NET_UP) {
        return false;
    }
    wl_net_result_from_errno(ENETDOWN);
    return true;
}

static WlResult failed(s32 value) {
    return wl_net_result_from_errno(value < 0 ? -(int)value : (int)value);
}

static void to_system(const WlNetAddr *address, struct sockaddr_in *out) {
    memset(out, 0, sizeof *out);
    out->sin_len = sizeof *out;
    out->sin_family = AF_INET;
    out->sin_port = htons(address->port);
    out->sin_addr.s_addr = htonl(address->address);
}

static void from_system(const struct sockaddr_in *in, WlNetAddr *out) {
    out->address = ntohl(in->sin_addr.s_addr);
    out->port = ntohs(in->sin_port);
}

WlResult wl_net_resolve(const char *host, uint32_t *out) {
    const struct hostent *found;

    if (host == NULL || out == NULL) {
        return WL_ERR_ARG;
    }
    if (wl_net_parse_address(host, out) == WL_OK) {
        return WL_OK;
    }
    if (down()) {
        return WL_ERR_NET_DOWN;
    }
    found = net_gethostbyname(host);
    if (found == NULL || found->h_addrtype != AF_INET || found->h_length != 4 || found->h_addr_list == NULL ||
        found->h_addr_list[0] == NULL) {
        return WL_ERR_NOT_FOUND;
    }
    {
        const uint8_t *bytes = (const uint8_t *)found->h_addr_list[0];

        *out = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
    }
    return WL_OK;
}

WlResult wl_net_socket(WlNetProto proto, WlSocket *out) {
    s32 value;

    if (out == NULL || (proto != WL_NET_UDP && proto != WL_NET_TCP)) {
        return WL_ERR_ARG;
    }
    if (down()) {
        return WL_ERR_NET_DOWN;
    }
    value = net_socket(AF_INET, proto == WL_NET_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (value < 0) {
        return failed(value);
    }
    wl_net_proto_set(value, proto);
    *out = value;
    return WL_OK;
}

WlResult wl_net_close(WlSocket socket) {
    s32 value;

    wl_net_proto_clear(socket);
    value = net_close(socket);

    return value < 0 ? failed(value) : WL_OK;
}

WlResult wl_net_bind(WlSocket socket, const WlNetAddr *address) {
    struct sockaddr_in system;
    s32 value;

    if (address == NULL) {
        return WL_ERR_ARG;
    }
    to_system(address, &system);
    value = net_bind(socket, (struct sockaddr *)&system, sizeof system);
    return value < 0 ? failed(value) : WL_OK;
}

WlResult wl_net_local_addr(WlSocket socket, WlNetAddr *out) {
    struct sockaddr_in system;
    socklen_t length = sizeof system;
    s32 value;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    value = net_getsockname(socket, (struct sockaddr *)&system, &length);
    if (value < 0) {
        return failed(value);
    }
    from_system(&system, out);
    return WL_OK;
}

WlResult wl_net_connect(WlSocket socket, const WlNetAddr *address) {
    struct sockaddr_in system;
    s32 value;

    if (address == NULL) {
        return WL_ERR_ARG;
    }
    to_system(address, &system);
    value = net_connect(socket, (struct sockaddr *)&system, sizeof system);
    return value < 0 ? failed(value) : WL_OK;
}

WlResult wl_net_listen(WlSocket socket, int32_t backlog) {
    s32 value = net_listen(socket, (u32)(backlog < 0 ? 0 : backlog));

    return value < 0 ? failed(value) : WL_OK;
}

WlResult wl_net_accept(WlSocket socket, WlSocket *out, WlNetAddr *peer) {
    struct sockaddr_in system;
    socklen_t length = sizeof system;
    s32 value;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    value = net_accept(socket, (struct sockaddr *)&system, &length);
    if (value < 0) {
        return failed(value);
    }
    if (peer != NULL) {
        from_system(&system, peer);
    }
    wl_net_proto_set(value, WL_NET_TCP);
    *out = value;
    return WL_OK;
}

int32_t wl_net_send(WlSocket socket, const void *data, uint32_t size) {
    s32 value;

    if (size > 0x7fffffffu) {
        return WL_ERR_ARG;
    }
    value = net_send(socket, data, (s32)size, 0);
    return value < 0 ? failed(value) : value;
}

int32_t wl_net_recv(WlSocket socket, void *buffer, uint32_t size) {
    s32 value;

    if (size > 0x7fffffffu) {
        return WL_ERR_ARG;
    }
    value = net_recv(socket, buffer, (s32)size, 0);
    return value < 0 ? failed(value) : value;
}

int32_t wl_net_sendto(WlSocket socket, const void *data, uint32_t size, const WlNetAddr *to) {
    struct sockaddr_in system;
    s32 value;

    if (to == NULL || size > 0x7fffffffu) {
        return WL_ERR_ARG;
    }
    to_system(to, &system);
    value = net_sendto(socket, data, (s32)size, 0, (struct sockaddr *)&system, sizeof system);
    return value < 0 ? failed(value) : value;
}

int32_t wl_net_recvfrom(WlSocket socket, void *buffer, uint32_t size, WlNetAddr *from) {
    struct sockaddr_in system;
    socklen_t length = sizeof system;
    s32 value;

    if (size > 0x7fffffffu) {
        return WL_ERR_ARG;
    }
    value = net_recvfrom(socket, buffer, (s32)size, 0, (struct sockaddr *)&system, &length);
    if (value < 0) {
        return failed(value);
    }
    if (from != NULL) {
        memset(from, 0, sizeof *from);
        // (IOS's sockaddr_in is 8 bytes, which libogc reports as the length; its fields are at
        // lwIP's offsets)
        if (length >= 8) {
            from_system(&system, from);
        }
    }
    return value;
}

WlResult wl_net_shutdown(WlSocket socket, WlNetShutdown how) {
    s32 value;

    if ((unsigned)how > (unsigned)WL_NET_SHUT_BOTH) {
        return WL_ERR_ARG;
    }
    value = net_shutdown(socket, (u32)how); // (0 read, 1 write, 2 both: the same numbers)
    return value < 0 ? failed(value) : WL_OK;
}

WlResult wl_net_set_nonblocking(WlSocket socket, bool nonblocking) {
    u32 on = nonblocking ? 1u : 0u;
    s32 value = net_ioctl(socket, (u32)FIONBIO, &on);

    return value < 0 ? failed(value) : WL_OK;
}

WlResult wl_net_bytes_available(WlSocket socket, uint32_t *out) {
    (void)socket;
    if (out == NULL) {
        return WL_ERR_ARG;
    }
    // libogc's net_ioctl handles FIONBIO only and answers EINVAL to anything else, FIONREAD included
    *out = 0;
    return WL_ERR_UNSUPPORTED;
}

WlResult wl_net_set_option(WlSocket socket, WlNetOption option, int32_t value) {
    u32 level = SOL_SOCKET, name;
    int setting = value;
    s32 result;

    switch (option) {
    case WL_NET_OPT_NODELAY: level = IPPROTO_TCP; name = TCP_NODELAY; setting = value != 0; break;
    case WL_NET_OPT_BROADCAST: name = SO_BROADCAST; setting = value != 0; break;
    case WL_NET_OPT_REUSEADDR: name = SO_REUSEADDR; setting = value != 0; break;
    case WL_NET_OPT_SNDBUF: name = SO_SNDBUF; break;
    case WL_NET_OPT_RCVBUF: name = SO_RCVBUF; break;
    default: return WL_ERR_ARG;
    }
    result = net_setsockopt(socket, level, name, &setting, sizeof setting);
    return result < 0 ? failed(result) : WL_OK;
}

WlResult wl_net_connect_result(WlSocket socket, WlResult *out) {
    int error = 0;
    socklen_t length = sizeof error;
    s32 value;

    if (out == NULL) {
        return WL_ERR_ARG;
    }
    value = net_getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length);
    if (value < 0) {
        return failed(value);
    }
    *out = error == 0 ? WL_OK : wl_net_result_from_errno(error < 0 ? -error : error);
    return WL_OK;
}

int32_t wl_net_poll(WlNetPoll *items, uint32_t count, int32_t timeout_ms) {
    struct pollsd sockets[WL_NET_POLL_MAX_SOCKETS];
    s32 ready;
    int32_t counted = 0;

    if ((items == NULL && count > 0) || count > WL_NET_POLL_MAX_SOCKETS) {
        return count > WL_NET_POLL_MAX_SOCKETS ? WL_ERR_LIMIT : WL_ERR_ARG;
    }
    if (down()) {
        return WL_ERR_NET_DOWN;
    }
    for (uint32_t i = 0; i < count; i++) {
        sockets[i].socket = items[i].socket;
        sockets[i].events = ((items[i].events & WL_NET_POLL_READ) ? POLLIN : 0u) |
                            ((items[i].events & WL_NET_POLL_WRITE) ? POLLOUT : 0u) |
                            ((items[i].events & WL_NET_POLL_PRIORITY) ? POLLPRI : 0u);
        sockets[i].revents = 0;
    }
    ready = net_poll(sockets, (s32)count, timeout_ms < 0 ? -1 : timeout_ms);
    if (ready < 0) {
        return failed(ready);
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t revents = 0;

        revents |= (sockets[i].revents & POLLIN) ? WL_NET_POLL_READ : 0u;
        revents |= (sockets[i].revents & POLLOUT) ? WL_NET_POLL_WRITE : 0u;
        revents |= (sockets[i].revents & POLLPRI) ? WL_NET_POLL_PRIORITY : 0u;
        revents |= (sockets[i].revents & POLLERR) ? WL_NET_POLL_ERROR : 0u;
        revents |= (sockets[i].revents & POLLHUP) ? WL_NET_POLL_HUP : 0u;
        items[i].revents = revents;
        counted += revents != 0;
    }
    return counted;
}
